/*
 * Offline unlock-all: grant every item through the game's own purchase paths,
 * so the save really owns it (ownership, upgrade chains and pet levels are
 * all bookkept by the game, which a getter hook can't fake consistently):
 *   weapons  ShopNGUIController.ProvideItem(categoryNabor - 1, tag) for every
 *            ItemDb record, upgrade stages included
 *   wear     ProvideItem(category, id) for every id in Wear.wear's chains
 *   gadgets  GadgetsInfo.ProvideGadget(id) for every GadgetsInfo.info entry;
 *            GadgetsInfo.ActualizeEquippedGadgets then swaps an equipped
 *            gadget for its last bought upgrade, as the game does after a
 *            bulk grant (checked every boot, it's cheap)
 *   pets     PetsManager.AddOrUpdatePet(base id) once per PetsInfo.info level
 *            entry: the first call adds the pet, the rest upgrade it to max
 * Owning things isn't the whole of it: levels, points and progress are
 * separate state, so a second pass maxes them the way the game's own paths
 * would:
 *   weapons  each owned weapon's WeaponUpgrade is stepped with
 *            WeaponUpgrade.IncreaseLevel until it stops changing, pending
 *            timed upgrades are dropped, then SaveWeaponLevels once
 *   armor    Wear.DisableNoviceArmor retires the novice set (and grants the
 *            first armor), a pending timed upgrade is cancelled, then
 *            Wear.CurrentPlayerArmor is stepped with ArmorUpgrade.IncreaseLevel
 *            to the last tier's last level and stored with SetCurrentPlayerArmor
 *   pets     PetsManager.Upgrade needs PlayerPet.Points >= the level's
 *            ToUpPoints (AddOrUpdatePet on an owned pet only adds a point),
 *            so each pet is given the points for a level, then upgraded
 *   league   RatingSystem trophies set to the top division threshold
 *            (leagueRatings' last entry), then UpdateLeagueEvent
 *   weapon skins       WeaponSkinsManager.ProvideSkin for every skin
 *   character skins    Storager.SetItem for every SkinsController
 *                      shopKeyFromNameSkin id (what ProvideItemCore does)
 *   lobby decorations  LobbyItemsController.AddItemNow for every item, once
 *                      FriendsController has a server time (game/clock.c)
 *                      to stamp the craft with
 *   achievements       every Achievement's points and stage set to its last
 *                      threshold, then AchievementsManager.SaveProgresses
 *   campaign           3 stars on every level of every LevelBox, boxes opened
 *                      with CampaignProgress.OpenNewBoxIfPossible
 *   craft parts        DetailsShopCraftInfo.AddDetails up to 9999 of each
 * Each pass runs once per save, on the first main menu frames where the
 * game's state is ready (Storager flags).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bionic/bionic.h"
#include "game/gamebin.h"
#include "game/managed.h"
#include "util/log.h"

#define UNLOCK_FLAG_KEY "VitaPort.UnlockAll.v1"
#define MAXED_FLAG_KEY "VitaPort.MaxAll.v2"
#define MAX_STEPS 64
#define MAX_ARMOR_STEPS 1024 /* one level per step: tiers x 12 levels */
#define DETAILS_TARGET 9999
#define CAMPAIGN_STARS 3

#define KLASS_STATIC_FIELDS 0x4C
#define WEAR_WEAR 0x30              /* static Dictionary<CategoryNames, List<List<string>>> wear */
#define LIST_ITEMS 0x8
#define LIST_SIZE 0xC
#define ARRAY_DATA 0x10
#define DICT_LINK_SLOTS 0xC
#define DICT_KEY_SLOTS 0x10
#define DICT_VALUE_SLOTS 0x14
#define DICT_TOUCHED_SLOTS 0x18
#define RECORD_CATEGORY 0x58        /* ItemRecord.categoryNabor, 1-based weapon category */
#define RECORD_TAG 0x68
#define PETINFO_ID_WITHOUT_UP 0x18
#define PETINFO_TO_UP_POINTS 0x2C
#define UPGRADE_TIER 0x8          /* WeaponUpgrade / ArmorUpgrade */
#define UPGRADE_LEVEL 0xC
#define WM_UPGRADING_WEAPONS 0x38   /* static Dictionary<string, long> _upgradingWeapons */
#define PETSMANAGER_PLAYER_PETS 0xC
#define PLAYERPET_POINTS 0x8
#define PLAYERPET_INFO_ID 0x10
#define ARRAY_LENGTH 0xC
#define RATING_LEAGUE_RATINGS 0x34  /* SaltedIntArray leagueRatings */
#define RATING_LEAGUE 0x38
#define RATING_DIVISION 0x3C
#define WEAPONSKIN_ID 0x8
#define SKINS_SHOP_KEYS 0x2C        /* static Dictionary<string, string> shopKeyFromNameSkin */
#define LOBBY_IS_READY 0x10
#define LOBBY_ALL_ITEMS 0x14
#define LOBBYITEM_PLAYER_INFO 0x10
#define LOBBYPLAYER_IS_CRAFTED 0x18
#define FRIENDS_SERVER_TIME_SET 0x5C /* static bool isUpdateServerTimeAfterRun */
#define ACHMANAGER_ALL 0xC
#define ACHMANAGER_READY 0x14
#define ACH_DATA 0x8
#define ACH_PROGRESS 0xC
#define ACHDATA_THRESHOLDS 0x20
#define ACHPROGRESS_POINTS 0x8      /* SaltedInt */
#define ACHPROGRESS_STAGE 0x10
#define LEVELBOX_LEVELS 0x8
#define LEVELBOX_NAME 0x10
#define CAMPAIGNLEVEL_SCENE 0x1C

