#ifndef PG3D_GL_H
#define PG3D_GL_H

/* Initializes vitaGL; everything else in the process must already hold its
 * memory (vitaGL takes the remaining RAM minus the threshold). */
void gfx_init(void);
/* libGLESv2/libEGL dlsym: our overrides first, then vitaGL. */
void *gl_lookup(const char *name);

#endif
