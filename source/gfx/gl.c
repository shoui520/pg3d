/*
 * GLES2 entry points for libunity via dlsym(libGLESv2.so) and the EGL calls
 * it imports directly. vitaGL provides almost everything; overrides here
 * cover what must look like a plain GLES 2.0 device to Unity.
 */
#include "gfx/gl.h"

#include <string.h>
#include <vitaGL.h>

#include "input/input.h"
#include "util/log.h"
#include "util/perf.h"

#define GL_LOW_INT 0x8DF3
#define GL_HIGH_INT 0x8DF5

#define VGL_RAM_THRESHOLD (32 * 1024 * 1024)
/* Per-frame scratch for client-side vertex data and temporary uploads, one
 * slice per display buffer, taken from PHYCONT. Unity draws from its own
 * buffers: the peak per frame was 0 KB through a match and the armory, so the
 * default 32 MB (whose third slice spilled into VRAM) only kept PHYCONT from
 * Unity's dynamic buffers and textures. Anything past a slice falls back to
 * normal allocations ("Circular pool overrun"). */
#define VGL_CIRCULAR_POOL (3 * 1024 * 1024)

void gfx_init(void) {
  vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
  vglSetCircularPoolSize(VGL_CIRCULAR_POOL);
  vglInitExtended(0, 960, 544, VGL_RAM_THRESHOLD, SCE_GXM_MULTISAMPLE_NONE);
  LOG("[gfx] vitaGL ready: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
}

/* Unity picks its renderer from GL_VERSION; promise ES 2.0 only. */
static const GLubyte *gl_getString(GLenum name) {
  switch (name) {
  case GL_VERSION: return (const GLubyte *)"OpenGL ES 2.0 vitaGL";
  case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
  case GL_VENDOR: return (const GLubyte *)"Sony";
  case GL_RENDERER: return (const GLubyte *)"SGX543MP4+";
  /* what GXM really does natively; no program binaries, no desktop extensions */
  case GL_EXTENSIONS:
    return (const GLubyte *)"GL_OES_texture_npot GL_OES_depth24 GL_OES_packed_depth_stencil "
                            "GL_OES_rgb8_rgba8 GL_OES_element_index_uint GL_OES_vertex_array_object "
                            "GL_OES_compressed_ETC1_RGB8_texture GL_IMG_texture_compression_pvrtc "
                            "GL_EXT_texture_compression_dxt1 GL_EXT_texture_compression_s3tc "
                            "GL_EXT_texture_format_BGRA8888 GL_OES_texture_half_float";
  default: return glGetString(name);
  }
}

/* GLES2 entry points vitaGL lacks; libunity calls them unchecked. */
static void gl_compressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei n, const void *d) {
  LOG("[gfx] glCompressedTexSubImage2D(%dx%d @%d,%d fmt 0x%X) ignored", w, h, x, y, f);
}

static void gl_validateProgram(GLuint p) {
}

static void gl_getShaderPrecisionFormat(GLenum shader, GLenum type, GLint *range, GLint *precision) {
  int is_int = type >= GL_LOW_INT && type <= GL_HIGH_INT;
  range[0] = is_int ? 31 : 127;
  range[1] = is_int ? 30 : 127;
  *precision = is_int ? 0 : 23;
}

static void gl_getTexParameteriv(GLenum t, GLenum pname, GLint *v) {
  *v = 0;
}

static void gl_getUniformiv(GLuint p, GLint loc, GLint *v) {
  *v = 0;
}

static const struct {
  const char *name;
  void *fn;
} overrides[] = {
  { "glGetString", (void *)gl_getString },
  { "glCompressedTexSubImage2D", (void *)gl_compressedTexSubImage2D },
  { "glValidateProgram", (void *)gl_validateProgram },
  { "glGetShaderPrecisionFormat", (void *)gl_getShaderPrecisionFormat },
  { "glGetTexParameteriv", (void *)gl_getTexParameteriv },
  { "glGetUniformiv", (void *)gl_getUniformiv },
};

void *gl_lookup(const char *name) {
  for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++)
    if (strcmp(name, overrides[i].name) == 0)
      return overrides[i].fn;
  void *p = vglGetProcAddress(name);
  if (!p)
    LOG("[gfx] GL function %s unavailable", name);
  return p;
}

/* ---------- EGL bridges (imported by name as b_egl*) ---------- */

EGLBoolean b_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
  static unsigned swaps;
  if (swaps++ < 3 || swaps % 600 == 0)
    LOG("[gfx] eglSwapBuffers #%u", swaps);
  vglSwapBuffers(input_dialog_active() ? GL_TRUE : GL_FALSE);
  perf_frame();
  return EGL_TRUE;
}

/* Unity probes a pbuffer for its offscreen context; vitaGL has a single
 * window surface, so hand that back. */
EGLSurface b_eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *attribs) {
  return eglCreateWindowSurface(d, c, NULL, NULL);
}

EGLBoolean b_eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *value) {
  switch (attr) {
  case EGL_WIDTH: *value = 960; return EGL_TRUE;
  case EGL_HEIGHT: *value = 544; return EGL_TRUE;
  default: return eglQuerySurface(d, s, attr, value);
  }
}

void *b_eglGetProcAddress(const char *name) {
  return gl_lookup(name);
}
