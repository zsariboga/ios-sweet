/*
 * SweetGL probe engine: a stand-in for
 * /System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle/GLEngine.
 *
 * EAGL dlopens GLEngine, dlsyms gliInitializeLibrary, gliTerminateLibrary
 * and 18 more gli* entry points, and gets the GL dispatch table (1036
 * slots at EAGLContextPrivate+0x28) filled by gliCreateContextWithShared.
 * Every public gl* call jumps through that table with x0 = our context.
 * See research/eagl-minimal.md and research/gl-dispatch-slots.txt.
 *
 * This first version is a null driver: it accepts everything, returns
 * plausible values for queries, and logs (through the SweetAccel service,
 * into the kernel log) the first use of every GL entry point.
 *
 * Apple's GLEngine lives in the dyld shared cache, so dyld never loads a file
 * at the original path; a dyld cache patch renames the path EAGL dlopens to
 * GLEngine.bundle/SweetGL, which is this file. Booting with "sweetgl=0" makes
 * gliGetVersion fail so EAGL/CoreAnimation keep the software path.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>

#define SWEETGL_NSLOTS 1036
#define EXPORT __attribute__((visibility("default")))

extern void *const sweetgl_default[SWEETGL_NSLOTS];
extern const char *const sweetgl_slot_name[SWEETGL_NSLOTS];

typedef struct sweetgl_ctx {
	uint32_t magic;
	uint32_t id;            /* context id on the wire (SGL_CONTEXT) */
	uint32_t api_flag;
	void *window;
	uint64_t presents;
} sweetgl_ctx;

static int g_enabled = -1;
static uint32_t g_next_name = 1;
static uint8_t g_seen[SWEETGL_NSLOTS];
static uint64_t g_calls[SWEETGL_NSLOTS];
static void *g_map_buffer;
static uint8_t g_version_blob[256];

/*
 * Log channel: GL clients (backboardd runs as mobile) cannot open
 * /dev/console, so messages go to the SweetAccel IOAcceleratorES service as a
 * "SweetGLLog" property; the kernel (patch 0094) prints them as "SWEETGL ...".
 */
typedef mach_port_t io_object_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_object_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
extern kern_return_t IORegistryEntrySetCFProperty(io_object_t entry, CFStringRef key, CFTypeRef value);

extern kern_return_t IOServiceOpen(io_object_t service, task_port_t owning, uint32_t type, io_object_t *connect);

static io_object_t g_accel;
static kern_return_t g_log_kr;

io_object_t
sweetgl_accel(void);

static io_object_t
accel(void)
{
	if (g_accel == 0) {
		g_accel = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("IOAcceleratorES"));
	}
	return g_accel;
}

/*
 * Progress marker that needs no privilege: SweetAccel refuses every user
 * client but logs "newUserClient type T from <proc>". T = 0x53470000 |
 * stage << 12 | (last SweetGLLog kern_return & 0xfff).
 */
static void
mark(unsigned stage)
{
	io_object_t conn = 0;

	if (accel() != 0) {
		(void)IOServiceOpen(g_accel, mach_task_self(), 0x53470000u | (stage & 0xf) << 12 |
		    ((unsigned)g_log_kr & 0xfff), &conn);
	}
}

__attribute__((constructor)) static void
sweetgl_loaded(void)
{
	mark(1);
}

void
sglog(const char *fmt, ...)
{
	char buf[240];
	int n;
	va_list ap;
	CFStringRef s;

	if (accel() == 0) {
		return;
	}
	n = snprintf(buf, sizeof(buf), "%s[%d]: ", getprogname(), getpid());
	va_start(ap, fmt);
	vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	s = CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
	if (s != NULL) {
		g_log_kr = IORegistryEntrySetCFProperty(g_accel, CFSTR("SweetGLLog"), s);
		CFRelease(s);
	}
}

