/*
 * Vita input -> Android input events delivered through nativeInjectEvent.
 * Front touch becomes touchscreen MotionEvents with Android's pointer
 * bookkeeping (DOWN / POINTER_DOWN / MOVE / POINTER_UP / UP); buttons become
 * gamepad KeyEvents, with START sent as BACK. The latest pad sample is also
 * kept for the in-game controls (game/pad.c). Soft keyboard requests open
 * the system IME dialog.
 */
#include "input/input.h"

#include <stdlib.h>
#include <string.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/clib.h>
#include <psp2/touch.h>

#include "android/ndk.h"
#include "bionic/bionic.h"
#include "jni/jvm.h"
#include "util/log.h"

#define MAX_POINTERS 6

#define ACTION_DOWN 0
#define ACTION_UP 1
#define ACTION_MOVE 2
#define ACTION_POINTER_DOWN 5
#define ACTION_POINTER_UP 6
#define SOURCE_TOUCHSCREEN 0x1002
#define SOURCE_GAMEPAD 0x401
#define SOURCE_KEYBOARD 0x101

typedef struct {
  int action;
  int count;
  int id[MAX_POINTERS];
  float x[MAX_POINTERS], y[MAX_POINTERS];
  int64_t time_ms;
  int source;
} motion;

typedef struct {
  int action, keycode, meta, repeat, source;
  int64_t time_ms;
} key;

static motion *M(jobj *o) { return (motion *)o->native; }
static key *K(jobj *o) { return (key *)o->native; }

static int idx(const jvalue *args, int n, int i) {
  return n > i ? args[i].i : 0;
}

static jvalue me_getAction(jobj *t, const jvalue *a, jmeth *m) { return jv_int(M(t)->action); }
static jvalue me_getActionMasked(jobj *t, const jvalue *a, jmeth *m) { return jv_int(M(t)->action & 0xff); }
static jvalue me_getActionIndex(jobj *t, const jvalue *a, jmeth *m) { return jv_int((M(t)->action >> 8) & 0xff); }
static jvalue me_getPointerCount(jobj *t, const jvalue *a, jmeth *m) { return jv_int(M(t)->count); }
static jvalue me_getPointerId(jobj *t, const jvalue *a, jmeth *m) { return jv_int(M(t)->id[idx(a, m->nargs, 0) % MAX_POINTERS]); }
static jvalue me_getX(jobj *t, const jvalue *a, jmeth *m) { return jv_float(M(t)->x[idx(a, m->nargs, 0) % MAX_POINTERS]); }
static jvalue me_getY(jobj *t, const jvalue *a, jmeth *m) { return jv_float(M(t)->y[idx(a, m->nargs, 0) % MAX_POINTERS]); }
static jvalue me_getToolType(jobj *t, const jvalue *a, jmeth *m) { return jv_int(1); /* TOOL_TYPE_FINGER */ }
static jvalue me_getDeviceId(jobj *t, const jvalue *a, jmeth *m) { return jv_int(1); }
static jvalue me_getEventTime(jobj *t, const jvalue *a, jmeth *m) { return jv_long(M(t)->time_ms); }
static jvalue me_getSource(jobj *t, const jvalue *a, jmeth *m) { return jv_int(M(t)->source); }
static jvalue me_zero_int(jobj *t, const jvalue *a, jmeth *m) { return jv_int(0); }
static jvalue me_zero_float(jobj *t, const jvalue *a, jmeth *m) { return jv_float(0.f); }
static jvalue me_zero_long(jobj *t, const jvalue *a, jmeth *m) { return jv_long(0); }
static jvalue me_void(jobj *t, const jvalue *a, jmeth *m) { return jv_void(); }

static jobj *motion_obj(const motion *src) {
  jobj *o = jvm_new("android/view/MotionEvent");
  o->native = malloc(sizeof(motion));
  o->native_free = free;
  memcpy(o->native, src, sizeof(motion));
  return o;
}

static jvalue me_obtain(jobj *t, const jvalue *a, jmeth *m) {
  jobj *src = (jobj *)a[0].l;
  return jv_obj(src ? motion_obj(M(src)) : NULL);
}

