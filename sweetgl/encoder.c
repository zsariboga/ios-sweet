/*
 * SweetGL encoder: the GL entry points CoreAnimation uses, serialized into
 * the SweetAccel command ring (protocol.h) for the host viewer to execute
 * on the host GPU. Queries are still answered locally (glengine.c): object
 * names, uniform locations and fences are allocated here, and the host maps
 * them to its own.
 *
 * If the ring cannot be opened, or the host stops polling it (its
 * heartbeat does not move for 2 s), commands are dropped and counted; GL
 * calls never block for long.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <mach/mach.h>
#include <os/lock.h>
#include "protocol.h"

typedef mach_port_t io_object_t;
extern kern_return_t IOServiceOpen(io_object_t service, task_port_t owning, uint32_t type, io_object_t *connect);
extern kern_return_t IOConnectMapMemory64(io_object_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);
extern kern_return_t IOConnectCallMethod(io_object_t connection, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt, const void *inputStruct, size_t inputStructCnt,
    uint64_t *output, uint32_t *outputCnt, void *outputStruct, size_t *outputStructCnt);

/* glengine.c */
extern void sglog(const char *fmt, ...);
extern int sweetgl_slot_of(const char *name);
extern io_object_t sweetgl_accel(void);
extern uint32_t sweetgl_context_id(void *ctx);

#define MAX_ATTRIBS     16
#define DESC_MAX        65536

static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;
static io_object_t g_conn;
static volatile uint8_t *g_ring;
static uint64_t g_data_size;            /* data area bytes */
static uint64_t g_wpos;                 /* our copy of the ring's wpos */
static uint64_t g_cur;                  /* wpos after the command being built */
static uint64_t g_open_after_ns;        /* next ring_open attempt (retries every 3 s) */
static uint64_t g_cmds, g_stat_ns;
static uint32_t g_last_beat;
static uint64_t g_beat_seen_ns;
static uint64_t g_dropped;
static uint32_t g_cur_ctx;

static struct {
	int enabled;
	uint32_t size, type, normalized, stride;
	const uint8_t *ptr;
} g_attr[MAX_ATTRIBS];

static struct {
	int32_t alignment, row_length, skip_rows, skip_pixels;
} g_unpack = { 4, 0, 0, 0 };

static uint8_t g_desc[DESC_MAX];

static uint64_t
now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

#define RING_U64(off) (*(volatile uint64_t *)(g_ring + (off)))
#define RING_U32(off) (*(volatile uint32_t *)(g_ring + (off)))

static int
ring_open(void)
{
	mach_vm_address_t addr = 0;
	mach_vm_size_t size = 0;
	kern_return_t kr;

	if (g_ring != NULL) {
		return 1;
	}
	if (now_ns() < g_open_after_ns) {
		return 0;
	}
	g_open_after_ns = now_ns() + 3000000000ull;
	if (sweetgl_accel() == 0) {
		sglog("ring: no IOAcceleratorES service");
		return 0;
	}
	kr = IOServiceOpen(sweetgl_accel(), mach_task_self(), SGL_RING_TYPE, &g_conn);
	if (kr != KERN_SUCCESS) {
		sglog("ring: IOServiceOpen 0x%x", kr);
		return 0;
	}
	kr = IOConnectMapMemory64(g_conn, 0, mach_task_self(), &addr, &size, 1 /* kIOMapAnywhere */);
	if (kr != KERN_SUCCESS || size <= SGL_HDR_SIZE * 2 || *(volatile uint32_t *)addr != SGL_RING_MAGIC) {
		sglog("ring: map 0x%x size %llu", kr, (unsigned long long)size);
		return 0;
	}
	g_ring = (volatile uint8_t *)addr;
	g_data_size = (size - SGL_HDR_SIZE) & ~7ull;
	g_wpos = RING_U64(SGL_OFF_WPOS);
	RING_U32(SGL_OFF_PID) = (uint32_t)getpid();
	g_last_beat = RING_U32(SGL_OFF_BEAT);
	g_beat_seen_ns = now_ns();
	sglog("ring: %llu MB at %p, host beat %u", (unsigned long long)(size >> 20), (void *)addr, g_last_beat);
	return 1;
}

