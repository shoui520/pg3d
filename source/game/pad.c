/*
 * PG3D has no controller support, so the Vita pad drives the same managed
 * state the touch HUD does:
 *   left stick   UIJoystick.value (JoystickController.leftJoystick)
 *   right stick  FirstPersonControlSharp.MoveCamera, as the touch drag does
 *   R            rightJoystick.isShooting, read by Player_move_c.ShootUpdate
 *   X            rightJoystick.jumpPressed, consumed by FirstPersonControlSharp
 *   L            Player_move_c.ZoomPress while held, if the weapon can zoom
 *   SQUARE       Player_move_c.ReloadPressed
 *   TRIANGLE     what releasing the gadget button does: the current gadget's
 *                CanUse check, then its use calls
 *   LEFT/RIGHT   previous / next weapon, UP/DOWN two back / ahead, CIRCLE
 *                the melee weapon (InGameGUI.SelectWeaponFromIndex, as tapping
 *                the weapon bar does: it lights the bar's slot, then switches)
 *   SELECT       scoreboard (RanksPressed / BackRanksPressed)
 *   START        BACK key (input.c), which the game's own Escape handler
 *                turns into Player_move_c.SwitchPause
 * The actions run in a Player_move_c.Update hook for the local player
 * (WeaponManager.myPlayerMoveC), on UnityMain right where the game reads its
 * touch state. Anything that can throw is called through managed_invoke.
 */
#include <math.h>
#include <stdint.h>
#include <psp2/ctrl.h>

#include "bionic/bionic.h"
#include "game/gamebin.h"
#include "game/managed.h"
#include "input/input.h"
#include "util/log.h"

#define STICK_DEADZONE 0.18f
/* tuned at the game's top sensitivity (SettingsController 6..19, default 12),
 * which MoveCamera multiplies in: the default slider now gives that speed */
#define CAMERA_PX_PER_SEC (250.0f * 19.0f / 12.0f)

/* Il2CppClass: static field storage */
#define KLASS_STATIC_FIELDS 0x4C

/* field offsets (Il2CppDumper) */
#define JC_RIGHT_JOYSTICK 0x4       /* static TouchPadController */
#define WM_SHARED_MANAGER 0x10C     /* static WeaponManager */
#define GUI_SHARED 0x0              /* static InGameGUI sharedInGameGUI */
#define UIJ_VALUE 0x14              /* Vector2 value */
#define TPC_JUMP_PRESSED 0x15
#define TPC_IS_SHOOTING 0x78
#define FPC_MOVE_C 0xA8
#define PMC_IS_ZOOMING 0x1A0
#define PMC_SHOW_RANKS 0x1ED
#define WM_MY_PLAYER_MOVE_C 0x3C
#define WM_PLAYER_WEAPONS 0x58      /* ArrayList of Weapon */
#define WM_CURRENT_INDEX 0x60
#define ARRAYLIST_SIZE 0x8
#define ARRAYLIST_ITEMS 0xC
#define ARRAY_DATA 0x10
#define WEAPON_RECORD 0x8           /* ItemRecord */
#define RECORD_IS_MELEE 0x41
#define RECORD_CATEGORY 0x58        /* categoryNabor, 1-based */
#define WS_IS_ZOOMING 0xC8          /* WeaponSounds.isZooming: weapon has a scope */

/* Mono Dictionary<TKey, TValue> */
#define DICT_LINK_SLOTS 0xC         /* Link[] {HashCode, Next}; used slots have the top bit */
#define DICT_KEY_SLOTS 0x10
#define DICT_VALUE_SLOTS 0x14
#define DICT_TOUCHED_SLOTS 0x18

/* Gadget vtable entries, as TouchPadController.OnPress reads them */
#define GADGET_VT_CAN_USE 0x104
#define GADGET_VT_USE 0xCC
#define GADGET_VT_AFTER_USE 0xD4

#define CATEGORY_MELEE 3