#define FIELD(o, off, type) (*(type *)((uint8_t *)(o) + (off)))

enum { GRANT_ITEM, GRANT_GADGET, GRANT_PET };

typedef struct {
  int kind, category;
  void *id; /* managed string, kept alive by the game's static tables */
} grant;

typedef void (*void_fn)(void *self, void *method);

static void_fn menu_update_orig;
static grant *work;
static int n_work, cap_work, next_work, failures;
static int state; /* 0 not started, 1 granting, 2 done or skipped */
static int maxed_state; /* 0 pending, 1 done or skipped */

static struct {
  void *provide_item, *provide_gadget, *pets_instance, *add_pet, *get_int, *set_int;
} mi;

static void add(int kind, int category, void *id) {
  if (!id)
    return;
  if (n_work == cap_work) {
    cap_work = cap_work ? cap_work * 2 : 1024;
    work = realloc(work, cap_work * sizeof(*work));
  }
  work[n_work++] = (grant){ kind, category, id };
}

typedef void (*dict_fn)(void *key, void *value, void *ctx);

static void dict_each(void *dict, dict_fn fn, void *ctx) {
  int touched = FIELD(dict, DICT_TOUCHED_SLOTS, int);
  uint8_t *links = FIELD(dict, DICT_LINK_SLOTS, uint8_t *);
  uint8_t *keys = FIELD(dict, DICT_KEY_SLOTS, uint8_t *);
  uint8_t *values = FIELD(dict, DICT_VALUE_SLOTS, uint8_t *);
  for (int i = 0; i < touched; i++)
    if (((int *)(links + ARRAY_DATA))[i * 2] < 0) /* used slot */
      fn(((void **)(keys + ARRAY_DATA))[i], ((void **)(values + ARRAY_DATA))[i], ctx);
}

static int list_size(void *list) {
  return list ? FIELD(list, LIST_SIZE, int) : 0;
}

static void *list_get(void *list, int i) {
  return ((void **)(FIELD(list, LIST_ITEMS, uint8_t *) + ARRAY_DATA))[i];
}

static void wear_category(void *key, void *chains, void *ctx) {
  int category = (int)(intptr_t)key;
  for (int c = 0; c < list_size(chains); c++) {
    void *chain = list_get(chains, c);
    for (int i = 0; i < list_size(chain); i++)
      add(GRANT_ITEM, category, list_get(chain, i));
  }
}

static void gadget_entry(void *id, void *info, void *ctx) {
  add(GRANT_GADGET, 0, id);
}

static void pet_entry(void *id, void *info, void *ctx) {
  add(GRANT_PET, 0, info ? FIELD(info, PETINFO_ID_WITHOUT_UP, void *) : NULL);
}

static void *call0(void *klass, const char *name) {
  void *r = NULL;
  managed_invoke(managed_method(klass, name, 0), NULL, NULL, &r);
  return r;
}

static int storager_int(const char *key) {
  void *k = managed_string(key), *boxed = NULL;
  int f = 0;
  void *args[] = { k, &f, &f };
  if (managed_invoke(mi.get_int, NULL, args, &boxed) || !boxed)
    return 0;
  return *(int *)managed_unbox(boxed);
}

