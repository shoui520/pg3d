/* Code patches applied to the verified game binaries after relocation. */
#include <stdint.h>

#include "game/gamebin.h"
#include "util/log.h"

#define ARM_BX_LR 0xE12FFF1Eu
#define ARM_MOV_R0_0 0xE3A00000u
#define ARM_LDR_R5_SP_20 0xE59D5014u
#define ARM_MOV_R5_64K 0xE3A05801u /* mov r5, #0x10000 */
#define ARM_MOV_R2_2F 0xE3A02101u    /* mov r2, #0x40000000 (2.0f) */
#define ARM_MOV_R2_1_8F 0xE3A0243Eu  /* mov r2, #0x3E000000 (0.125f) */

static void ret_void(so_module *m, uintptr_t off, const char *what) {
  uint32_t insn = ARM_BX_LR;
  so_write(m->base + off, &insn, sizeof(insn));
  LOG("[patch] %s -> return", what);
}

static void ret_false(so_module *m, uintptr_t off, const char *what) {
  uint32_t insn[] = { ARM_MOV_R0_0, ARM_BX_LR };
  so_write(m->base + off, insn, sizeof(insn));
  LOG("[patch] %s -> false", what);
}

void unity_log_mem(void);

/* Unity's own report formats a garbage label and crashes; say what we know. */
static void out_of_memory(unsigned size, int align, int label, int line, const char *file) {
  LOG("[mem] Unity out of memory: %u bytes (align %d, label %d) at %s:%d", size, align, label, file ? file : "?", line);
  unity_log_mem();
  extern void alloc_dump_callers(void);
  alloc_dump_callers();
  fatal("[mem] out of memory");
}

void gamebin_patch(void) {
  so_hook_arm(gamebin_unity->base + GB_UNITY_OUT_OF_MEMORY_ERROR, (const void *)out_of_memory);
  /* Frame pacing through a Java Choreographer HandlerThread; vitaGL already
   * paces on vblank and we have no HandlerThread. */
  ret_void(gamebin_unity, GB_UNITY_ENABLE_FRAME_TIME_TRACKER, "EnableFrameTimeTracker");
  /* No Twitter/Facebook plugins offline: their Java calls throw (Settings
   * polls Twitter login state every frame). Report them unsupported, as the
   * game does on platforms without them. */
  ret_false(gamebin_il2cpp, GB_IL2CPP_TWITTER_SUPPORTED, "TwitterController.TwitterSupported");
  ret_false(gamebin_il2cpp, GB_IL2CPP_FACEBOOK_SUPPORTED, "FacebookController.FacebookSupported");
  /* The online results screen still asks the Android plugin directly when
   * you place first; the throw aborts it before the next round. */
  ret_false(gamebin_il2cpp, GB_IL2CPP_TWITTERANDROID_LOGGEDIN, "TwitterAndroid.isLoggedIn");
  /* ConnectScene.Start searches for LAN servers over UDP before hiding the
   * battle panel the main menu opens with; the socket is refused offline, the
   * throw aborts Start and the panel stays up. */
  ret_void(gamebin_il2cpp, GB_IL2CPP_CONNECTSCENE_LAN_SEARCH, "ConnectScene.StartSearchLocalServers");
  progress_patch();
  pad_patch();
  unlock_patch();
  clock_patch();
  zipcache_patch();

  /* Every open serialized file keeps two cache blocks (256 KB each by
   * default); PG3D keeps hundreds of .split files open. Use 64 KB blocks. */
  uintptr_t bs = gamebin_unity->base + GB_UNITY_FILECACHER_BLOCKSIZE;
  if (*(uint32_t *)bs == ARM_LDR_R5_SP_20) {
    uint32_t insn = ARM_MOV_R5_64K;
    so_write(bs, &insn, sizeof(insn));
    LOG("[patch] FileCacherRead block size -> 64 KB");
  }

  /* The loading screen before every match waits 2 s before it starts loading
   * the map (the wait shows nothing new); a few frames are enough for the
   * screen to be up before the main thread blocks. */
  uintptr_t delay = gamebin_il2cpp->base + GB_IL2CPP_CAMPAIGNLOADING_DELAY;
  if (*(uint32_t *)delay == ARM_MOV_R2_2F) {
    uint32_t insn = ARM_MOV_R2_1_8F;
    so_write(delay, &insn, sizeof(insn));
    LOG("[patch] CampaignLoading delay -> 0.125 s");
  }
}