static int
enabled(void)
{
	char args[1024];
	size_t len = sizeof(args) - 1;

	if (g_enabled >= 0) {
		return g_enabled;
	}
	/* On unless the kernel was booted with "sweetgl=0". The real switch is the dyld
	 * cache patch that points EAGL at this file (tools/dsc-patches-gl.txt). */
	g_enabled = 1;
	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) == 0) {
		args[len] = 0;
		g_enabled = strstr(args, "sweetgl=0") == NULL;
	}
	return g_enabled;
}

static uint32_t
next_name(void)
{
	return __atomic_fetch_add(&g_next_name, 1, __ATOMIC_RELAXED);
}

/* Called by every default slot stub (stubs.c). */
uint64_t
sweetgl_hit(int slot)
{
	__atomic_fetch_add(&g_calls[slot], 1, __ATOMIC_RELAXED);
	if (!__atomic_exchange_n(&g_seen[slot], 1, __ATOMIC_RELAXED)) {
		sglog("first %s (slot %d)", sweetgl_slot_name[slot], slot);
	}
	return 0;
}

int
sweetgl_slot_of(const char *name)
{
	for (int i = 0; i < SWEETGL_NSLOTS; i++) {
		if (strcmp(sweetgl_slot_name[i], name) == 0) {
			return i;
		}
	}
	return -1;
}

#define SEEN(name) do { \
	static int s_slot = -2; \
	if (s_slot == -2) s_slot = sweetgl_slot_of(name); \
	if (s_slot >= 0) sweetgl_hit(s_slot); \
} while (0)

/* ---- GL entry points that must return something sensible ---- */

static uint32_t
gl_GetError(void *c)
{
	(void)c;
	return 0;
}

static const char k_extensions[] =
    "GL_APPLE_fence GL_APPLE_texture_format_BGRA8888 GL_APPLE_rgb_422 "
    "GL_EXT_texture_filter_anisotropic GL_EXT_discard_framebuffer "
    "GL_EXT_texture_rg GL_EXT_color_buffer_half_float "
    "GL_OES_texture_half_float GL_OES_rgb8_rgba8 GL_OES_packed_depth_stencil "
    "GL_OES_vertex_array_object GL_OES_mapbuffer GL_EXT_map_buffer_range";

static const char *
gl_GetString(void *c, uint32_t name)
{
	(void)c;
	SEEN("glGetString");
	switch (name) {
	case 0x1F00: return "Sweet";
	case 0x1F01: return "SweetGL paravirtual";
	case 0x1F02: return "OpenGL ES 3.0 SweetGL";
	case 0x8B8C: return "OpenGL ES GLSL ES 3.00";
	case 0x1F03: return k_extensions;
	default: return "";
	}
}

static const char *
gl_GetStringi(void *c, uint32_t name, uint32_t i)
{
	(void)c; (void)name; (void)i;
	return "";
}

static int32_t
int_param(uint32_t pname)
{
	switch (pname) {
	case 0x0D33: /* MAX_TEXTURE_SIZE */
	case 0x84E8: /* MAX_RENDERBUFFER_SIZE */
	case 0x0D3A: /* MAX_VIEWPORT_DIMS */
	case 0x851C: /* MAX_CUBE_MAP_TEXTURE_SIZE */
		return 8192;
	case 0x8872: return 16;  /* MAX_TEXTURE_IMAGE_UNITS */
	case 0x8B4D: return 32;  /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */
	case 0x8B4C: return 16;  /* MAX_VERTEX_TEXTURE_IMAGE_UNITS */
	case 0x8869: return 16;  /* MAX_VERTEX_ATTRIBS */
	case 0x8DFB: return 256; /* MAX_VERTEX_UNIFORM_VECTORS */
	case 0x8DFD: return 256; /* MAX_FRAGMENT_UNIFORM_VECTORS */
	case 0x8DFC: return 15;  /* MAX_VARYING_VECTORS */
	case 0x821B: return 3;   /* MAJOR_VERSION */
	case 0x821C: return 0;   /* MINOR_VERSION */
	case 0x8D57: return 4;   /* MAX_SAMPLES */
	case 0x8CDF: return 4;   /* MAX_COLOR_ATTACHMENTS */
	case 0x8824: return 4;   /* MAX_DRAW_BUFFERS */
	case 0x0D50: case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55:
		return 8;            /* RED/GREEN/BLUE/ALPHA bits */
	default: return 0;
	}
}