/* 1: work list built, 0: not ready yet (retry next frame), -1: give up */
static int build(void) {
  void *shop = managed_class("", "ShopNGUIController");
  void *gadgets = managed_class("", "GadgetsInfo");
  void *pets_info = managed_class("", "PetsInfo");
  void *pets = managed_class("Rilisoft", "PetsManager");
  void *itemdb = managed_class("", "ItemDb");
  void *wear = managed_class("", "Wear");
  void *storager = managed_class("", "Storager");
  mi.provide_item = managed_method(shop, "ProvideItem", 12);
  mi.provide_gadget = managed_method(gadgets, "ProvideGadget", 1);
  mi.add_pet = managed_method(pets, "AddOrUpdatePet", 1);
  mi.get_int = managed_method(storager, "getInt", 3);
  mi.set_int = managed_method(storager, "setInt", 3);
  if (!mi.provide_item || !mi.provide_gadget || !mi.add_pet || !mi.get_int || !mi.set_int || !itemdb || !wear ||
      !pets_info)
    return -1;
  if (storager_int(UNLOCK_FLAG_KEY)) {
    LOG("[unlock] already granted for this save");
    return -1;
  }
  mi.pets_instance = call0(pets, "get_Instance");
  void *records = call0(itemdb, "get_allRecords");
  void *wear_statics = FIELD(wear, KLASS_STATIC_FIELDS, void *);
  void *wear_dict = wear_statics ? FIELD(wear_statics, WEAR_WEAR, void *) : NULL;
  void *gadget_dict = call0(gadgets, "get_info");
  void *pet_dict = call0(pets_info, "get_info");
  if (!mi.pets_instance || !records || !wear_dict || !gadget_dict || !pet_dict)
    return 0;

  for (int i = 0; i < list_size(records); i++) {
    void *r = list_get(records, i);
    int cat = r ? FIELD(r, RECORD_CATEGORY, int) - 1 : -1;
    if (cat >= 0 && cat <= 5)
      add(GRANT_ITEM, cat, FIELD(r, RECORD_TAG, void *));
  }
  int weapons = n_work;
  dict_each(wear_dict, wear_category, NULL);
  int wears = n_work - weapons;
  dict_each(gadget_dict, gadget_entry, NULL);
  int gadget_n = n_work - weapons - wears;
  dict_each(pet_dict, pet_entry, NULL);
  LOG("[unlock] granting %d weapons, %d wear, %d gadgets, %d pet levels", weapons, wears, gadget_n,
      n_work - weapons - wears - gadget_n);
  return 1;
}

static void grant_one(const grant *g) {
  int r;
  if (g->kind == GRANT_ITEM) {
    int cat = g->category, one = 1, no = 0, yes = 1;
    /* category, id, gearCount, buyArmorUpToSourceTg, timeForRentIndex, contextSpecificAction,
     * customEquipWearAction, equipSkin, equipWear, equipIfNeeded, eventWeapon, allowWearUpgrade */
    void *args[] = { &cat, g->id, &one, &no, &no, NULL, NULL, &no, &no, &no, &no, &yes };
    r = managed_invoke(mi.provide_item, NULL, args, NULL);
  } else if (g->kind == GRANT_GADGET) {
    void *args[] = { g->id };
    r = managed_invoke(mi.provide_gadget, NULL, args, NULL);
  } else {
    void *args[] = { g->id };
    r = managed_invoke(mi.add_pet, mi.pets_instance, args, NULL);
  }
  failures += r != 0;
}

static void storager_set(const char *key, int v) {
  void *k = managed_string(key);
  int no = 0;
  void *args[] = { k, &v, &no };
  managed_invoke(mi.set_int, NULL, args, NULL);
}

static int same_upgrade(void *a, void *b) {
  return FIELD(a, UPGRADE_TIER, int) == FIELD(b, UPGRADE_TIER, int) &&
         FIELD(a, UPGRADE_LEVEL, int) == FIELD(b, UPGRADE_LEVEL, int);
}

typedef struct {
  void *ids[64];
  int n;
} key_list;

static void collect_key(void *key, void *value, void *ctx) {
  key_list *keys = ctx;
  if (keys->n < 64)
    keys->ids[keys->n++] = key;
}

static int max_weapons(void) {
  void *wm = managed_class("", "WeaponManager");
  void *itemdb = managed_class("", "ItemDb");
  void *record_k = managed_class("", "ItemRecord");
  void *wu_k = managed_class("", "WeaponUpgrade");
  void *upgrade_of = managed_method(wm, "UpgradeOfWeapon", 1);
  void *increase = managed_method(wu_k, "IncreaseLevel", 2);
  void *rarity = managed_method(record_k, "get_Rarity", 0);
  void *save = managed_method(wm, "SaveWeaponLevels", 0);
  void *init_upgrading = managed_method(wm, "InitUpgradingWeapons", 0);
  void *cancel = managed_method(wm, "CancelUpgradingWeapon", 1);
  void *save_upgrading = managed_method(wm, "SaveUpgradingWeapons", 0);
  if (!upgrade_of || !increase || !rarity || !save)
    return -1;

  /* a purchase of an owned weapon starts a timed upgrade; drop those */
  if (init_upgrading && cancel && save_upgrading && !managed_invoke(init_upgrading, NULL, NULL, NULL)) {
    void *statics = FIELD(wm, KLASS_STATIC_FIELDS, void *);
    void *pending = statics ? FIELD(statics, WM_UPGRADING_WEAPONS, void *) : NULL;
    key_list keys = { .n = 0 };
    if (pending)
      dict_each(pending, collect_key, &keys);
    for (int i = 0; i < keys.n; i++) {
      void *args[] = { keys.ids[i] };
      managed_invoke(cancel, NULL, args, NULL);
    }
    if (keys.n)
      managed_invoke(save_upgrading, NULL, NULL, NULL);
    LOG("[unlock] dropped %d pending weapon upgrades", keys.n);
  }

  void *records = call0(itemdb, "get_allRecords");
  int maxed = 0, steps = 0;
  for (int i = 0; i < list_size(records); i++) {
    void *r = list_get(records, i);
    void *tag = r ? FIELD(r, RECORD_TAG, void *) : NULL;
    void *cur = NULL, *boxed = NULL;
    void *args[] = { tag };
    if (!tag || managed_invoke(upgrade_of, NULL, args, &cur) || !cur)
      continue; /* not owned */
    if (managed_invoke(managed_override(r, rarity), r, NULL, &boxed) || !boxed)
      continue;
    int grade = *(int *)managed_unbox(boxed);
    void *top = cur;
    for (int n = 0; n < MAX_STEPS; n++) {
      void *next = NULL;
      void *inc_args[] = { top, &grade };
      if (managed_invoke(increase, NULL, inc_args, &next) || !next || same_upgrade(next, top))
        break;
      top = next;
      steps++;
    }
    if (top != cur) {
      /* UpgradeOfWeapon hands out the dictionary's own object */
      FIELD(cur, UPGRADE_TIER, int) = FIELD(top, UPGRADE_TIER, int);
      FIELD(cur, UPGRADE_LEVEL, int) = FIELD(top, UPGRADE_LEVEL, int);
      maxed++;
    }
  }
  managed_invoke(save, NULL, NULL, NULL);
  LOG("[unlock] weapons: %d raised to max (%d levels)", maxed, steps);
  return 1;
}