static jvalue ke_getAction(jobj *t, const jvalue *a, jmeth *m) { return jv_int(K(t)->action); }
static jvalue ke_getKeyCode(jobj *t, const jvalue *a, jmeth *m) { return jv_int(K(t)->keycode); }
static jvalue ke_getMetaState(jobj *t, const jvalue *a, jmeth *m) { return jv_int(K(t)->meta); }
static jvalue ke_getRepeatCount(jobj *t, const jvalue *a, jmeth *m) { return jv_int(K(t)->repeat); }
static jvalue ke_getSource(jobj *t, const jvalue *a, jmeth *m) { return jv_int(K(t)->source); }
static jvalue ke_getEventTime(jobj *t, const jvalue *a, jmeth *m) { return jv_long(K(t)->time_ms); }
static jvalue ke_getCharacters(jobj *t, const jvalue *a, jmeth *m) { return jv_obj(NULL); }

static jvalue id_getDeviceIds(jobj *t, const jvalue *a, jmeth *m) { return jv_obj(jvm_array('I', 0)); }
static jvalue id_getDevice(jobj *t, const jvalue *a, jmeth *m) { return jv_obj(NULL); }

/* ---------- touch ---------- */

typedef struct {
  int active;
  int vita_id;
  int android_id;
  float x, y;
} slot;

static slot slots[MAX_POINTERS];

static int64_t now_ms(void) {
  return bionic_monotonic_us() / 1000;
}

static void send_motion(int action) {
  motion mv;
  memset(&mv, 0, sizeof(mv));
  mv.action = action;
  mv.time_ms = now_ms();
  mv.source = SOURCE_TOUCHSCREEN;
  for (int i = 0; i < MAX_POINTERS; i++) {
    if (!slots[i].active)
      continue;
    mv.id[mv.count] = slots[i].android_id;
    mv.x[mv.count] = slots[i].x;
    mv.y[mv.count] = slots[i].y;
    mv.count++;
  }
  jobj *o = motion_obj(&mv);
  unity_inject(o);
  jvm_release(o);
}

static int pointer_index(int s) {
  int n = 0;
  for (int i = 0; i < s; i++)
    n += slots[i].active;
  return n;
}

static int active_count(void) {
  return pointer_index(MAX_POINTERS);
}

static int free_android_id(void) {
  for (int id = 0;; id++) {
    int used = 0;
    for (int i = 0; i < MAX_POINTERS; i++)
      used |= slots[i].active && slots[i].android_id == id;
    if (!used)
      return id;
  }
}

static void poll_touch(void) {
  SceTouchData td;
  if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) < 1)
    return;
  int seen[MAX_POINTERS] = { 0 };
  int moved = 0;

  /* moves and new contacts */
  for (unsigned r = 0; r < td.reportNum && r < MAX_POINTERS; r++) {
    float x = td.report[r].x * (SCREEN_W / 1920.0f);
    float y = td.report[r].y * (SCREEN_H / 1088.0f);
    int s = -1;
    for (int i = 0; i < MAX_POINTERS; i++)
      if (slots[i].active && slots[i].vita_id == td.report[r].id)
        s = i;
    if (s >= 0) {
      seen[s] = 1;
      if (slots[s].x != x || slots[s].y != y) {
        slots[s].x = x;
        slots[s].y = y;
        moved = 1;
      }
      continue;
    }
    for (int i = 0; i < MAX_POINTERS; i++) {
      if (!slots[i].active) {
        int first = active_count() == 0;
        slots[i].active = 1;
        slots[i].vita_id = td.report[r].id;
        slots[i].android_id = free_android_id();
        slots[i].x = x;
        slots[i].y = y;
        seen[i] = 1;
        send_motion(first ? ACTION_DOWN : ACTION_POINTER_DOWN | (pointer_index(i) << 8));
        break;
      }
    }
  }
  if (moved)
    send_motion(ACTION_MOVE);
  /* lifted contacts: the event still lists the pointer, then it is dropped */
  for (int i = 0; i < MAX_POINTERS; i++) {
    if (slots[i].active && !seen[i]) {
      int last = active_count() == 1;
      send_motion(last ? ACTION_UP : ACTION_POINTER_UP | (pointer_index(i) << 8));
      slots[i].active = 0;
    }
  }
}

/* ---------- buttons ---------- */

static const struct {
  uint32_t mask;
  int keycode;
} buttons[] = {
  { SCE_CTRL_CROSS, 96 },    /* BUTTON_A */
  { SCE_CTRL_CIRCLE, 97 },   /* BUTTON_B */
  { SCE_CTRL_SQUARE, 99 },   /* BUTTON_X */
  { SCE_CTRL_TRIANGLE, 100 },/* BUTTON_Y */
  { SCE_CTRL_LTRIGGER, 102 },/* BUTTON_L1 */
  { SCE_CTRL_RTRIGGER, 103 },/* BUTTON_R1 */
  { SCE_CTRL_START, 4 },     /* BACK (Unity: Escape, the game's pause/back) */
  { SCE_CTRL_SELECT, 109 },  /* BUTTON_SELECT */
  { SCE_CTRL_UP, 19 },
  { SCE_CTRL_DOWN, 20 },
  { SCE_CTRL_LEFT, 21 },
  { SCE_CTRL_RIGHT, 22 },
};

