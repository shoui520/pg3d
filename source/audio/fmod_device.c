/*
 * org.fmod.FMODAudioDevice: FMOD's Android output. The Java class runs a
 * thread that asks the native mixer for 16-bit stereo blocks (fmodProcess into
 * a direct ByteBuffer) and writes them to an AudioTrack; here the blocks go to
 * a SceAudioOut BGM port, whose blocking output paces the mixer.
 */
#include <stdlib.h>
#include <string.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/bthread.h"
#include "jni/java.h"
#include "util/log.h"

#define FMOD "org/fmod/FMODAudioDevice"

enum { INFO_SAMPLE_RATE = 0, INFO_DSP_BUFFER_LENGTH = 1, INFO_DSP_NUM_BUFFERS = 2, INFO_MIXER_RUNNING = 3 };

typedef jint (*getinfo_fn)(JNIEnv *, jobject, jint);
typedef jint (*process_fn)(JNIEnv *, jobject, jobject);

static bthread *thread;
static volatile int running;
static jobj *device;

static void *audio_main(void *arg) {
  getinfo_fn get_info = jvm_native(FMOD, "fmodGetInfo", "(I)I");
  process_fn process = jvm_native(FMOD, "fmodProcess", "(Ljava/nio/ByteBuffer;)I");
  jobject self = (jobject)device;
  int port = -1, frames = 0;
  void *pcm = NULL;
  jobject buffer = NULL;

  while (running) {
    if (port < 0) {
      int rate = get_info(jvm_env, self, INFO_SAMPLE_RATE);
      frames = get_info(jvm_env, self, INFO_DSP_BUFFER_LENGTH);
      int grain = (frames + 63) & ~63;
      if (rate <= 0 || frames <= 0 || grain > SCE_AUDIO_MAX_LEN) {
        sceKernelDelayThread(10 * 1000);
        continue;
      }
      port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, rate, SCE_AUDIO_OUT_MODE_STEREO);
      if (port < 0) {
        LOG("[fmod] sceAudioOutOpenPort(%d Hz, %d) failed: 0x%08X", rate, grain, port);
        break;
      }
      /* FMOD fills exactly `frames`; pad the port grain with silence */
      pcm = calloc(grain, 4);
      buffer = (*jvm_env)->NewDirectByteBuffer(jvm_env, pcm, (jlong)frames * 4);
      LOG("[fmod] output %d Hz, %d frames x %d buffers", rate, frames, get_info(jvm_env, self, INFO_DSP_NUM_BUFFERS));
    }
    if (get_info(jvm_env, self, INFO_MIXER_RUNNING) == 1) {
      process(jvm_env, self, buffer);
      sceAudioOutOutput(port, pcm);
    } else {
      sceKernelDelayThread(5 * 1000);
    }
  }
  if (port >= 0) {
    sceAudioOutOutput(port, NULL);
    sceAudioOutReleasePort(port);
  }
  if (buffer)
    jvm_release((jobj *)buffer);
  free(pcm);
  return NULL;
}

static jvalue fmod_start(jobj *thiz, const jvalue *args, jmeth *m) {
  if (thread)
    return jv_void();
  device = jvm_pin(jvm_retain(thiz));
  running = 1;
  if (bthread_create(&thread, "FMODAudioDevice", audio_main, NULL, 64 * 1024, 0, 0x10000100 - 10) != 0) {
    LOG("[fmod] cannot create audio thread");
    thread = NULL;
    running = 0;
  }
  return jv_void();
}

static jvalue fmod_stop(jobj *thiz, const jvalue *args, jmeth *m) {
  if (!thread)
    return jv_void();
  running = 0;
  bthread_join(thread, NULL);
  thread = NULL;
  return jv_void();
}

static jvalue fmod_isRunning(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_bool(thread != NULL);
}

static jvalue fmod_noRecord(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_int(-1);
}

static jvalue fmod_void(jobj *thiz, const jvalue *args, jmeth *m) {
  return jv_void();
}

void java_fmod_init(void) {
  jvm_method(FMOD, "start", "()V", fmod_start);
  jvm_method(FMOD, "stop", "()V", fmod_stop);
  jvm_method(FMOD, "close", "()V", fmod_stop);
  jvm_method(FMOD, "isRunning", "()Z", fmod_isRunning);
  jvm_method(FMOD, "startAudioRecord", "(III)I", fmod_noRecord);
  jvm_method(FMOD, "stopAudioRecord", "()V", fmod_void);
}
