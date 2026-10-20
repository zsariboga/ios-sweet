/*
 * SweetGL loader: what EAGL dlopens as OpenGLES.framework/GLEngine.bundle/SweetGL.
 *
 * It holds no GL code. Every gli* entry point is a trampoline into an engine
 * (glengine.c + encoder.c) loaded at startup from, in order:
 *   1. the engine blob the host viewer wrote into guest RAM (kernel 0099,
 *      SweetAccel user client type 'SGLB'), copied to /private/var/tmp and
 *      dlopened;
 *   2. GLEngine.bundle/SweetGLEngine on the root image.
 * A thread watches the blob; when the host publishes a new generation the
 * process exits and launchd restarts it with the new engine, so testing an
 * engine build needs no reboot and no root image change.
 *
 * Blob layout (host writes data, then size, then gen, then magic):
 *   +0 magic 'SGLE'  +4 gen  +8 size (bytes of the dylib)  +0x1000 dylib
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>

typedef mach_port_t io_object_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_object_t IOServiceGetMatchingService(mach_port_t master, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_object_t service, task_port_t owning, uint32_t type, io_object_t *connect);
extern kern_return_t IOConnectMapMemory64(io_object_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);
extern kern_return_t IORegistryEntrySetCFProperty(io_object_t entry, CFStringRef key, CFTypeRef value);

#define BLOB_TYPE       0x53474c42u     /* 'SGLB' */
#define BLOB_MAGIC      0x454c4753u     /* 'SGLE' */
#define BLOB_DATA       0x1000
#define DISK_ENGINE     "/System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle/SweetGLEngine"

#define GLI_FUNCS(X) \
	X(0, gliInitializeLibrary) X(1, gliTerminateLibrary) X(2, gliGetVersion) \
	X(3, gliChoosePixelFormat) X(4, gliDestroyPixelFormat) X(5, gliQueryRendererInfo) \
	X(6, gliDestroyRendererInfo) X(7, gliCreateContext) X(8, gliCreateContextWithShared) \
	X(9, gliDestroyContext) X(10, gliAttachDrawable) X(11, gliAttachDrawableWithOptions) \
	X(12, gliSetInteger) X(13, gliGetInteger) X(14, gliGetAttribute) X(15, gliSetAttribute) \
	X(16, gliCopyAttributes) X(17, gliGetMemoryAnnotationList) X(18, gliBindViewES) \
	X(19, gliPresentViewES)
#define NFUNCS 20

static int
gli_unavailable(void)
{
	return 0;
}

/* Read by the trampolines below; one engine function per entry, never NULL after load. */
__attribute__((visibility("hidden"))) void *sweetgl_fn[NFUNCS];

#define NAME_(i, name) #name,
static const char *k_names[NFUNCS] = { GLI_FUNCS(NAME_) };

/* x0-x7 and the stack are untouched: jump straight into the engine. */
#define TRAMP_(i, name) \
	__attribute__((naked, visibility("default"))) void name(void) \
	{ \
		__asm__ volatile ( \
		    "adrp x16, _sweetgl_fn@PAGE\n" \
		    "add x16, x16, _sweetgl_fn@PAGEOFF\n" \
		    "ldr x16, [x16, #(" #i " * 8)]\n" \
		    "br x16\n"); \
	}
GLI_FUNCS(TRAMP_)

static io_object_t g_accel;
static volatile const uint8_t *g_blob;
static mach_vm_size_t g_blob_size;
static uint32_t g_gen;

static void
lg(const char *fmt, ...)
{
	char buf[240];
	int n;
	va_list ap;
	CFStringRef s;

	if (g_accel == 0) {
		return;
	}
	n = snprintf(buf, sizeof(buf), "%s[%d]: loader: ", getprogname(), getpid());
	va_start(ap, fmt);
	vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	s = CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
	if (s != NULL) {
		(void)IORegistryEntrySetCFProperty(g_accel, CFSTR("SweetGLLog"), s);
		CFRelease(s);
	}
}