static void
gl_GetIntegerv(void *c, uint32_t pname, int32_t *v)
{
	(void)c;
	SEEN("glGetIntegerv");
	if (v == NULL) {
		return;
	}
	v[0] = int_param(pname);
	if (pname == 0x0D3A) {
		v[1] = 8192;
	}
}

static void
gl_GetInteger64v(void *c, uint32_t pname, int64_t *v)
{
	(void)c;
	if (v != NULL) {
		v[0] = int_param(pname);
	}
}

static void
gl_GetFloatv(void *c, uint32_t pname, float *v)
{
	(void)c;
	if (v == NULL) {
		return;
	}
	v[0] = pname == 0x84FF ? 16.0f : (float)int_param(pname);
}

static void
gl_GetBooleanv(void *c, uint32_t pname, uint8_t *v)
{
	(void)c; (void)pname;
	if (v != NULL) {
		v[0] = 0;
	}
}

static void
gl_Gen(void *c, int32_t n, uint32_t *ids)
{
	(void)c;
	for (int32_t i = 0; ids != NULL && i < n; i++) {
		ids[i] = next_name();
	}
}

static uint32_t
gl_Create(void *c)
{
	(void)c;
	return next_name();
}

static uint32_t
gl_CreateShader(void *c, uint32_t type)
{
	(void)c; (void)type;
	SEEN("glCreateShader");
	return next_name();
}

static void
gl_GetObjectiv(void *c, uint32_t obj, uint32_t pname, int32_t *v)
{
	(void)c; (void)obj;
	if (v == NULL) {
		return;
	}
	switch (pname) {
	case 0x8B81: case 0x8B82: case 0x8B83: /* COMPILE/LINK/VALIDATE_STATUS */
		v[0] = 1;
		break;
	default:
		v[0] = 0;
	}
}

static void
gl_GetInfoLog(void *c, uint32_t obj, int32_t size, int32_t *len, char *log)
{
	(void)c; (void)obj;
	if (len != NULL) {
		*len = 0;
	}
	if (log != NULL && size > 0) {
		log[0] = 0;
	}
}

static int32_t
gl_GetLocation(void *c, uint32_t prog, const char *name)
{
	static int32_t loc;
	(void)c; (void)prog; (void)name;
	return __atomic_fetch_add(&loc, 1, __ATOMIC_RELAXED) & 63;
}

static uint32_t
gl_GetUniformBlockIndex(void *c, uint32_t prog, const char *name)
{
	(void)c; (void)prog; (void)name;
	return 0;
}

static uint32_t
gl_CheckFramebufferStatus(void *c, uint32_t target)
{
	(void)c; (void)target;
	SEEN("glCheckFramebufferStatus");
	return 0x8CD5; /* FRAMEBUFFER_COMPLETE */
}

static void *
gl_FenceSync(void *c, uint32_t cond, uint32_t flags)
{
	(void)c; (void)cond; (void)flags;
	SEEN("glFenceSync");
	return (void *)(uintptr_t)(next_name() << 4);
}

static uint32_t
gl_ClientWaitSync(void *c, void *sync, uint32_t flags, uint64_t timeout)
{
	(void)c; (void)sync; (void)flags; (void)timeout;
	return 0x911A; /* ALREADY_SIGNALED */
}

static uint8_t
gl_True(void *c)
{
	(void)c;
	return 1;
}

static void
gl_GetSynciv(void *c, void *sync, uint32_t pname, int32_t size, int32_t *len, int32_t *v)
{
	(void)c; (void)sync;
	if (len != NULL) {
		*len = 1;
	}
	if (v != NULL && size > 0) {
		v[0] = pname == 0x9114 ? 0x9119 : 0; /* SYNC_STATUS -> SIGNALED */
	}
}