static int invoke_int(void *method, void *obj, void **args, int *out) {
  void *boxed = NULL;
  if (managed_invoke(method, obj, args, &boxed) || !boxed)
    return -1;
  *out = *(int *)managed_unbox(boxed);
  return 0;
}

static int invoke_bool(void *method, void *obj, void **args) {
  void *boxed = NULL;
  if (managed_invoke(method, obj, args, &boxed) || !boxed)
    return 0;
  return *(uint8_t *)managed_unbox(boxed);
}

static void *klass_of(void *obj) {
  return *(void **)obj;
}

static void *statics_of(void *klass) {
  return klass ? FIELD(klass, KLASS_STATIC_FIELDS, void *) : NULL;
}

/* SaltedInt { int salt; int salted; }: value = salt ^ salted */
static int salted_get(void *p) {
  return FIELD(p, 0, int) ^ FIELD(p, 4, int);
}

static void salted_set(void *p, int v) {
  FIELD(p, 4, int) = FIELD(p, 0, int) ^ v;
}

static int max_armor(void) {
  void *wear = managed_class("", "Wear");
  void *au_k = managed_class("", "ArmorUpgrade");
  void *disable_novice = managed_method(wear, "DisableNoviceArmor", 1);
  void *get_armor = managed_method(wear, "get_CurrentPlayerArmor", 0);
  void *set_armor = managed_method(wear, "SetCurrentPlayerArmor", 1);
  void *upgrading = managed_method(wear, "get_IsUpgradingArmor", 0);
  void *cancel = managed_method(wear, "CancelUpgradingArmor", 0);
  void *increase = managed_method(au_k, "IncreaseLevel", 1);
  if (!disable_novice || !get_armor || !set_armor || !increase)
    return -1;
  int yes = 1;
  void *dn_args[] = { &yes }; /* checkStorager: only grant the first armor if there is none */
  managed_invoke(disable_novice, NULL, dn_args, NULL);
  /* buying owned armor starts a timed upgrade, which offline never finishes */
  if (upgrading && cancel && invoke_bool(upgrading, NULL, NULL)) {
    managed_invoke(cancel, NULL, NULL, NULL);
    LOG("[unlock] armor: cancelled a pending upgrade");
  }
  void *cur = NULL;
  if (managed_invoke(get_armor, NULL, NULL, &cur) || !cur) {
    LOG("[unlock] armor: no current armor after DisableNoviceArmor");
    return -1;
  }
  void *top = cur;
  for (int n = 0; n < MAX_ARMOR_STEPS; n++) {
    void *next = NULL;
    void *args[] = { top };
    if (managed_invoke(increase, NULL, args, &next) || !next || same_upgrade(next, top))
      break;
    top = next;
  }
  void *args[] = { top };
  managed_invoke(set_armor, NULL, args, NULL);
  LOG("[unlock] armor: tier %d level %d -> tier %d level %d", FIELD(cur, UPGRADE_TIER, int),
      FIELD(cur, UPGRADE_LEVEL, int), FIELD(top, UPGRADE_TIER, int), FIELD(top, UPGRADE_LEVEL, int));
  return 1;
}