static void
map_blob(void)
{
	io_object_t conn = 0;
	mach_vm_address_t addr = 0;
	mach_vm_size_t size = 0;

	g_accel = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("IOAcceleratorES"));
	if (g_accel == 0 || IOServiceOpen(g_accel, mach_task_self(), BLOB_TYPE, &conn) != KERN_SUCCESS) {
		return;
	}
	if (IOConnectMapMemory64(conn, 0, mach_task_self(), &addr, &size, 1 /* kIOMapAnywhere */) == KERN_SUCCESS &&
	    size > BLOB_DATA) {
		g_blob = (volatile const uint8_t *)addr;
		g_blob_size = size;
	}
}


#define BLOB_U32(off) (*(volatile const uint32_t *)(g_blob + (off)))
#define BLOB_U64(off) (*(volatile const uint64_t *)(g_blob + (off)))

/* The blob's engine as a file (written once per generation); NULL if there is none. */
static const char *
blob_engine(char *path, size_t len)
{
	uint32_t gen;
	uint64_t size;
	int fd;

	if (g_blob == NULL || BLOB_U32(0) != BLOB_MAGIC) {
		return NULL;
	}
	gen = BLOB_U32(4);
	size = BLOB_U64(8);
	if (gen == 0 || size == 0 || size > g_blob_size - BLOB_DATA) {
		return NULL;
	}
	snprintf(path, len, "/private/var/tmp/sweetgl-engine-%u-%d.dylib", gen, getpid());
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0) {
		lg("cannot write %s", path);
		return NULL;
	}
	if (write(fd, (const void *)(g_blob + BLOB_DATA), (size_t)size) != (ssize_t)size) {
		close(fd);
		unlink(path);
		lg("short write %s", path);
		return NULL;
	}
	close(fd);
	if (BLOB_U32(4) != gen) {      /* the host replaced it meanwhile: the watcher restarts us */
		unlink(path);
		return NULL;
	}
	g_gen = gen;
	return path;
}

static void *
watcher(void *arg)
{
	(void)arg;
	for (;;) {
		sleep(2);
		if (BLOB_U32(0) == BLOB_MAGIC && BLOB_U32(4) != 0 && BLOB_U32(4) != g_gen) {
			lg("engine generation %u -> %u: restarting", g_gen, BLOB_U32(4));
			_exit(0);
		}
	}
	return NULL;
}

__attribute__((constructor)) static void
sweetgl_loader(void)
{
	char path[128];
	const char *from;
	void *h = NULL;
	pthread_t t;
	int missing = 0;

	map_blob();
	from = blob_engine(path, sizeof(path));
	if (from != NULL) {
		h = dlopen(from, RTLD_NOW | RTLD_LOCAL);
		unlink(from);           /* mapped now; nothing to clean up later */
		if (h == NULL) {
			lg("blob engine gen %u: %s", g_gen, dlerror());
			g_gen = 0;
		}
	}
	if (h == NULL) {
		from = DISK_ENGINE;
		h = dlopen(from, RTLD_NOW | RTLD_LOCAL);
		if (h == NULL) {
			lg("disk engine: %s", dlerror());
		}
	}
	for (int i = 0; i < NFUNCS; i++) {
		sweetgl_fn[i] = h != NULL ? dlsym(h, k_names[i]) : NULL;
		if (sweetgl_fn[i] == NULL) {
			sweetgl_fn[i] = (void *)gli_unavailable;
			missing++;
		}
	}
	lg("engine %s (gen %u), %d/%d entry points", h != NULL ? (g_gen != 0 ? "from host blob" : from) : "none",
	    g_gen, NFUNCS - missing, NFUNCS);
	if (g_blob != NULL && pthread_create(&t, NULL, watcher, NULL) == 0) {
		pthread_detach(t);
	}
}
