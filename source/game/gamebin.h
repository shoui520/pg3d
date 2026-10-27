#ifndef PG3D_GAMEBIN_H
#define PG3D_GAMEBIN_H

/*
 * Supported binaries: Pixel Gun 3D 12.5.0 (armeabi-v7a).
 * Every raw offset below is only valid for these exact files.
 */
#include "loader/so_loader.h"

#define GB_LIBUNITY_SHA256  "98aa0892"   /* prefix check, full hash in gamebin.c */
#define GB_LIBIL2CPP_SHA256 "e0c7c745"

/* libil2cpp.so static addresses */
#define GB_IL2CPP_SIGACTION_SUSPEND_RET 0x1c7211c /* GC_stop_init: sigaction(GC_sig_suspend) */
#define GB_IL2CPP_SIGACTION_RESTART_RET 0x1c72154 /* GC_stop_init: sigaction(GC_sig_thr_restart) */
#define GB_IL2CPP_GC_STOP_COUNT         0x2219e04
#define GB_IL2CPP_GC_WORLD_IS_STOPPED   0x2219e08
#define GB_IL2CPP_GC_THREADS            0x2219e14
#define GB_IL2CPP_GC_SUSPEND_ACK_SEM    0x2220150

/* libunity.so static addresses (ARM code) */
#define GB_UNITY_ENABLE_FRAME_TIME_TRACKER 0x55dd2c /* EnableFrameTimeTracker() */
#define GB_UNITY_FILECACHER_BLOCKSIZE      0x332238 /* FileCacherRead ctor: ldr r5, [sp, #20] (block size) */
#define GB_UNITY_OUT_OF_MEMORY_ERROR       0x0e1cdc /* OutOfMemoryError(size, align, label, line, file) */
#define GB_UNITY_ZIPSTREAM_READ            0x56a170 /* zip entry stream Read(this, buf, n) */
#define GB_UNITY_ZIPSTREAM_SEEK            0x56a2f8 /* zip entry stream Seek(this, offset, whence) */
#define GB_UNITY_ZIPSTREAM_READ_SLOT       0xfe03b8 /* its vtable (0xfe03ac) slot 3 */
#define GB_UNITY_ZIPSTREAM_SEEK_SLOT       0xfe03c0 /* slot 5 */

/* libil2cpp.so managed methods (Il2CppDumper RVAs, ARM code) */
#define GB_IL2CPP_TRAINING_COMPLETED      0x10F07F0 /* TrainingController.get_TrainingCompleted() */
#define GB_IL2CPP_TRAINING_STAGE          0x10F0D18 /* TrainingController.get_CompletedTrainingStage() */
#define GB_IL2CPP_STORAGER_HASITEM        0x0CA206C /* Storager.HasItem(string) */
#define GB_IL2CPP_STORAGER_GETINT         0x0CA183C /* Storager.getInt(string, bool, bool) */
#define GB_IL2CPP_EXP_PLAYERLEVEL         0x16C5D84 /* ExperienceController.get_PlayerLevel() */
#define GB_IL2CPP_EXP_GETCURRENTLEVEL     0x16C7038 /* ExperienceController.GetCurrentLevel() */
#define GB_IL2CPP_EXP_LEVEL_CORRECTED     0x16C0E78 /* ExperienceController.GetCurrentLevelWithUpdateCorrection() */
#define GB_IL2CPP_EXP_CURRENTLEVEL        0x16BFB14 /* ExperienceController.get_currentLevel() */
#define GB_IL2CPP_EXPC_OURTIER            0x16BECBC /* ExpController.get_OurTier() */
#define GB_IL2CPP_EXPC_GETOURTIER         0x16BEE74 /* ExpController.GetOurTier() */
#define GB_IL2CPP_EXPC_OURTIERANYPLACE    0x16BEAA0 /* ExpController.OurTierForAnyPlace() */
#define GB_IL2CPP_EXPC_TIERFORLEVEL       0x16C0918 /* ExpController.TierForLevel(int) */

/* Controls (see game/pad.c) */
#define GB_IL2CPP_TYPEINFO_WEAPONMANAGER  0x21EBA6C /* WeaponManager_TypeInfo slot */
#define GB_IL2CPP_TYPEINFO_JOYSTICKCTRL   0x21EBA7C /* JoystickController_TypeInfo slot */
#define GB_IL2CPP_TYPEINFO_INGAMEGUI      0x21EBA80 /* InGameGUI_TypeInfo slot */
#define GB_IL2CPP_PMC_UPDATE              0x07AD69C /* Player_move_c.Update() */
#define GB_IL2CPP_WM_CURRENTWEAPONSOUNDS  0x08B547C /* WeaponManager.get_currentWeaponSounds() */
#define GB_IL2CPP_FPC_UPDATE              0x0A59A48 /* FirstPersonControlSharp.Update() */
#define GB_IL2CPP_UIJOYSTICK_UPDATE       0x0FE4490 /* UIJoystick.Update() */

/* Unlock-all (see game/unlock.c) */
#define GB_IL2CPP_MAINMENU_UPDATE         0x0D819B0 /* MainMenuController.Update() */
#define GB_IL2CPP_SHOP_STATE_BUTTONS      0x0942D30 /* ShopNGUIController.GetStateButtons() */
#define GB_IL2CPP_TWITTER_SUPPORTED       0x111F9BC /* TwitterController.get_TwitterSupported() */
#define GB_IL2CPP_FACEBOOK_SUPPORTED      0x0A3BF58 /* FacebookController.get_FacebookSupported() */
#define GB_IL2CPP_TWITTERANDROID_LOGGEDIN 0x12CEF08 /* Prime31.TwitterAndroid.isLoggedIn() */
#define GB_IL2CPP_CONNECTSCENE_LAN_SEARCH 0x0F62FD8 /* ConnectScene.StartSearchLocalServers() */
#define GB_IL2CPP_FRIENDS_UPDATE          0x0AA20EC /* FriendsController.Update() */

/* Loading (see game/patches.c) */
#define GB_IL2CPP_CAMPAIGNLOADING_DELAY   0x1647134 /* CampaignLoading.Start: mov r2, #2.0f for Invoke("Load", 2.0f) */

extern so_module *gamebin_unity;
extern so_module *gamebin_il2cpp;

/* Verifies size + SHA-256 of both libraries (cached after the first check). */
int gamebin_verify(const char *unity_path, const char *il2cpp_path, const char *cache_path);

/* Code patches for things the Vita can't or needn't do; call before so_flush. */
void gamebin_patch(void);
/* Offline progression: max level, everything owned, training done. */
void progress_patch(void);
/* Vita pad -> in-game controls. */
void pad_patch(void);
/* Grants every item once per save, from the main menu. */
void unlock_patch(void);
/* The game's server clock, from the console's. */
void clock_patch(void);
/* Decompressed-entry cache under Unity's zip streams. */
void zipcache_patch(void);
/* Trampoline for an ARM function whose first two instructions are not
 * PC-relative: runs them, then continues at addr + 8. */
void *gamebin_trampoline(so_module *m, uintptr_t addr);

#endif
