#ifndef PG3D_NDK_H
#define PG3D_NDK_H

#include <stdint.h>

#define SCREEN_W 960
#define SCREEN_H 544

/* Native input event as produced by our input layer (AInputEvent*). */
typedef struct {
  int32_t type;      /* AINPUT_EVENT_TYPE_KEY = 1, MOTION = 2 */
  int32_t device_id;
  int32_t source;
  int32_t action;
  int32_t keycode;
  int32_t meta;
  int32_t repeat;
  int64_t time_ns;
} ndk_input_event;

void *ndk_window(void);

#endif