#define FIELD(o, off, type) (*(type *)((uint8_t *)(o) + (off)))

typedef void (*void_fn)(void *self, void *method);
typedef void *(*obj_fn)(void *self, void *method);

static uintptr_t il2cpp;
static void_fn pmc_update_orig, uij_update_orig, fpc_update_orig;
static obj_fn current_weapon_sounds;

static struct {
  int resolved;
  void *reload, *zoom, *ranks, *back_ranks, *select_weapon, *move_camera, *gadget_set, *default_gadget;
} mi;

static uint32_t prev_buttons;
static int stick_was_active;

static void *class_static(uintptr_t typeinfo_slot, int offset) {
  void *klass = *(void **)(il2cpp + typeinfo_slot);
  if (!klass)
    return NULL;
  void *statics = FIELD(klass, KLASS_STATIC_FIELDS, void *);
  return statics ? FIELD(statics, offset, void *) : NULL;
}

static void *weapon_manager(void) {
  return class_static(GB_IL2CPP_TYPEINFO_WEAPONMANAGER, WM_SHARED_MANAGER);
}

static void *local_player(void) {
  void *wm = weapon_manager();
  return wm ? FIELD(wm, WM_MY_PLAYER_MOVE_C, void *) : NULL;
}

/* MethodInfos, looked up once the game's classes exist */
static int resolve(void) {
  if (mi.resolved)
    return mi.resolved > 0;
  void *pmc = managed_class("", "Player_move_c");
  void *gui = managed_class("", "InGameGUI");
  void *fpc = managed_class("", "FirstPersonControlSharp");
  void *set = managed_class("", "InGameGadgetSet");
  void *gi = managed_class("", "GadgetsInfo");
  mi.reload = managed_method(pmc, "ReloadPressed", 0);
  mi.zoom = managed_method(pmc, "ZoomPress", 0);
  mi.ranks = managed_method(pmc, "RanksPressed", 0);
  mi.back_ranks = managed_method(pmc, "BackRanksPressed", 0);
  mi.select_weapon = managed_method(gui, "SelectWeaponFromIndex", 2);
  mi.move_camera = managed_method(fpc, "MoveCamera", 1);
  mi.gadget_set = managed_method(set, "get_CurrentSet", 0);
  mi.default_gadget = managed_method(gi, "get_DefaultGadget", 0);
  mi.resolved = mi.reload && mi.zoom && mi.ranks && mi.back_ranks && mi.select_weapon && mi.move_camera &&
                        mi.gadget_set && mi.default_gadget
                    ? 1
                    : -1;
  LOG("[pad] managed methods %s", mi.resolved > 0 ? "resolved" : "MISSING, pad actions disabled");
  return mi.resolved > 0;
}

static float axis(uint8_t v) {
  float a = (v - 127.5f) / 127.5f;
  if (fabsf(a) < STICK_DEADZONE)
    return 0.f;
  /* rescale past the deadzone, squared for finer aim near the centre */
  a = (a > 0 ? a - STICK_DEADZONE : a + STICK_DEADZONE) / (1.f - STICK_DEADZONE);
  if (a > 1.f)
    a = 1.f;
  if (a < -1.f)
    a = -1.f;
  return a * fabsf(a);
}

/* ---------- actions ---------- */

static void *weapon_record(void *weapons, int i) {
  void *items = FIELD(weapons, ARRAYLIST_ITEMS, void *);
  void *w = items ? ((void **)((uint8_t *)items + ARRAY_DATA))[i] : NULL;
  return w ? FIELD(w, WEAPON_RECORD, void *) : NULL;
}