/* Is the host consuming? A beat that has not moved for 2 s means no. */
static int
host_alive(void)
{
	uint32_t beat = RING_U32(SGL_OFF_BEAT);
	uint64_t t = now_ns();

	if (beat != g_last_beat) {
		g_last_beat = beat;
		g_beat_seen_ns = t;
		return 1;
	}
	return beat != 0 && t - g_beat_seen_ns < 2000000000ull;
}

static int
wait_space(uint64_t need)
{
	for (;;) {
		uint64_t rpos = RING_U64(SGL_OFF_RPOS);
		if (g_data_size - (g_wpos - rpos) >= need) {
			return 1;
		}
		if (!host_alive()) {
			return 0;
		}
		usleep(50);
	}
}

/* Start a command with a payload of n bytes; returns the payload or NULL (dropped). */
static uint8_t *
cmd_begin(uint16_t op, uint32_t n)
{
	uint64_t len = (sizeof(struct sgl_cmd) + (uint64_t)n + 7) & ~7ull;
	uint64_t p, pad;
	struct sgl_cmd *c;

	if (!ring_open() || len > g_data_size / 2 || !host_alive()) {
		g_dropped++;
		if (g_ring != NULL) {
			RING_U64(SGL_OFF_DROPPED) = g_dropped;
		}
		return NULL;
	}
	p = g_wpos % g_data_size;
	pad = p + len > g_data_size ? g_data_size - p : 0;
	if (!wait_space(pad + len)) {
		g_dropped++;
		RING_U64(SGL_OFF_DROPPED) = g_dropped;
		return NULL;
	}
	if (pad != 0) {
		c = (struct sgl_cmd *)(g_ring + SGL_HDR_SIZE + p);
		c->op = SGL_PAD;
		c->flags = 0;
		c->len = (uint32_t)pad;
		g_wpos += pad;
		p = 0;
	}
	c = (struct sgl_cmd *)(g_ring + SGL_HDR_SIZE + p);
	c->op = op;
	c->flags = 0;
	c->len = (uint32_t)len;
	g_cur = g_wpos + len;
	return (uint8_t *)(c + 1);
}

static void
cmd_end(void)
{
	__atomic_thread_fence(__ATOMIC_RELEASE);
	g_wpos = g_cur;
	RING_U64(SGL_OFF_WPOS) = g_wpos;
	g_cmds++;
}

/* Once every 10 s: ring position, host progress and drops. */
static void
stats(void)
{
	uint64_t t = now_ns();

	if (t - g_stat_ns < 10000000000ull) {
		return;
	}
	g_stat_ns = t;
	if (g_ring == NULL) {
		sglog("stats: no ring, %llu dropped", (unsigned long long)g_dropped);
		return;
	}
	sglog("stats: %llu cmds, wpos %llu rpos %llu, beat %u, %llu dropped", (unsigned long long)g_cmds,
	    (unsigned long long)g_wpos, (unsigned long long)RING_U64(SGL_OFF_RPOS), RING_U32(SGL_OFF_BEAT),
	    (unsigned long long)g_dropped);
}

static void
cmd_u32s(uint16_t op, int n, const uint32_t *v)
{
	uint8_t *p = cmd_begin(op, (uint32_t)n * 4);
	if (p != NULL) {
		memcpy(p, v, (size_t)n * 4);
		cmd_end();
	}
}

/* u32 values followed by a NUL-terminated string. */
static void
cmd_u32s_str(uint16_t op, int n, const uint32_t *v, const char *s, size_t slen)
{
	uint8_t *p = cmd_begin(op, (uint32_t)(n * 4 + slen + 1));
	if (p != NULL) {
		memcpy(p, v, (size_t)n * 4);
		memcpy(p + n * 4, s, slen);
		p[n * 4 + slen] = 0;
		cmd_end();
	}
}

/* Every entry point: take the lock and note a context switch. */
static void
enter(void *ctx)
{
	uint32_t id = sweetgl_context_id(ctx);

	os_unfair_lock_lock(&g_lock);
	stats();
	if (id != g_cur_ctx) {
		g_cur_ctx = id;
		cmd_u32s(SGL_CONTEXT, 1, &id);
	}
}

static void
leave(void)
{
	os_unfair_lock_unlock(&g_lock);
}

#define U32CMD(op, ...) do { \
	uint32_t v_[] = { __VA_ARGS__ }; \
	cmd_u32s(op, (int)(sizeof(v_) / 4), v_); \
} while (0)