static int max_pets(void) {
  void *pets = managed_class("Rilisoft", "PetsManager");
  void *pet_k = managed_class("Rilisoft", "PlayerPet");
  void *next_up = managed_method(pets, "GetNextUp", 1);
  void *upgrade = managed_method(pets, "Upgrade", 1);
  void *get_info = managed_method(pet_k, "get_Info", 0);
  void *instance = call0(pets, "get_Instance");
  if (!next_up || !upgrade || !get_info || !instance)
    return -1;
  void *list = FIELD(instance, PETSMANAGER_PLAYER_PETS, void *);
  int levels = 0;
  for (int i = 0; i < list_size(list); i++) {
    void *pet = list_get(list, i);
    for (int n = 0; pet && n < MAX_STEPS; n++) {
      void *id = FIELD(pet, PLAYERPET_INFO_ID, void *);
      void *next = NULL, *info = NULL;
      void *args[] = { id };
      if (managed_invoke(next_up, instance, args, &next) || !next)
        break; /* max level */
      if (managed_invoke(get_info, pet, NULL, &info) || !info)
        break;
      int need = FIELD(info, PETINFO_TO_UP_POINTS, int);
      if (FIELD(pet, PLAYERPET_POINTS, int) < need)
        FIELD(pet, PLAYERPET_POINTS, int) = need;
      if (managed_invoke(upgrade, instance, args, NULL) || FIELD(pet, PLAYERPET_INFO_ID, void *) == id)
        break;
      levels++;
    }
  }
  LOG("[unlock] pets: %d owned, %d levels upgraded", list_size(list), levels);
  return 1;
}

static int max_league(void) {
  void *rs_k = managed_class("", "RatingSystem");
  void *arr_k = managed_class("", "SaltedIntArray");
  void *length = managed_method(arr_k, "get_Length", 0);
  void *item = managed_method(arr_k, "get_Item", 1);
  void *get_pos = managed_method(rs_k, "get_positiveRating", 0);
  void *get_neg = managed_method(rs_k, "get_negativeRating", 0);
  void *set_pos = managed_method(rs_k, "set_positiveRating", 1);
  void *set_neg = managed_method(rs_k, "set_negativeRating", 1);
  void *update = managed_method(rs_k, "UpdateLeagueEvent", 0);
  if (!length || !item || !get_pos || !get_neg || !set_pos || !set_neg || !update)
    return -1;
  void *rs = call0(rs_k, "get_instance");
  void *ratings = rs ? FIELD(rs, RATING_LEAGUE_RATINGS, void *) : NULL;
  int n = 0, top = 0, pos = 0, neg = 0;
  if (!ratings || invoke_int(length, ratings, NULL, &n) || n <= 0)
    return 0; /* config not parsed yet */
  int last = n - 1;
  void *item_args[] = { &last };
  if (invoke_int(item, ratings, item_args, &top) || invoke_int(get_pos, rs, NULL, &pos) ||
      invoke_int(get_neg, rs, NULL, &neg))
    return -1;
  /* currentRating = positive - negative; the last threshold is the top division */
  if (pos - neg < top) {
    int zero = 0;
    void *pos_args[] = { &top }, *neg_args[] = { &zero };
    managed_invoke(set_pos, rs, pos_args, NULL);
    managed_invoke(set_neg, rs, neg_args, NULL);
  }
  managed_invoke(update, rs, NULL, NULL);
  LOG("[unlock] league: trophies %d -> %d, league %d division %d", pos - neg, top > pos - neg ? top : pos - neg,
      FIELD(rs, RATING_LEAGUE, int), FIELD(rs, RATING_DIVISION, int));
  return 1;
}

static int grant_weapon_skins(void) {
  void *k = managed_class("Rilisoft", "WeaponSkinsManager");
  void *provide = managed_method(k, "ProvideSkin", 1);
  void *bought = managed_method(k, "IsBoughtSkin", 1);
  if (!provide || !bought)
    return -1;
  void *all = call0(k, "get_AllSkins");
  if (!all)
    return -1;
  int granted = 0;
  for (int i = 0; i < list_size(all); i++) {
    void *skin = list_get(all, i);
    void *args[] = { skin ? FIELD(skin, WEAPONSKIN_ID, void *) : NULL };
    if (!args[0] || invoke_bool(bought, NULL, args))
      continue;
    granted += !managed_invoke(provide, NULL, args, NULL);
  }
  LOG("[unlock] weapon skins: %d of %d granted", granted, list_size(all));
  return 1;
}

typedef struct {
  void *has_item, *set_item;
  int seen, granted;
} skin_grant;

static void grant_skin(void *name, void *store_id, void *ctx) {
  skin_grant *g = ctx;
  void *args[] = { store_id };
  g->seen++;
  if (store_id && !invoke_bool(g->has_item, NULL, args))
    g->granted += !managed_invoke(g->set_item, NULL, args, NULL);
}