static void change_weapon(void *wm, int delta, int melee) {
  void *gui = class_static(GB_IL2CPP_TYPEINFO_INGAMEGUI, GUI_SHARED);
  void *weapons = FIELD(wm, WM_PLAYER_WEAPONS, void *);
  if (!gui || !weapons)
    return;
  int n = FIELD(weapons, ARRAYLIST_SIZE, int);
  int cur = FIELD(wm, WM_CURRENT_INDEX, int);
  if (n <= 1)
    return;
  int target = -1;
  if (melee) {
    for (int i = 0; i < n && target < 0; i++) {
      void *r = weapon_record(weapons, i);
      if (r && FIELD(r, RECORD_CATEGORY, int) == CATEGORY_MELEE)
        target = i;
    }
    for (int i = 0; i < n && target < 0; i++) {
      void *r = weapon_record(weapons, i);
      if (r && FIELD(r, RECORD_IS_MELEE, uint8_t))
        target = i;
    }
  } else {
    target = ((cur + delta) % n + n) % n;
  }
  if (target >= 0 && target != cur) {
    int update_swipe = 1;
    void *args[] = { &target, &update_swipe };
    managed_invoke(mi.select_weapon, gui, args, NULL);
  }
}

/* InGameGadgetSet.CurrentSet[GadgetsInfo.DefaultGadget] */
static void *current_gadget(void) {
  void *set, *boxed;
  if (managed_invoke(mi.gadget_set, NULL, NULL, &set) || !set)
    return NULL;
  if (managed_invoke(mi.default_gadget, NULL, NULL, &boxed) || !boxed)
    return NULL;
  int category = *(int *)managed_unbox(boxed);
  int touched = FIELD(set, DICT_TOUCHED_SLOTS, int);
  uint8_t *links = FIELD(set, DICT_LINK_SLOTS, uint8_t *);
  uint8_t *keys = FIELD(set, DICT_KEY_SLOTS, uint8_t *);
  uint8_t *values = FIELD(set, DICT_VALUE_SLOTS, uint8_t *);
  if (!links || !keys || !values)
    return NULL;
  for (int i = 0; i < touched; i++) {
    int hash = ((int *)(links + ARRAY_DATA))[i * 2];
    if (hash < 0 && ((int *)(keys + ARRAY_DATA))[i] == category)
      return ((void **)(values + ARRAY_DATA))[i];
  }
  return NULL;
}

static void use_gadget(void) {
  void *g = current_gadget();
  void *can;
  if (!g) {
    LOG("[pad] no gadget equipped");
    return;
  }
  if (managed_invoke(managed_vmethod(g, GADGET_VT_CAN_USE), g, NULL, &can) || !can || !*(uint8_t *)managed_unbox(can))
    return;
  if (managed_invoke(managed_vmethod(g, GADGET_VT_USE), g, NULL, NULL) == 0)
    managed_invoke(managed_vmethod(g, GADGET_VT_AFTER_USE), g, NULL, NULL);
}

/* ---------- hooks ---------- */

static void pmc_update(void *self, void *method) {
  if (self == local_player() && !input_dialog_active() && resolve()) {
    const SceCtrlData *pad = input_pad();
    uint32_t held = pad->buttons, down = held & ~prev_buttons, up = prev_buttons & ~held;
    prev_buttons = held;
    void *tpc = class_static(GB_IL2CPP_TYPEINFO_JOYSTICKCTRL, JC_RIGHT_JOYSTICK);
    void *wm = weapon_manager();
    int ranks = FIELD(self, PMC_SHOW_RANKS, uint8_t);

    if (down & SCE_CTRL_SELECT)
      managed_invoke(ranks ? mi.back_ranks : mi.ranks, self, NULL, NULL);
    if (tpc && !ranks) {
      if (held & SCE_CTRL_RTRIGGER)
        FIELD(tpc, TPC_IS_SHOOTING, uint8_t) = 1;
      else if (up & SCE_CTRL_RTRIGGER)
        FIELD(tpc, TPC_IS_SHOOTING, uint8_t) = 0;
      if (down & SCE_CTRL_CROSS)
        FIELD(tpc, TPC_JUMP_PRESSED, uint8_t) = 1;
      else if (up & SCE_CTRL_CROSS)
        FIELD(tpc, TPC_JUMP_PRESSED, uint8_t) = 0;
    }
    if (!ranks) {
      if (down & SCE_CTRL_TRIANGLE)
        use_gadget();
      if (down & SCE_CTRL_SQUARE)
        managed_invoke(mi.reload, self, NULL, NULL);
      int zooming = FIELD(self, PMC_IS_ZOOMING, uint8_t);
      if (((down & SCE_CTRL_LTRIGGER) && !zooming) || ((up & SCE_CTRL_LTRIGGER) && zooming)) {
        void *ws = wm ? current_weapon_sounds(wm, NULL) : NULL;
        if (zooming || (ws && FIELD(ws, WS_IS_ZOOMING, uint8_t)))
          managed_invoke(mi.zoom, self, NULL, NULL);
      }
      if (wm && (down & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_CIRCLE))) {
        int delta = (down & SCE_CTRL_LEFT) ? -1 : (down & SCE_CTRL_UP) ? -2 : (down & SCE_CTRL_DOWN) ? 2 : 1;
        change_weapon(wm, delta, !!(down & SCE_CTRL_CIRCLE));
      }
    }
  }
  pmc_update_orig(self, method);
}