static void *
gl_MapBuffer(void *c)
{
	(void)c;
	SEEN("glMapBufferRange");
	if (g_map_buffer == NULL) {
		g_map_buffer = calloc(1, 32 << 20);
	}
	return g_map_buffer;
}

static uint8_t
gl_UnmapBuffer(void *c, uint32_t target)
{
	(void)c; (void)target;
	return 1;
}

static void
gl_GetShaderPrecisionFormat(void *c, uint32_t st, uint32_t pt, int32_t *range, int32_t *prec)
{
	(void)c; (void)st; (void)pt;
	if (range != NULL) {
		range[0] = 127;
		range[1] = 127;
	}
	if (prec != NULL) {
		*prec = 23;
	}
}

static void
gl_GetRenderbufferParameteriv(void *c, uint32_t target, uint32_t pname, int32_t *v)
{
	(void)c; (void)target;
	if (v == NULL) {
		return;
	}
	switch (pname) {
	case 0x8D42: v[0] = 1125; break; /* WIDTH */
	case 0x8D43: v[0] = 2436; break; /* HEIGHT */
	default: v[0] = 0;
	}
}

static void
gl_GetParam3(void *c, uint32_t a, uint32_t b, int32_t *v)
{
	(void)c; (void)a; (void)b;
	if (v != NULL) {
		v[0] = 0;
	}
}

static void
gl_GetFramebufferAttachmentParameteriv(void *c, uint32_t t, uint32_t a, uint32_t p, int32_t *v)
{
	(void)c; (void)t; (void)a; (void)p;
	if (v != NULL) {
		v[0] = 0;
	}
}

static void
gl_GetInternalformativ(void *c, uint32_t t, uint32_t f, uint32_t p, int32_t size, int32_t *v)
{
	(void)c; (void)t; (void)f; (void)p;
	if (v != NULL && size > 0) {
		v[0] = 0;
	}
}

static const struct {
	const char *name;
	void *fn;
} k_special[] = {
	{ "glGetError", (void *)gl_GetError },
	{ "glGetString", (void *)gl_GetString },
	{ "glGetStringi", (void *)gl_GetStringi },
	{ "glGetIntegerv", (void *)gl_GetIntegerv },
	{ "glGetInteger64v", (void *)gl_GetInteger64v },
	{ "glGetFloatv", (void *)gl_GetFloatv },
	{ "glGetBooleanv", (void *)gl_GetBooleanv },
	{ "glGenTextures", (void *)gl_Gen },
	{ "glGenBuffers", (void *)gl_Gen },
	{ "glGenFramebuffers", (void *)gl_Gen },
	{ "glGenRenderbuffers", (void *)gl_Gen },
	{ "glGenVertexArrays", (void *)gl_Gen },
	{ "glGenQueries", (void *)gl_Gen },
	{ "glGenSamplers", (void *)gl_Gen },
	{ "glGenTransformFeedbacks", (void *)gl_Gen },
	{ "glGenFencesAPPLE", (void *)gl_Gen },
	{ "glGenProgramPipelinesEXT", (void *)gl_Gen },
	{ "glCreateProgram", (void *)gl_Create },
	{ "glCreateShader", (void *)gl_CreateShader },
	{ "glGetShaderiv", (void *)gl_GetObjectiv },
	{ "glGetProgramiv", (void *)gl_GetObjectiv },
	{ "glGetShaderInfoLog", (void *)gl_GetInfoLog },
	{ "glGetProgramInfoLog", (void *)gl_GetInfoLog },
	{ "glGetUniformLocation", (void *)gl_GetLocation },
	{ "glGetAttribLocation", (void *)gl_GetLocation },
	{ "glGetUniformBlockIndex", (void *)gl_GetUniformBlockIndex },
	{ "glCheckFramebufferStatus", (void *)gl_CheckFramebufferStatus },
	{ "glFenceSync", (void *)gl_FenceSync },
	{ "glClientWaitSync", (void *)gl_ClientWaitSync },
	{ "glIsSync", (void *)gl_True },
	{ "glGetSynciv", (void *)gl_GetSynciv },
	{ "glIsFenceAPPLE", (void *)gl_True },
	{ "glTestFenceAPPLE", (void *)gl_True },
	{ "glMapBufferRange", (void *)gl_MapBuffer },
	{ "glMapBufferOES", (void *)gl_MapBuffer },
	{ "glUnmapBuffer", (void *)gl_UnmapBuffer },
	{ "glGetShaderPrecisionFormat", (void *)gl_GetShaderPrecisionFormat },
	{ "glGetRenderbufferParameteriv", (void *)gl_GetRenderbufferParameteriv },
	{ "glGetBufferParameteriv", (void *)gl_GetParam3 },
	{ "glGetTexParameteriv", (void *)gl_GetParam3 },
	{ "glGetFramebufferAttachmentParameteriv", (void *)gl_GetFramebufferAttachmentParameteriv },
	{ "glGetInternalformativ", (void *)gl_GetInternalformativ },
};