static int grant_character_skins(void) {
  void *skins = managed_class("", "SkinsController");
  void *storager = managed_class("", "Storager");
  skin_grant g = { managed_method(storager, "HasItem", 1), managed_method(storager, "SetItem", 1), 0, 0 };
  void *statics = statics_of(skins);
  void *shop_keys = statics ? FIELD(statics, SKINS_SHOP_KEYS, void *) : NULL;
  if (!g.has_item || !g.set_item)
    return -1;
  if (!shop_keys)
    return 0;
  dict_each(shop_keys, grant_skin, &g);
  LOG("[unlock] character skins: %d of %d granted", g.granted, g.seen);
  return 1;
}

static int grant_lobby_items(void) {
  void *k = managed_class("Rilisoft", "LobbyItemsController");
  void *add_now = managed_method(k, "AddItemNow", 1);
  if (!add_now)
    return -1;
  void *lobby = call0(k, "get_Instance");
  void *statics = statics_of(managed_class("", "FriendsController"));
  /* AddItemNow fails without FriendsController.ServerTime to stamp the craft with */
  if (!lobby || !FIELD(lobby, LOBBY_IS_READY, uint8_t) || !statics || !FIELD(statics, FRIENDS_SERVER_TIME_SET, uint8_t))
    return 0;
  void *items = FIELD(lobby, LOBBY_ALL_ITEMS, void *);
  int granted = 0;
  for (int i = 0; i < list_size(items); i++) {
    void *item = list_get(items, i);
    void *info = item ? FIELD(item, LOBBYITEM_PLAYER_INFO, void *) : NULL;
    if (!item || (info && FIELD(info, LOBBYPLAYER_IS_CRAFTED, uint8_t)))
      continue;
    void *args[] = { item };
    granted += invoke_bool(add_now, lobby, args);
  }
  LOG("[unlock] lobby items: %d of %d added", granted, list_size(items));
  return 1;
}

static int max_achievements(void) {
  void *k = managed_class("Rilisoft", "AchievementsManager");
  void *save = managed_method(k, "SaveProgresses", 0);
  if (!save)
    return -1;
  void *manager = call0(k, "get_Instance");
  if (!manager || !FIELD(manager, ACHMANAGER_READY, uint8_t))
    return 0;
  void *all = FIELD(manager, ACHMANAGER_ALL, void *);
  int raised = 0;
  for (int i = 0; i < list_size(all); i++) {
    void *ach = list_get(all, i);
    void *data = ach ? FIELD(ach, ACH_DATA, void *) : NULL;
    void *progress = ach ? FIELD(ach, ACH_PROGRESS, void *) : NULL;
    void *thresholds = data ? FIELD(data, ACHDATA_THRESHOLDS, void *) : NULL;
    int stages = thresholds ? FIELD(thresholds, ARRAY_LENGTH, int) : 0;
    if (!progress || stages <= 0)
      continue;
    /* Achievement.IncrementStageIfNeeded: stage counts the thresholds points has reached */
    int top = ((int *)((uint8_t *)thresholds + ARRAY_DATA))[stages - 1];
    void *points = (uint8_t *)progress + ACHPROGRESS_POINTS, *stage = (uint8_t *)progress + ACHPROGRESS_STAGE;
    if (salted_get(points) >= top && salted_get(stage) >= stages)
      continue;
    if (salted_get(points) < top)
      salted_set(points, top);
    salted_set(stage, stages);
    raised++;
  }
  managed_invoke(save, manager, NULL, NULL);
  LOG("[unlock] achievements: %d of %d completed", raised, list_size(all));
  return 1;
}

static int dict_contains(void *dict, void *key) {
  void *args[] = { key };
  return invoke_bool(managed_method(klass_of(dict), "ContainsKey", 1), dict, args);
}

static void *dict_get(void *dict, void *key) {
  void *args[] = { key }, *r = NULL;
  managed_invoke(managed_method(klass_of(dict), "get_Item", 1), dict, args, &r);
  return r;
}

static void dict_set_int(void *dict, void *key, int v) {
  void *args[] = { key, &v };
  managed_invoke(managed_method(klass_of(dict), "set_Item", 2), dict, args, NULL);
}

