#ifndef PG3D_INPUT_H
#define PG3D_INPUT_H

#include <psp2/ctrl.h>

void input_init(void);
/* Called once per frame on UnityMain before nativeRender. */
void input_poll(void);
/* Latest pad sample from input_poll (neutral while a dialog is open). */
const SceCtrlData *input_pad(void);

void input_show_keyboard(const char *text, int type, int autocorrect, int multiline, int secure, int alert,
                         const char *placeholder);
void input_hide_keyboard(void);
/* nonzero while a system dialog needs vglSwapBuffers(GL_TRUE) */
int input_dialog_active(void);

/* unity.c */
int unity_inject(void *event);
void unity_soft_input_done(const char *text, int canceled);

#endif
