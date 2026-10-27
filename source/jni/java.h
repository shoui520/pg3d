#ifndef PG3D_JAVA_H
#define PG3D_JAVA_H

#include <stddef.h>
#include <stdint.h>

#include "jni/jvm.h"

extern jobj *java_activity;
extern jobj *java_unity_player;

void java_android_init(void);
void java_prefs_init(void);
void java_unity_init(void);
void java_prefs_flush(void);
/* Writes dirty preferences, at most once a second; call once per frame. */
void java_prefs_tick(void);
void java_runtime_init(void);
void java_fmod_init(void);
/* runs Runnables posted to the UI thread / main Handler (UnityMain loop) */
void java_ui_drain(void);
void java_run(jobj *runnable);
void java_map_put(jobj *map, jobj *k, jobj *v);
/* entries of a map from our java/util/Map model (borrowed references) */
int java_map_size(jobj *map);
jobj *java_map_key(jobj *map, int i);
jobj *java_map_value(jobj *map, int i);
void java_www_init(void);

jobj *java_file(const char *path);
/* DER of the APK's signing certificate (malloc'd), or NULL */
uint8_t *java_apk_certificate(const char *vita_apk_path, size_t *len);
const char *java_device_id(void);
const char *java_language(void);
char *java_uri_encode(const char *s, const char *allow); /* malloc'd */
char *java_uri_decode(const char *s);                    /* malloc'd */

#endif