static int max_campaign(void) {
  void *box_k = managed_class("", "LevelBox");
  void *cp_k = managed_class("", "CampaignProgress");
  void *open = managed_method(cp_k, "OpenNewBoxIfPossible", 0);
  void *save = managed_method(cp_k, "SaveCampaignProgress", 0);
  void *by_scene = managed_method(box_k, "GetLevelBySceneName", 1);
  if (!open || !save || !by_scene)
    return -1;
  void *none_args[] = { managed_string("") };
  managed_invoke(by_scene, NULL, none_args, NULL); /* runs LevelBox's static constructor */
  managed_invoke(open, NULL, NULL, NULL);
  void *box_statics = statics_of(box_k), *cp_statics = statics_of(cp_k);
  void *boxes = box_statics ? FIELD(box_statics, 0, void *) : NULL;  /* campaignBoxes */
  void *progress = cp_statics ? FIELD(cp_statics, 0, void *) : NULL; /* boxesLevelsAndStars */
  if (!boxes || !progress)
    return 0;
  int opened = 0, starred = 0;
  for (int i = 0; i < list_size(boxes); i++) {
    void *box = list_get(boxes, i);
    void *name = box ? FIELD(box, LEVELBOX_NAME, void *) : NULL;
    void *levels = box ? FIELD(box, LEVELBOX_LEVELS, void *) : NULL;
    if (!name || !list_size(levels))
      continue;
    if (!dict_contains(progress, name)) {
      managed_invoke(open, NULL, NULL, NULL); /* opens the box after the completed ones */
      if (!dict_contains(progress, name))
        break;
      opened++;
    }
    void *stars = dict_get(progress, name);
    for (int l = 0; stars && l < list_size(levels); l++) {
      void *level = list_get(levels, l);
      void *scene = level ? FIELD(level, CAMPAIGNLEVEL_SCENE, void *) : NULL;
      if (!scene)
        continue;
      void *boxed = dict_contains(stars, scene) ? dict_get(stars, scene) : NULL;
      if (boxed && *(int *)managed_unbox(boxed) >= CAMPAIGN_STARS)
        continue;
      dict_set_int(stars, scene, CAMPAIGN_STARS);
      starred++;
    }
  }
  managed_invoke(save, NULL, NULL, NULL);
  LOG("[unlock] campaign: %d boxes opened, %d levels given %d stars", opened, starred, CAMPAIGN_STARS);
  return 1;
}

typedef struct {
  void *ids[256];
  int n;
} id_list;

static void collect_id(void *key, void *value, void *ctx) {
  id_list *l = ctx;
  if (l->n < 256)
    l->ids[l->n++] = key;
}

static int max_details(void) {
  void *k = managed_class("Rilisoft", "DetailsShopCraftInfo");
  void *add = managed_method(k, "AddDetails", 1);
  void *ours_m = managed_method(k, "get_OurDetails", 0);
  if (!add || !ours_m)
    return -1;
  void *ours = NULL;
  if (managed_invoke(ours_m, NULL, NULL, &ours) || !ours)
    return -1;
  void *statics = statics_of(k);
  void *names = statics ? FIELD(statics, 0, void *) : NULL; /* detailsAndLocalizations */
  if (!names)
    return 0;
  static id_list ids;
  int have[256];
  ids.n = 0;
  dict_each(names, collect_id, &ids);
  for (int i = 0; i < ids.n; i++) {
    void *boxed = dict_contains(ours, ids.ids[i]) ? dict_get(ours, ids.ids[i]) : NULL;
    have[i] = boxed ? *(int *)managed_unbox(boxed) : 0;
  }
  /* OurDetails is a fresh copy: reuse it as the amounts to add */
  managed_invoke(managed_method(klass_of(ours), "Clear", 0), ours, NULL, NULL);
  int raised = 0;
  for (int i = 0; i < ids.n; i++)
    if (have[i] < DETAILS_TARGET) {
      dict_set_int(ours, ids.ids[i], DETAILS_TARGET - have[i]);
      raised++;
    }
  void *args[] = { ours };
  if (raised)
    managed_invoke(add, NULL, args, NULL);
  LOG("[unlock] craft parts: %d of %d raised to %d", raised, ids.n, DETAILS_TARGET);
  return 1;
}

typedef int (*pass_fn)(void);

static const struct {
  const char *name;
  pass_fn fn;
} passes[] = {
  { "weapons", max_weapons },
  { "armor", max_armor },
  { "pets", max_pets },
  { "league", max_league },
  { "weapon skins", grant_weapon_skins },
  { "character skins", grant_character_skins },
  { "lobby items", grant_lobby_items },
  { "achievements", max_achievements },
  { "campaign", max_campaign },
  { "craft parts", max_details },
};

#define N_PASSES (int)(sizeof(passes) / sizeof(passes[0]))

/* 1 done or skipped, 0 some pass not ready yet (retry next frame) */
static int max_upgrades(int last_try) {
  static int done, failed, started;
  if (!started) {
    if (storager_int(MAXED_FLAG_KEY)) {
      LOG("[unlock] already maxed for this save");
      return 1;
    }
    if (!call0(managed_class("Rilisoft", "PetsManager"), "get_Instance") ||
        !call0(managed_class("", "ItemDb"), "get_allRecords"))
      return last_try;
    started = 1;
  }
  for (int i = 0; i < N_PASSES; i++) {
    if (done & 1 << i)
      continue;
    int64_t start = bionic_monotonic_us();
    int r = passes[i].fn();
    if (r == 0 && !last_try)
      continue;
    if (r != 1) {
      failed |= 1 << i;
      LOG("[unlock] %s: %s", passes[i].name, r ? "failed" : "never became ready");
    } else if (bionic_monotonic_us() - start > 100000) {
      LOG("[unlock] %s took %lld ms", passes[i].name, (bionic_monotonic_us() - start) / 1000);
    }
    done |= 1 << i;
  }
  if (done != (1 << N_PASSES) - 1)
    return 0;
  if (!failed)
    storager_set(MAXED_FLAG_KEY, 1);
  LOG("[unlock] max-out pass %s", failed ? "partly done" : "done");
  return 1;
}

