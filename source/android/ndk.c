/*
 * libandroid fakes: one fixed 960x544 native window, loopers, a sensor
 * manager with no sensors, and AInputEvent accessors over ndk_input_event.
 */
#include "android/ndk.h"

#include <stdlib.h>
#include <string.h>

#include "util/log.h"

/* ---------- ANativeWindow ---------- */

typedef struct {
  uint32_t magic;
  int32_t width, height, format;
  void *pixels;
} fake_window;

static fake_window window = { 0x574e4457, SCREEN_W, SCREEN_H, 1, NULL };

void *ndk_window(void) {
  return &window;
}

void *ANativeWindow_fromSurface(void *env, void *surface) {
  DLOG("[ndk] ANativeWindow_fromSurface(%p)", surface);
  return surface ? &window : NULL;
}

void ANativeWindow_acquire(void *w) {}
void ANativeWindow_release(void *w) {}
int32_t ANativeWindow_getWidth(fake_window *w) { return w->width; }
int32_t ANativeWindow_getHeight(fake_window *w) { return w->height; }

int32_t ANativeWindow_setBuffersGeometry(fake_window *w, int32_t width, int32_t height, int32_t format) {
  DLOG("[ndk] ANativeWindow_setBuffersGeometry(%d, %d, %d)", width, height, format);
  if (format)
    w->format = format;
  return 0;
}

typedef struct {
  int32_t width, height, stride, format;
  void *bits;
  uint32_t reserved[6];
} ANativeWindow_Buffer;

/* Software-locked frames (Unity only uses this for its splash fallback) go
 * nowhere visible; the buffer just has to be valid. */
int32_t ANativeWindow_lock(fake_window *w, ANativeWindow_Buffer *out, void *dirty) {
  if (!w->pixels)
    w->pixels = calloc(SCREEN_W * SCREEN_H, 4);
  out->width = w->width;
  out->height = w->height;
  out->stride = w->width;
  out->format = w->format;
  out->bits = w->pixels;
  return 0;
}

int32_t ANativeWindow_unlockAndPost(fake_window *w) {
  return 0;
}

/* ---------- ALooper ---------- */

static uint32_t looper_obj = 0x4c4f4f50;

void *ALooper_forThread(void) { return &looper_obj; }
void *ALooper_prepare(int opts) { return &looper_obj; }

/* ---------- ASensor (no sensors) ---------- */

static uint32_t sensor_manager = 0x53454e53;
static uint32_t sensor_queue = 0x53515545;

void *ASensorManager_getInstance(void) { return &sensor_manager; }
int ASensorManager_getSensorList(void *m, const void ***list) {
  if (list)
    *list = NULL;
  return 0;
}
const void *ASensorManager_getDefaultSensor(void *m, int type) { return NULL; }
void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data) { return &sensor_queue; }
int ASensorManager_destroyEventQueue(void *m, void *q) { return 0; }
int ASensorEventQueue_enableSensor(void *q, const void *s) { return -1; }
int ASensorEventQueue_disableSensor(void *q, const void *s) { return 0; }
int ASensorEventQueue_setEventRate(void *q, const void *s, int32_t us) { return 0; }
int ASensorEventQueue_hasEvents(void *q) { return 0; }
long ASensorEventQueue_getEvents(void *q, void *events, size_t count) { return 0; }
const char *ASensor_getName(const void *s) { return "none"; }
const char *ASensor_getVendor(const void *s) { return "none"; }
int ASensor_getType(const void *s) { return 0; }
float ASensor_getResolution(const void *s) { return 0.f; }
int ASensor_getMinDelay(const void *s) { return 0; }

/* ---------- AInputEvent ---------- */

int32_t AInputEvent_getType(const ndk_input_event *e) { return e->type; }
int32_t AInputEvent_getDeviceId(const ndk_input_event *e) { return e->device_id; }
int32_t AKeyEvent_getAction(const ndk_input_event *e) { return e->action; }
int32_t AKeyEvent_getKeyCode(const ndk_input_event *e) { return e->keycode; }
int32_t AKeyEvent_getMetaState(const ndk_input_event *e) { return e->meta; }
