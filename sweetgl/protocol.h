/*
 * SweetGL wire protocol: guest GL engine (glengine.c) -> host viewer
 * (iOS16Viewer.cs, SweetGLHost). Keep the numbers in sync with the C# side.
 *
 * The ring is a physically contiguous buffer owned by the SweetAccel kernel
 * service (patch 0095); the host finds it through the kernel's sweet_gl
 * table and reads/writes it directly in guest RAM.
 *
 * Ring layout:
 *   0x000 u32 magic 'SGLQ' (kernel)   0x004 u32 version
 *   0x008 u64 size (whole ring)
 *   0x010 u64 wpos   guest: bytes written to the data area (monotonic)
 *   0x018 u64 rpos   host:  bytes consumed (monotonic)
 *   0x020 u32 host_beat  host increments while it is polling
 *   0x024 u32 guest_pid
 *   0x028 u64 dropped    commands the guest dropped (host not alive)
 *   0x030 u32 host_epoch host bumps it when it (re)starts and loses state
 *   0x1000 data area (size - 0x1000 bytes), circular
 *
 * Commands are 8-byte aligned: { u16 op; u16 flags; u32 len } + payload,
 * len including the header. A command never wraps; the guest pads the tail
 * of the data area with SGL_PAD instead.
 */
#ifndef SWEETGL_PROTOCOL_H
#define SWEETGL_PROTOCOL_H

#define SGL_RING_TYPE           0x53474c52u     /* IOServiceOpen type 'SGLR' */
#define SGL_RING_MAGIC          0x51474c53u     /* 'SGLQ' */
#define SGL_HDR_SIZE            0x1000
#define SGL_OFF_WPOS            0x10
#define SGL_OFF_RPOS            0x18
#define SGL_OFF_BEAT            0x20
#define SGL_OFF_PID             0x24
#define SGL_OFF_DROPPED         0x28
#define SGL_OFF_EPOCH           0x30

enum sgl_op {
	SGL_PAD = 1,
	SGL_CONTEXT,            /* u32 guest context id */
	SGL_ACTIVE_TEXTURE,     /* u32 unit */
	SGL_ATTACH_SHADER,      /* u32 prog, shader */
	SGL_BIND_ATTRIB_LOCATION, /* u32 prog, index, then NUL-terminated name */
	SGL_BIND_FRAMEBUFFER,   /* u32 target, fb */
	SGL_BIND_TEXTURE,       /* u32 target, tex */
	SGL_BLEND_FUNC,         /* u32 srcRGB, dstRGB, srcA, dstA */
	SGL_BLEND_EQUATION,     /* u32 modeRGB, modeA */
	SGL_CLEAR,              /* u32 mask */
	SGL_CLEAR_COLOR,        /* f32 r, g, b, a */
	SGL_COLOR_MASK,         /* u32 r, g, b, a */
	SGL_COMPILE_SHADER,     /* u32 shader */
	SGL_CREATE_SHADER,      /* u32 shader, type */
	SGL_CREATE_PROGRAM,     /* u32 prog */
	SGL_DELETE_FRAMEBUFFERS, /* u32 n, names[n] */
	SGL_DELETE_PROGRAM,     /* u32 prog */
	SGL_DELETE_SHADER,      /* u32 shader */
	SGL_DELETE_TEXTURES,    /* u32 n, names[n] */
	SGL_DISABLE,            /* u32 cap */
	SGL_ENABLE,             /* u32 cap */
	SGL_VERTEX_ATTRIB_ARRAY, /* u32 index, enabled */
	SGL_DRAW,               /* see struct sgl_draw */
	SGL_FRAMEBUFFER_TEXTURE2D, /* u32 target, attachment, textarget, tex, level */
	SGL_GENERATE_MIPMAP,    /* u32 target */
	SGL_LINK_PROGRAM,       /* u32 prog */
	SGL_PIXEL_STOREI,       /* u32 pname, i32 value */
	SGL_SCISSOR,            /* i32 x, y, w, h */
	SGL_SHADER_SOURCE,      /* u32 shader, then NUL-terminated source */
	SGL_TEX_IMAGE2D,        /* see struct sgl_tex_image */
	SGL_TEX_SUB_IMAGE2D,    /* see struct sgl_tex_image (xoff/yoff used) */
	SGL_TEX_PARAMETERF,     /* u32 target, pname, f32 value */
	SGL_TEX_PARAMETERI,     /* u32 target, pname, i32 value */
	SGL_UNIFORM,            /* u32 kind, i32 loc, u32 count, u32 transpose, data */
	SGL_UNIFORM_LOCATION,   /* u32 prog, i32 guest loc, then NUL-terminated name */
	SGL_USE_PROGRAM,        /* u32 prog */
	SGL_VIEWPORT,           /* i32 x, y, w, h */
	SGL_TEX_IOSURFACE,      /* see struct sgl_tex_iosurface */
	SGL_FLUSH,              /* nothing */
	SGL_DELETE_CONTEXT,     /* u32 guest context id */
	SGL_OP_MAX
};

/* SGL_UNIFORM kinds */
enum sgl_uniform_kind {
	SGL_U1F = 1, SGL_U2F, SGL_U3F, SGL_U4F,
	SGL_U1I, SGL_U2I, SGL_U3I, SGL_U4I,
	SGL_UMAT2, SGL_UMAT3, SGL_UMAT4,
};

struct sgl_cmd {
	unsigned short op;
	unsigned short flags;
	unsigned int len;
};

/* SGL_DRAW: one per glDrawArrays/glDrawElements, carrying the client arrays. */
struct sgl_draw {
	unsigned int mode;
	unsigned int first;             /* DrawArrays */
	unsigned int count;
	unsigned int index_type;        /* 0 = DrawArrays, else GL_UNSIGNED_BYTE/SHORT/INT */
	unsigned int index_bytes;       /* index data length (follows the attribs table) */
	unsigned int nattribs;
	/* nattribs x struct sgl_attrib, then index data, then attrib data, each 8-aligned */
};

struct sgl_attrib {
	unsigned int index;
	unsigned int size;              /* components */
	unsigned int type;
	unsigned int normalized;
	unsigned int stride;            /* effective stride (never 0) */
	unsigned int bytes;             /* data length, covering vertex 0 .. max */
	unsigned int offset;            /* data offset from the start of the command payload */
	unsigned int pad;
};

struct sgl_tex_image {
	unsigned int target, level, internal_format;
	int xoff, yoff;
	unsigned int width, height, format, type;
	unsigned int row_bytes;         /* bytes per row in the data */
	unsigned int bytes;             /* data length, 0 = no data */
	unsigned int pad;
	/* data */
};

struct sgl_tex_iosurface {
	unsigned int target, internal_format, width, height, format, type, plane, invert;
	unsigned int surface_id;
	unsigned int desc_bytes;        /* kernel surface description (SweetAccel selector 0) */
	/* description: 12 u32 header + nseg (pa, len) u64 pairs */
};

#endif