/* TEMP: the shop shows no Equip button for throwing gadgets; log their state */
static void ascii(void *str, char *out, int n) {
  int len = str ? FIELD(str, 8, int) : 0, i;
  for (i = 0; i < len && i < n - 1; i++) {
    uint16_t c = ((uint16_t *)((uint8_t *)str + 12))[i];
    out[i] = c < 128 ? c : '?';
  }
  out[i] = 0;
}

static void actualize_gadgets(void) {
  void *m = managed_method(managed_class("", "GadgetsInfo"), "ActualizeEquippedGadgets", 0);
  if (m)
    managed_invoke(m, NULL, NULL, NULL);
}

/* TEMP: what the shop compares when it decides on the equip button */
typedef void (*state_buttons_fn)(void *self, void *viewed, void *show_for, void *container, int from_main,
                                 void *item, void *method);
static state_buttons_fn state_buttons_orig;

static void state_buttons(void *self, void *viewed, void *show_for, void *container, int from_main, void *item,
                          void *method) {
  state_buttons_orig(self, viewed, show_for, container, from_main, item, method);
  char v[64], s[64], l[64];
  ascii(viewed, v, sizeof(v));
  if (strncmp(v, "gadget_", 7))
    return;
  void *gi = managed_class("", "GadgetsInfo");
  void *args[] = { viewed }, *last = NULL, *go = NULL, *active = NULL;
  managed_invoke(managed_method(gi, "LastBoughtFor", 1), NULL, args, &last);
  ascii(show_for, s, sizeof(s));
  ascii(last, l, sizeof(l));
  void *button = container ? FIELD(container, 0xB8, void *) : NULL;
  if (button && !managed_invoke(managed_method(managed_class("UnityEngine", "Component"), "get_gameObject", 0), button,
                                NULL, &go) && go)
    managed_invoke(managed_method(managed_class("UnityEngine", "GameObject"), "get_activeSelf", 0), go, NULL, &active);
  LOG("[gadget] view %s for %s main %d item %p: last bought %s, equip button %s", v, show_for ? s : "-", from_main & 1,
      item, last ? l : "-", active ? (*(uint8_t *)managed_unbox(active) ? "shown" : "hidden") : "?");
}

/* TEMP */
static void log_gadgets(void) {
  void *equipped = managed_method(managed_class("", "GadgetsInfo"), "EquippedForCategory", 1);
  if (!equipped)
    return;
  static const int cats[] = { 12500, 13000, 13500 };
  for (int i = 0; i < 3; i++) {
    int cat = cats[i];
    void *args[] = { &cat }, *e = NULL;
    char a[64];
    managed_invoke(equipped, NULL, args, &e);
    ascii(e, a, sizeof(a));
    LOG("[gadget] equipped for %d: %s", cat, e ? a : "-");
  }
}

static void menu_update(void *self, void *method) {
  static int tries;
  if (state == 0) {
    int r = build();
    if (r == 0 && ++tries >= 600) {
      LOG("[unlock] game tables never became ready, not granting");
      r = -1;
    }
    if (r != 0)
      state = r > 0 ? 1 : 2;
  }
  if (state == 1) {
    int64_t start = bionic_monotonic_us();
    while (next_work < n_work)
      grant_one(&work[next_work++]);
    storager_set(UNLOCK_FLAG_KEY, 1);
    LOG("[unlock] done in %lld ms: %d granted, %d failed", (bionic_monotonic_us() - start) / 1000,
        n_work - failures, failures);
    free(work);
    work = NULL;
    state = 2;
  }
  if (state == 2 && !maxed_state) {
    static int maxed_tries;
    if (!maxed_tries) {
      actualize_gadgets();
      log_gadgets();
    }
    if (!mi.get_int || !mi.set_int || max_upgrades(++maxed_tries >= 600))
      maxed_state = 1;
  }
  menu_update_orig(self, method);
}

void unlock_patch(void) {
  so_module *m = gamebin_il2cpp;
  menu_update_orig = gamebin_trampoline(m, m->base + GB_IL2CPP_MAINMENU_UPDATE);
  if (!menu_update_orig) {
    LOG("[unlock] trampoline allocation failed");
    return;
  }
  so_hook_arm(m->base + GB_IL2CPP_MAINMENU_UPDATE, (const void *)menu_update);
  state_buttons_orig = gamebin_trampoline(m, m->base + GB_IL2CPP_SHOP_STATE_BUTTONS);
  if (state_buttons_orig)
    so_hook_arm(m->base + GB_IL2CPP_SHOP_STATE_BUTTONS, (const void *)state_buttons);
}