/* encoder.c */
extern void sweetgl_encoder_fill(void **disp);
extern void sweetgl_tex_iosurface(void *ctx, const int32_t *v);
extern void sweetgl_delete_context(uint32_t id);

io_object_t
sweetgl_accel(void)
{
	return accel();
}

uint32_t
sweetgl_context_id(void *ctx)
{
	return ctx != NULL ? ((sweetgl_ctx *)ctx)->id : 0;
}

static void
fill_dispatch(void **disp)
{
	memcpy(disp, sweetgl_default, sizeof(void *) * SWEETGL_NSLOTS);
	for (size_t i = 0; i < sizeof(k_special) / sizeof(k_special[0]); i++) {
		int s = sweetgl_slot_of(k_special[i].name);
		if (s >= 0) {
			disp[s] = k_special[i].fn;
		}
	}
	sweetgl_encoder_fill(disp);
}

/* ---- GLI interface used by EAGL ---- */

EXPORT int
gliInitializeLibrary(void *services, void *out, int count, int a3, uint32_t flags)
{
	mark(2);
	sglog("gliInitializeLibrary services %p count %d flags 0x%x enabled %d",
	    services, count, flags, enabled());
	(void)out; (void)a3;
	return 1;
}

EXPORT int
gliTerminateLibrary(void)
{
	return 0;
}

EXPORT int
gliGetVersion(uint32_t *a, void **p, uint32_t *c)
{
	mark(enabled() ? 3 : 4);
	if (!enabled()) {
		return 0;
	}
	if (a != NULL) {
		*a = 1;
	}
	if (p != NULL) {
		*p = g_version_blob;
	}
	if (c != NULL) {
		*c = 0;
	}
	return 1;
}

EXPORT int
gliChoosePixelFormat(void **out, const void *opts)
{
	uint8_t *pf;

	(void)opts;
	if (out == NULL || !enabled()) {
		return 10000;
	}
	pf = calloc(1, 0x100);
	if (pf == NULL) {
		return 10000;
	}
	*(uint32_t *)(pf + 0x8) = 0x00010100;  /* driver ID */
	*(uint32_t *)(pf + 0xc) = 0x00020100;  /* usable + ES3 */
	*out = pf;
	mark(5);
	sglog("gliChoosePixelFormat -> %p", pf);
	return 0;
}

EXPORT int
gliDestroyPixelFormat(void *pf)
{
	free(pf);
	return 0;
}

EXPORT int
gliQueryRendererInfo(void)
{
	sglog("gliQueryRendererInfo");
	return 10000;
}

EXPORT int
gliDestroyRendererInfo(void *ri)
{
	(void)ri;
	return 0;
}

EXPORT int
gliCreateContext(void)
{
	sglog("gliCreateContext (unsupported)");
	return 10000;
}