static void uij_update(void *self, void *method) {
  uij_update_orig(self, method);
  const SceCtrlData *pad = input_pad();
  float x = axis(pad->lx), y = -axis(pad->ly);
  int active = x != 0.f || y != 0.f;
  if (active || stick_was_active) {
    float len = sqrtf(x * x + y * y);
    if (len > 1.f) {
      x /= len;
      y /= len;
    }
    FIELD(self, UIJ_VALUE, float) = x;
    FIELD(self, UIJ_VALUE + 4, float) = y;
  }
  stick_was_active = active;
}

static void fpc_update(void *self, void *method) {
  static int64_t last_us;
  void *local = local_player();
  if (local && FIELD(self, FPC_MOVE_C, void *) == local && resolve()) {
    int64_t now = bionic_monotonic_us();
    float dt = last_us ? (now - last_us) / 1e6f : 0.f;
    last_us = now;
    if (dt > 0.1f)
      dt = 0.1f;
    const SceCtrlData *pad = input_pad();
    float delta[2] = { axis(pad->rx) * CAMERA_PX_PER_SEC * dt, -axis(pad->ry) * CAMERA_PX_PER_SEC * dt };
    if (delta[0] != 0.f || delta[1] != 0.f) {
      void *args[] = { delta };
      managed_invoke(mi.move_camera, self, args, NULL);
    }
  }
  fpc_update_orig(self, method);
}

void pad_patch(void) {
  so_module *m = gamebin_il2cpp;
  il2cpp = m->base;
  if (!managed_init()) {
    LOG("[pad] il2cpp API incomplete, no pad controls");
    return;
  }
  current_weapon_sounds = (obj_fn)(il2cpp + GB_IL2CPP_WM_CURRENTWEAPONSOUNDS);
  pmc_update_orig = gamebin_trampoline(m, il2cpp + GB_IL2CPP_PMC_UPDATE);
  uij_update_orig = gamebin_trampoline(m, il2cpp + GB_IL2CPP_UIJOYSTICK_UPDATE);
  fpc_update_orig = gamebin_trampoline(m, il2cpp + GB_IL2CPP_FPC_UPDATE);
  if (!pmc_update_orig || !uij_update_orig || !fpc_update_orig) {
    LOG("[pad] trampoline allocation failed, no pad controls");
    return;
  }
  so_hook_arm(il2cpp + GB_IL2CPP_PMC_UPDATE, (const void *)pmc_update);
  so_hook_arm(il2cpp + GB_IL2CPP_UIJOYSTICK_UPDATE, (const void *)uij_update);
  so_hook_arm(il2cpp + GB_IL2CPP_FPC_UPDATE, (const void *)fpc_update);
  LOG("[pad] in-game controls hooked");
}