static uint32_t prev_buttons;
static SceCtrlData pad;

const SceCtrlData *input_pad(void) {
  return &pad;
}

static void send_key(int action, int keycode) {
  jobj *o = jvm_new("android/view/KeyEvent");
  key *k = calloc(1, sizeof(*k));
  k->action = action;
  k->keycode = keycode;
  k->source = keycode == 4 ? SOURCE_KEYBOARD : SOURCE_GAMEPAD;
  k->time_ms = now_ms();
  o->native = k;
  o->native_free = free;
  unity_inject(o);
  jvm_release(o);
}

static void poll_buttons(void) {
  if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1)
    return;
  uint32_t changed = pad.buttons ^ prev_buttons;
  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
    if (changed & buttons[i].mask)
      send_key((pad.buttons & buttons[i].mask) ? 0 : 1, buttons[i].keycode);
  prev_buttons = pad.buttons;
}

/* ---------- soft keyboard ---------- */

static int ime_running;
static uint16_t ime_title[64], ime_initial[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1], ime_buffer[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

static void utf8_to_ucs2(const char *s, uint16_t *out, int max) {
  int n = 0;
  const unsigned char *u = (const unsigned char *)s;
  while (*u && n < max - 1) {
    unsigned c = *u++;
    if (c >= 0xe0 && u[0] && u[1]) {
      c = ((c & 0x0f) << 12) | ((u[0] & 0x3f) << 6) | (u[1] & 0x3f);
      u += 2;
    } else if (c >= 0xc0 && u[0]) {
      c = ((c & 0x1f) << 6) | (u[0] & 0x3f);
      u += 1;
    }
    out[n++] = c;
  }
  out[n] = 0;
}

static void ucs2_to_utf8(const uint16_t *s, char *out, int max) {
  int n = 0;
  for (; *s && n < max - 4; s++) {
    unsigned c = *s;
    if (c < 0x80) {
      out[n++] = c;
    } else if (c < 0x800) {
      out[n++] = 0xc0 | (c >> 6);
      out[n++] = 0x80 | (c & 0x3f);
    } else {
      out[n++] = 0xe0 | (c >> 12);
      out[n++] = 0x80 | ((c >> 6) & 0x3f);
      out[n++] = 0x80 | (c & 0x3f);
    }
  }
  out[n] = 0;
}

void input_show_keyboard(const char *text, int type, int autocorrect, int multiline, int secure, int alert,
                         const char *placeholder) {
  if (ime_running)
    return;
  SceImeDialogParam p;
  sceImeDialogParamInit(&p);
  utf8_to_ucs2(placeholder && *placeholder ? placeholder : "Pixel Gun 3D", ime_title, 64);
  utf8_to_ucs2(text ? text : "", ime_initial, SCE_IME_DIALOG_MAX_TEXT_LENGTH);
  memset(ime_buffer, 0, sizeof(ime_buffer));
  p.supportedLanguages = 0x0001FFFF;
  p.languagesForced = SCE_FALSE;
  p.type = type == 2 || type == 4 ? SCE_IME_TYPE_NUMBER : SCE_IME_TYPE_DEFAULT;
  p.option = multiline ? SCE_IME_OPTION_MULTILINE : 0;
  p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
  p.textBoxMode = secure ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
  p.title = ime_title;
  p.maxTextLength = 128;
  p.initialText = ime_initial;
  p.inputTextBuffer = ime_buffer;
  int r = sceImeDialogInit(&p);
  if (r < 0) {
    LOG("[input] sceImeDialogInit failed 0x%08X", r);
    unity_soft_input_done(text, 1);
    return;
  }
  ime_running = 1;
}

void input_hide_keyboard(void) {
  if (ime_running) {
    sceImeDialogAbort();
  }
}

int input_dialog_active(void) {
  return ime_running;
}

static void poll_ime(void) {
  if (!ime_running)
    return;
  SceCommonDialogStatus st = sceImeDialogGetStatus();
  if (st != SCE_COMMON_DIALOG_STATUS_FINISHED)
    return;
  SceImeDialogResult res;
  memset(&res, 0, sizeof(res));
  sceImeDialogGetResult(&res);
  char text[SCE_IME_DIALOG_MAX_TEXT_LENGTH * 3 + 1];
  int canceled = res.button != SCE_IME_DIALOG_BUTTON_ENTER;
  ucs2_to_utf8(canceled ? ime_initial : ime_buffer, text, sizeof(text));
  sceImeDialogTerm();
  ime_running = 0;
  unity_soft_input_done(text, canceled);
}

void input_poll(void) {
  poll_ime();
  if (ime_running) {
    sceClibMemset(&pad, 0, sizeof(pad)); /* the dialog owns input */
    pad.lx = pad.ly = pad.rx = pad.ry = 128;
    return;
  }
  poll_touch();
  poll_buttons();
}

void input_init(void) {
  pad.lx = pad.ly = pad.rx = pad.ry = 128;
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

  jvm_declare("android/view/InputEvent", NULL);
  jvm_declare("android/view/MotionEvent", "android/view/InputEvent");
  jvm_declare("android/view/KeyEvent", "android/view/InputEvent");
  const char *ME = "android/view/MotionEvent";
  jvm_method(ME, "getAction", "()I", me_getAction);
  jvm_method(ME, "getActionMasked", "()I", me_getActionMasked);
  jvm_method(ME, "getActionIndex", "()I", me_getActionIndex);
  jvm_method(ME, "getPointerCount", "()I", me_getPointerCount);
  jvm_method(ME, "getPointerId", "(I)I", me_getPointerId);
  jvm_method(ME, "getX", "(I)F", me_getX);
  jvm_method(ME, "getY", "(I)F", me_getY);
  jvm_method(ME, "getX", "()F", me_getX);
  jvm_method(ME, "getY", "()F", me_getY);
  jvm_method(ME, "getToolType", "(I)I", me_getToolType);
  jvm_method(ME, "getDeviceId", "()I", me_getDeviceId);
  jvm_method(ME, "getEventTime", "()J", me_getEventTime);
  jvm_method(ME, "getSource", "()I", me_getSource);
  jvm_method(ME, "getHistorySize", "()I", me_zero_int);
  jvm_method(ME, "getHistoricalX", "(II)F", me_zero_float);
  jvm_method(ME, "getHistoricalY", "(II)F", me_zero_float);
  jvm_method(ME, "getHistoricalEventTime", "(I)J", me_zero_long);
  jvm_method(ME, "getButtonState", "()I", me_zero_int);
  jvm_method(ME, "getAxisValue", "(II)F", me_zero_float);
  jvm_method(ME, "getAxisValue", "(I)F", me_zero_float);
  jvm_method(ME, "getMetaState", "()I", me_zero_int);
  jvm_method(ME, "recycle", "()V", me_void);
  jvm_method(ME, "obtain", "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", me_obtain);
  jvm_method("android/view/InputEvent", "getSource", "()I", me_getSource);
  const char *KE = "android/view/KeyEvent";
  jvm_method(KE, "getAction", "()I", ke_getAction);
  jvm_method(KE, "getKeyCode", "()I", ke_getKeyCode);
  jvm_method(KE, "getMetaState", "()I", ke_getMetaState);
  jvm_method(KE, "getRepeatCount", "()I", ke_getRepeatCount);
  jvm_method(KE, "getDeviceId", "()I", me_getDeviceId);
  jvm_method(KE, "getSource", "()I", ke_getSource);
  jvm_method(KE, "getEventTime", "()J", ke_getEventTime);
  jvm_method(KE, "getCharacters", "()Ljava/lang/String;", ke_getCharacters);
  jvm_method(KE, "getUnicodeChar", "()I", me_zero_int);
  jvm_method(KE, "getUnicodeChar", "(I)I", me_zero_int);
  jvm_static(KE, "KEYCODE_VOLUME_DOWN", "I", jv_int(25));
  jvm_static(KE, "KEYCODE_VOLUME_UP", "I", jv_int(24));
  jvm_static(KE, "KEYCODE_ZOOM_IN", "I", jv_int(168));
  jvm_static(KE, "KEYCODE_ZOOM_OUT", "I", jv_int(169));
  jvm_method("android/view/InputDevice", "getDeviceIds", "()[I", id_getDeviceIds);
  jvm_method("android/view/InputDevice", "getDevice", "(I)Landroid/view/InputDevice;", id_getDevice);
}