EXPORT int
gliCreateContextWithShared(void **out, void *pf, void *shared, void **disp,
    void **saved, uint32_t api_flag)
{
	sweetgl_ctx *ctx;

	if (out == NULL || disp == NULL) {
		return 10000;
	}
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return 10000;
	}
	ctx->magic = 0x53474c31; /* SGL1 */
	ctx->id = next_name();
	ctx->api_flag = api_flag;
	fill_dispatch(disp);
	if (saved != NULL) {
		fill_dispatch(saved);
	}
	*out = ctx;
	mark(6);
	sglog("gliCreateContextWithShared ctx %p pf %p shared %p api 0x%x",
	    ctx, pf, shared, api_flag);
	return 0;
}

EXPORT int
gliDestroyContext(void *ctx)
{
	if (ctx != NULL) {
		sweetgl_delete_context(((sweetgl_ctx *)ctx)->id);
	}
	sglog("gliDestroyContext %p", ctx);
	free(ctx);
	return 0;
}

EXPORT int
gliAttachDrawable(void *ctx, int type, void *drawable)
{
	sglog("gliAttachDrawable %p type %d %p", ctx, type, drawable);
	return 0;
}

EXPORT int
gliAttachDrawableWithOptions(void *ctx, int type, void *drawable, void *opts)
{
	sglog("gliAttachDrawableWithOptions %p type %d %p %p", ctx, type, drawable, opts);
	return 0;
}

EXPORT int
gliSetInteger(void *ctx, uint32_t pname, const int32_t *v)
{
	if (pname == 0x38e && v != NULL) {
		sweetgl_tex_iosurface(ctx, v);
	}
	sglog("gliSetInteger 0x%x = %d", pname, v != NULL ? v[0] : -1);
	(void)ctx;
	return 0;
}

EXPORT int
gliGetInteger(void *ctx, uint32_t pname, int32_t *v)
{
	(void)ctx;
	sglog("gliGetInteger 0x%x", pname);
	if (v != NULL) {
		v[0] = 0;
	}
	return 0;
}

EXPORT int
gliGetAttribute(void *ctx, uint32_t pname, void *v)
{
	(void)ctx; (void)v;
	sglog("gliGetAttribute 0x%x", pname);
	return 0;
}

EXPORT int
gliSetAttribute(void *ctx, uint32_t pname, void *v)
{
	(void)ctx; (void)v;
	sglog("gliSetAttribute 0x%x", pname);
	return 0;
}

EXPORT int
gliCopyAttributes(void *src, void *dst, uint32_t mask)
{
	(void)src; (void)dst; (void)mask;
	return 0;
}

EXPORT int
gliGetMemoryAnnotationList(void)
{
	return 10000;
}

EXPORT int
gliBindViewES(void *ctx, void *window, int retained, uint32_t format)
{
	sweetgl_ctx *c = ctx;

	sglog("gliBindViewES ctx %p window %p retained %d format 0x%x",
	    ctx, window, retained, format);
	if (c != NULL) {
		c->window = window;
	}
	mark(window != NULL ? 7 : 8);
	return window != NULL;
}

EXPORT int
gliPresentViewES(void *ctx, int flags, double when)
{
	sweetgl_ctx *c = ctx;
	uint64_t n;

	(void)flags; (void)when;
	if (c == NULL) {
		return 0;
	}
	n = ++c->presents;
	if (n == 1) {
		mark(9);
	}
	if (n == 1 || n % 300 == 0) {
		uint64_t total = 0;
		int busiest = 0;
		for (int i = 0; i < SWEETGL_NSLOTS; i++) {
			total += g_calls[i];
			if (g_calls[i] > g_calls[busiest]) {
				busiest = i;
			}
		}
		sglog("present #%llu ctx %p, %llu GL calls so far, top %s x%llu",
		    (unsigned long long)n, ctx, (unsigned long long)total,
		    sweetgl_slot_name[busiest], (unsigned long long)g_calls[busiest]);
	}
	return 0;
}