static uint32_t
fbits(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

/* ---- simple state ---- */

static void e_ActiveTexture(void *c, uint32_t u) { enter(c); U32CMD(SGL_ACTIVE_TEXTURE, u); leave(); }
static void e_AttachShader(void *c, uint32_t p, uint32_t s) { enter(c); U32CMD(SGL_ATTACH_SHADER, p, s); leave(); }
static void e_DetachShader(void *c, uint32_t p, uint32_t s) { (void)c; (void)p; (void)s; }
static void e_BindFramebuffer(void *c, uint32_t t, uint32_t f) { enter(c); U32CMD(SGL_BIND_FRAMEBUFFER, t, f); leave(); }
static void e_BindTexture(void *c, uint32_t t, uint32_t x) { enter(c); U32CMD(SGL_BIND_TEXTURE, t, x); leave(); }
static void e_BlendFunc(void *c, uint32_t s, uint32_t d) { enter(c); U32CMD(SGL_BLEND_FUNC, s, d, s, d); leave(); }
static void e_BlendFuncSeparate(void *c, uint32_t s, uint32_t d, uint32_t sa, uint32_t da) { enter(c); U32CMD(SGL_BLEND_FUNC, s, d, sa, da); leave(); }
static void e_BlendEquation(void *c, uint32_t m) { enter(c); U32CMD(SGL_BLEND_EQUATION, m, m); leave(); }
static void e_BlendEquationSeparate(void *c, uint32_t m, uint32_t a) { enter(c); U32CMD(SGL_BLEND_EQUATION, m, a); leave(); }
static void e_Clear(void *c, uint32_t m) { enter(c); U32CMD(SGL_CLEAR, m); leave(); }
static void e_ClearColor(void *c, float r, float g, float b, float a) { enter(c); U32CMD(SGL_CLEAR_COLOR, fbits(r), fbits(g), fbits(b), fbits(a)); leave(); }
static void e_ColorMask(void *c, uint8_t r, uint8_t g, uint8_t b, uint8_t a) { enter(c); U32CMD(SGL_COLOR_MASK, r, g, b, a); leave(); }
static void e_CompileShader(void *c, uint32_t s) { enter(c); U32CMD(SGL_COMPILE_SHADER, s); leave(); }
static void e_DeleteProgram(void *c, uint32_t p) { enter(c); U32CMD(SGL_DELETE_PROGRAM, p); leave(); }
static void e_DeleteShader(void *c, uint32_t s) { enter(c); U32CMD(SGL_DELETE_SHADER, s); leave(); }
static void e_Disable(void *c, uint32_t x) { enter(c); U32CMD(SGL_DISABLE, x); leave(); }
static void e_Enable(void *c, uint32_t x) { enter(c); U32CMD(SGL_ENABLE, x); leave(); }
static void e_GenerateMipmap(void *c, uint32_t t) { enter(c); U32CMD(SGL_GENERATE_MIPMAP, t); leave(); }
static void e_LinkProgram(void *c, uint32_t p) { enter(c); U32CMD(SGL_LINK_PROGRAM, p); leave(); }
static void e_Scissor(void *c, int32_t x, int32_t y, int32_t w, int32_t h) { enter(c); U32CMD(SGL_SCISSOR, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h); leave(); }
static void e_Viewport(void *c, int32_t x, int32_t y, int32_t w, int32_t h) { enter(c); U32CMD(SGL_VIEWPORT, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h); leave(); }
static void e_TexParameteri(void *c, uint32_t t, uint32_t p, int32_t v) { enter(c); U32CMD(SGL_TEX_PARAMETERI, t, p, (uint32_t)v); leave(); }
static void e_TexParameterf(void *c, uint32_t t, uint32_t p, float v) { enter(c); U32CMD(SGL_TEX_PARAMETERF, t, p, fbits(v)); leave(); }
static void e_UseProgram(void *c, uint32_t p) { enter(c); U32CMD(SGL_USE_PROGRAM, p); leave(); }
static void e_Flush(void *c) { enter(c); cmd_u32s(SGL_FLUSH, 0, NULL); leave(); }
static void e_Nop(void *c) { (void)c; }

static void
e_FramebufferTexture2D(void *c, uint32_t t, uint32_t a, uint32_t tt, uint32_t tex, int32_t lvl)
{
	enter(c);
	U32CMD(SGL_FRAMEBUFFER_TEXTURE2D, t, a, tt, tex, (uint32_t)lvl);
	leave();
}

static void
e_Names(void *c, uint16_t op, int32_t n, const uint32_t *names)
{
	uint8_t *p;

	if (n <= 0 || names == NULL) {
		return;
	}
	enter(c);
	p = cmd_begin(op, 4 + (uint32_t)n * 4);
	if (p != NULL) {
		uint32_t un = (uint32_t)n;
		memcpy(p, &un, 4);
		memcpy(p + 4, names, (size_t)n * 4);
		cmd_end();
	}
	leave();
}

static void e_DeleteTextures(void *c, int32_t n, const uint32_t *t) { e_Names(c, SGL_DELETE_TEXTURES, n, t); }
static void e_DeleteFramebuffers(void *c, int32_t n, const uint32_t *f) { e_Names(c, SGL_DELETE_FRAMEBUFFERS, n, f); }

static void
e_PixelStorei(void *c, uint32_t pname, int32_t v)
{
	(void)c;
	/* Unpack state is applied here: uploads are sent tightly packed. */
	switch (pname) {
	case 0x0CF5: g_unpack.alignment = v; break;     /* UNPACK_ALIGNMENT */
	case 0x0CF2: g_unpack.row_length = v; break;    /* UNPACK_ROW_LENGTH */
	case 0x0CF3: g_unpack.skip_rows = v; break;     /* UNPACK_SKIP_ROWS */
	case 0x0CF4: g_unpack.skip_pixels = v; break;   /* UNPACK_SKIP_PIXELS */
	default: break;
	}
}

/* ---- shaders and programs ---- */

static uint32_t g_names = 0x10000;     /* separate from glengine.c's object names */

static uint32_t
e_CreateShader(void *c, uint32_t type)
{
	uint32_t id;

	enter(c);
	id = ++g_names;
	U32CMD(SGL_CREATE_SHADER, id, type);
	leave();
	return id;
}

static uint32_t
e_CreateProgram(void *c)
{
	uint32_t id;

	enter(c);
	id = ++g_names;
	U32CMD(SGL_CREATE_PROGRAM, id);
	leave();
	return id;
}

static void
e_ShaderSource(void *c, uint32_t shader, int32_t count, const char *const *str, const int32_t *lens)
{
	size_t total = 0;
	uint8_t *p;

	if (count <= 0 || str == NULL) {
		return;
	}
	for (int32_t i = 0; i < count; i++) {
		total += (lens != NULL && lens[i] >= 0) ? (size_t)lens[i] : strlen(str[i]);
	}
	enter(c);
	p = cmd_begin(SGL_SHADER_SOURCE, (uint32_t)(4 + total + 1));
	if (p != NULL) {
		size_t o = 4;
		memcpy(p, &shader, 4);
		for (int32_t i = 0; i < count; i++) {
			size_t l = (lens != NULL && lens[i] >= 0) ? (size_t)lens[i] : strlen(str[i]);
			memcpy(p + o, str[i], l);
			o += l;
		}
		p[o] = 0;
		cmd_end();
	}
	leave();
}

static void
e_BindAttribLocation(void *c, uint32_t prog, uint32_t index, const char *name)
{
	uint32_t v[2] = { prog, index };

	if (name == NULL) {
		return;
	}
	enter(c);
	cmd_u32s_str(SGL_BIND_ATTRIB_LOCATION, 2, v, name, strlen(name));
	leave();
}

/* Uniform locations: per program, handed out in order and mapped by the host by name. */
#define LOC_SLOTS 4096
static struct { uint32_t prog; int32_t next; } g_locs[LOC_SLOTS];

static int32_t
e_GetUniformLocation(void *c, uint32_t prog, const char *name)
{
	uint32_t h = (prog * 2654435761u) % LOC_SLOTS;
	int32_t loc;

	if (name == NULL) {
		return -1;
	}
	enter(c);
	for (int i = 0; i < LOC_SLOTS; i++, h = (h + 1) % LOC_SLOTS) {
		if (g_locs[h].prog == prog || g_locs[h].prog == 0) {
			break;
		}
	}
	g_locs[h].prog = prog;
	loc = g_locs[h].next++;
	{
		uint32_t v[2] = { prog, (uint32_t)loc };
		cmd_u32s_str(SGL_UNIFORM_LOCATION, 2, v, name, strlen(name));
	}
	leave();
	return loc;
}

static void
uniform(void *c, uint32_t kind, int32_t loc, int32_t count, uint32_t transpose, const void *data, uint32_t elem_bytes)
{
	uint8_t *p;
	uint32_t bytes;

	if (loc < 0 || count <= 0 || data == NULL) {
		return;
	}
	bytes = (uint32_t)count * elem_bytes;
	enter(c);
	p = cmd_begin(SGL_UNIFORM, 16 + bytes);
	if (p != NULL) {
		uint32_t h[4] = { kind, (uint32_t)loc, (uint32_t)count, transpose };
		memcpy(p, h, 16);
		memcpy(p + 16, data, bytes);
		cmd_end();
	}
	leave();
}

static void e_Uniform1f(void *c, int32_t l, float x) { uniform(c, SGL_U1F, l, 1, 0, &x, 4); }
static void e_Uniform2f(void *c, int32_t l, float x, float y) { float v[2] = { x, y }; uniform(c, SGL_U2F, l, 1, 0, v, 8); }
static void e_Uniform3f(void *c, int32_t l, float x, float y, float z) { float v[3] = { x, y, z }; uniform(c, SGL_U3F, l, 1, 0, v, 12); }
static void e_Uniform4f(void *c, int32_t l, float x, float y, float z, float w) { float v[4] = { x, y, z, w }; uniform(c, SGL_U4F, l, 1, 0, v, 16); }
static void e_Uniform1i(void *c, int32_t l, int32_t x) { uniform(c, SGL_U1I, l, 1, 0, &x, 4); }
static void e_Uniform2i(void *c, int32_t l, int32_t x, int32_t y) { int32_t v[2] = { x, y }; uniform(c, SGL_U2I, l, 1, 0, v, 8); }
static void e_Uniform1fv(void *c, int32_t l, int32_t n, const float *v) { uniform(c, SGL_U1F, l, n, 0, v, 4); }
static void e_Uniform2fv(void *c, int32_t l, int32_t n, const float *v) { uniform(c, SGL_U2F, l, n, 0, v, 8); }
static void e_Uniform3fv(void *c, int32_t l, int32_t n, const float *v) { uniform(c, SGL_U3F, l, n, 0, v, 12); }
static void e_Uniform4fv(void *c, int32_t l, int32_t n, const float *v) { uniform(c, SGL_U4F, l, n, 0, v, 16); }
static void e_Uniform1iv(void *c, int32_t l, int32_t n, const int32_t *v) { uniform(c, SGL_U1I, l, n, 0, v, 4); }
static void e_UniformMatrix2fv(void *c, int32_t l, int32_t n, uint8_t t, const float *v) { uniform(c, SGL_UMAT2, l, n, t, v, 16); }
static void e_UniformMatrix3fv(void *c, int32_t l, int32_t n, uint8_t t, const float *v) { uniform(c, SGL_UMAT3, l, n, t, v, 36); }
static void e_UniformMatrix4fv(void *c, int32_t l, int32_t n, uint8_t t, const float *v) { uniform(c, SGL_UMAT4, l, n, t, v, 64); }

/* ---- vertex arrays and draws ---- */

static uint32_t
type_size(uint32_t type)
{
	switch (type) {
	case 0x1400: case 0x1401: return 1;                     /* BYTE, UNSIGNED_BYTE */
	case 0x1402: case 0x1403: case 0x140B: case 0x8D61: return 2; /* SHORT, USHORT, HALF_FLOAT(_OES) */
	default: return 4;                                      /* INT, UINT, FLOAT, FIXED */
	}
}

static void
e_VertexAttribPointer(void *c, uint32_t i, int32_t size, uint32_t type, uint8_t norm, int32_t stride, const void *ptr)
{
	(void)c;
	if (i >= MAX_ATTRIBS) {
		return;
	}
	os_unfair_lock_lock(&g_lock);
	g_attr[i].size = (uint32_t)size;
	g_attr[i].type = type;
	g_attr[i].normalized = norm;
	g_attr[i].stride = stride > 0 ? (uint32_t)stride : (uint32_t)size * type_size(type);
	g_attr[i].ptr = ptr;
	os_unfair_lock_unlock(&g_lock);
}

static void
e_EnableVertexAttribArray(void *c, uint32_t i)
{
	(void)c;
	if (i < MAX_ATTRIBS) {
		g_attr[i].enabled = 1;
	}
}

static void
e_DisableVertexAttribArray(void *c, uint32_t i)
{
	(void)c;
	if (i < MAX_ATTRIBS) {
		g_attr[i].enabled = 0;
	}
}

static void
draw(void *c, uint32_t mode, uint32_t first, uint32_t count, uint32_t itype, const void *indices)
{
	struct sgl_attrib at[MAX_ATTRIBS];
	uint32_t na = 0, last, ibytes = 0, payload;
	uint8_t *p;

	if (count == 0) {
		return;
	}
	if (itype != 0) {
		uint32_t m = 0;
		if (indices == NULL) {
			return;
		}
		for (uint32_t k = 0; k < count; k++) {
			uint32_t x = itype == 0x1401 ? ((const uint8_t *)indices)[k] :
			    itype == 0x1403 ? ((const uint16_t *)indices)[k] : ((const uint32_t *)indices)[k];
			if (x > m) {
				m = x;
			}
		}
		last = m;
		ibytes = count * type_size(itype);
	} else {
		last = first + count - 1;
	}
	enter(c);
	payload = sizeof(struct sgl_draw);
	for (uint32_t i = 0; i < MAX_ATTRIBS; i++) {
		if (g_attr[i].enabled && g_attr[i].ptr != NULL) {
			at[na].index = i;
			at[na].size = g_attr[i].size;
			at[na].type = g_attr[i].type;
			at[na].normalized = g_attr[i].normalized;
			at[na].stride = g_attr[i].stride;
			at[na].bytes = last * g_attr[i].stride + g_attr[i].size * type_size(g_attr[i].type);
			at[na].pad = 0;
			na++;
		}
	}
	payload += na * (uint32_t)sizeof(struct sgl_attrib);
	payload += (ibytes + 7) & ~7u;
	for (uint32_t k = 0; k < na; k++) {
		at[k].offset = payload;
		payload += (at[k].bytes + 7) & ~7u;
	}
	p = cmd_begin(SGL_DRAW, payload);
	if (p != NULL) {
		struct sgl_draw d = { mode, first, count, itype, ibytes, na };
		uint32_t o = sizeof(d);
		memcpy(p, &d, sizeof(d));
		memcpy(p + o, at, na * sizeof(struct sgl_attrib));
		o += na * (uint32_t)sizeof(struct sgl_attrib);
		if (ibytes != 0) {
			memcpy(p + o, indices, ibytes);
		}
		for (uint32_t k = 0; k < na; k++) {
			memcpy(p + at[k].offset, g_attr[at[k].index].ptr, at[k].bytes);
		}
		cmd_end();
	}
	leave();
}

static void
e_DrawArrays(void *c, uint32_t mode, int32_t first, int32_t count)
{
	if (first >= 0 && count > 0) {
		draw(c, mode, (uint32_t)first, (uint32_t)count, 0, NULL);
	}
}

static void
e_DrawElements(void *c, uint32_t mode, int32_t count, uint32_t type, const void *indices)
{
	if (count > 0) {
		draw(c, mode, 0, (uint32_t)count, type, indices);
	}
}

/* ---- textures ---- */

static uint32_t
pixel_bytes(uint32_t format, uint32_t type)
{
	uint32_t comps;

	switch (type) {
	case 0x8033: case 0x8034: case 0x8363: return 2;                 /* 4444, 5551, 565 */
	case 0x8368: case 0x84FA: case 0x8C3B: case 0x8C3E: return 4;    /* 2_10_10_10, 24_8, 10F_11F_11F, 5_9_9_9 */
	default: break;
	}
	switch (format) {
	case 0x1908: case 0x80E1: case 0x8D99: comps = 4; break;         /* RGBA, BGRA, RGBA_INTEGER */
	case 0x1907: comps = 3; break;                                   /* RGB */
	case 0x8227: case 0x190A: comps = 2; break;                      /* RG, LUMINANCE_ALPHA */
	default: comps = 1; break;                                       /* RED, ALPHA, LUMINANCE, DEPTH */
	}
	return comps * type_size(type);
}

static void
tex_image(void *c, uint16_t op, uint32_t target, int32_t level, uint32_t ifmt, int32_t xoff, int32_t yoff,
    int32_t w, int32_t h, uint32_t format, uint32_t type, const void *pixels)
{
	uint32_t bpp = pixel_bytes(format, type), row = 0, bytes = 0;
	uint8_t *p;

	if (w < 0 || h < 0) {
		return;
	}
	if (pixels != NULL && w > 0 && h > 0) {
		row = (uint32_t)w * bpp;
		bytes = row * (uint32_t)h;
	}
	enter(c);
	p = cmd_begin(op, (uint32_t)sizeof(struct sgl_tex_image) + bytes);
	if (p != NULL) {
		struct sgl_tex_image t = { target, (uint32_t)level, ifmt, xoff, yoff, (uint32_t)w, (uint32_t)h,
		    format, type, row, bytes, 0 };
		memcpy(p, &t, sizeof(t));
		if (bytes != 0) {
			uint32_t len = (g_unpack.row_length > 0 ? (uint32_t)g_unpack.row_length : (uint32_t)w) * bpp;
			uint32_t al = g_unpack.alignment > 0 ? (uint32_t)g_unpack.alignment : 1;
			uint32_t src_row = (len + al - 1) / al * al;
			const uint8_t *src = (const uint8_t *)pixels + (size_t)g_unpack.skip_rows * src_row +
			    (size_t)g_unpack.skip_pixels * bpp;
			uint8_t *dst = p + sizeof(t);
			if (src_row == row) {
				memcpy(dst, src, bytes);
			} else {
				for (int32_t y = 0; y < h; y++) {
					memcpy(dst + (size_t)y * row, src + (size_t)y * src_row, row);
				}
			}
		}
		cmd_end();
	}
	leave();
}

static void
e_TexImage2D(void *c, uint32_t target, int32_t level, int32_t ifmt, int32_t w, int32_t h, int32_t border,
    uint32_t format, uint32_t type, const void *pixels)
{
	(void)border;
	tex_image(c, SGL_TEX_IMAGE2D, target, level, (uint32_t)ifmt, 0, 0, w, h, format, type, pixels);
}

static void
e_TexSubImage2D(void *c, uint32_t target, int32_t level, int32_t x, int32_t y, int32_t w, int32_t h,
    uint32_t format, uint32_t type, const void *pixels)
{
	tex_image(c, SGL_TEX_SUB_IMAGE2D, target, level, 0, x, y, w, h, format, type, pixels);
}

/*
 * -[EAGLContext texImageIOSurface:...] arrives as gliSetInteger(ctx, 0x38e, v):
 * v = { surface id, target, internalFormat, width, height, format, type, plane }.
 * The kernel describes the surface (physical segments) for the host.
 */
void
sweetgl_tex_iosurface(void *c, const int32_t *v)
{
	uint64_t in = (uint32_t)v[0];
	size_t dlen = sizeof(g_desc);
	kern_return_t kr;
	uint8_t *p;

	enter(c);
	if (!ring_open()) {
		leave();
		return;
	}
	kr = IOConnectCallMethod(g_conn, 0, &in, 1, NULL, 0, NULL, NULL, g_desc, &dlen);
	if (kr != KERN_SUCCESS) {
		dlen = 0;
	}
	p = cmd_begin(SGL_TEX_IOSURFACE, (uint32_t)(sizeof(struct sgl_tex_iosurface) + dlen));
	if (p != NULL) {
		struct sgl_tex_iosurface t = { (uint32_t)v[1], (uint32_t)v[2], (uint32_t)v[3], (uint32_t)v[4],
		    (uint32_t)v[5], (uint32_t)v[6], (uint32_t)v[7], 0, (uint32_t)v[0], (uint32_t)dlen };
		memcpy(p, &t, sizeof(t));
		memcpy(p + sizeof(t), g_desc, dlen);
		cmd_end();
	}
	leave();
}

void
sweetgl_delete_context(uint32_t id)
{
	os_unfair_lock_lock(&g_lock);
	cmd_u32s(SGL_DELETE_CONTEXT, 1, &id);
	if (g_cur_ctx == id) {
		g_cur_ctx = 0;
	}
	os_unfair_lock_unlock(&g_lock);
}

static const struct {
	const char *name;
	void *fn;
} k_encoders[] = {
	{ "glActiveTexture", (void *)e_ActiveTexture },
	{ "glAttachShader", (void *)e_AttachShader },
	{ "glDetachShader", (void *)e_DetachShader },
	{ "glBindAttribLocation", (void *)e_BindAttribLocation },
	{ "glBindFramebuffer", (void *)e_BindFramebuffer },
	{ "glBindTexture", (void *)e_BindTexture },
	{ "glBlendFunc", (void *)e_BlendFunc },
	{ "glBlendFuncSeparate", (void *)e_BlendFuncSeparate },
	{ "glBlendEquation", (void *)e_BlendEquation },
	{ "glBlendEquationSeparate", (void *)e_BlendEquationSeparate },
	{ "glClear", (void *)e_Clear },
	{ "glClearColor", (void *)e_ClearColor },
	{ "glColorMask", (void *)e_ColorMask },
	{ "glCompileShader", (void *)e_CompileShader },
	{ "glCreateShader", (void *)e_CreateShader },
	{ "glCreateProgram", (void *)e_CreateProgram },
	{ "glDeleteFramebuffers", (void *)e_DeleteFramebuffers },
	{ "glDeleteProgram", (void *)e_DeleteProgram },
	{ "glDeleteShader", (void *)e_DeleteShader },
	{ "glDeleteTextures", (void *)e_DeleteTextures },
	{ "glDisable", (void *)e_Disable },
	{ "glEnable", (void *)e_Enable },
	{ "glDisableVertexAttribArray", (void *)e_DisableVertexAttribArray },
	{ "glEnableVertexAttribArray", (void *)e_EnableVertexAttribArray },
	{ "glDrawArrays", (void *)e_DrawArrays },
	{ "glDrawElements", (void *)e_DrawElements },
	{ "glFramebufferTexture2D", (void *)e_FramebufferTexture2D },
	{ "glGenerateMipmap", (void *)e_GenerateMipmap },
	{ "glLinkProgram", (void *)e_LinkProgram },
	{ "glPixelStorei", (void *)e_PixelStorei },
	{ "glScissor", (void *)e_Scissor },
	{ "glShaderSource", (void *)e_ShaderSource },
	{ "glTexImage2D", (void *)e_TexImage2D },
	{ "glTexSubImage2D", (void *)e_TexSubImage2D },
	{ "glTexParameterf", (void *)e_TexParameterf },
	{ "glTexParameteri", (void *)e_TexParameteri },
	{ "glUniform1f", (void *)e_Uniform1f },
	{ "glUniform2f", (void *)e_Uniform2f },
	{ "glUniform3f", (void *)e_Uniform3f },
	{ "glUniform4f", (void *)e_Uniform4f },
	{ "glUniform1i", (void *)e_Uniform1i },
	{ "glUniform2i", (void *)e_Uniform2i },
	{ "glUniform1fv", (void *)e_Uniform1fv },
	{ "glUniform2fv", (void *)e_Uniform2fv },
	{ "glUniform3fv", (void *)e_Uniform3fv },
	{ "glUniform4fv", (void *)e_Uniform4fv },
	{ "glUniform1iv", (void *)e_Uniform1iv },
	{ "glUniformMatrix2fv", (void *)e_UniformMatrix2fv },
	{ "glUniformMatrix3fv", (void *)e_UniformMatrix3fv },
	{ "glUniformMatrix4fv", (void *)e_UniformMatrix4fv },
	{ "glGetUniformLocation", (void *)e_GetUniformLocation },
	{ "glUseProgram", (void *)e_UseProgram },
	{ "glVertexAttribPointer", (void *)e_VertexAttribPointer },
	{ "glViewport", (void *)e_Viewport },
	{ "glFlush", (void *)e_Flush },
	{ "glFinish", (void *)e_Flush },
	{ "glLabelObjectEXT", (void *)e_Nop },
	{ "glFinishObjectAPPLE", (void *)e_Nop },
	{ "glDeleteSync", (void *)e_Nop },
};

/* Point the dispatch slots of the encoded entry points at the encoders. */
void
sweetgl_encoder_fill(void **disp)
{
	for (size_t i = 0; i < sizeof(k_encoders) / sizeof(k_encoders[0]); i++) {
		int s = sweetgl_slot_of(k_encoders[i].name);
		if (s >= 0) {
			disp[s] = k_encoders[i].fn;
		}
	}
}
