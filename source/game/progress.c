/*
 * The port is offline, so start at the end of progression: training done,
 * player at the level cap and top tier, currencies topped up. Done by
 * replacing a few managed getters in libil2cpp; items themselves are granted
 * for real by game/unlock.c. In this IL2CPP generation a
 * static method takes a dummy this pointer first and a MethodInfo* last.
 */
#include <stdint.h>
#include <string.h>

#include "game/gamebin.h"
#include "util/log.h"

#define MAX_LEVEL 70 /* ExperienceController.maxLevel */
#define CURRENCY 999999

typedef struct {
  void *klass, *monitor;
  int32_t length;
  uint16_t chars[];
} il2cpp_string;

static int str_is(const il2cpp_string *s, const char *ascii) {
  size_t n = strlen(ascii);
  if (!s || s->length != (int32_t)n)
    return 0;
  for (size_t i = 0; i < n; i++)
    if (s->chars[i] != (uint8_t)ascii[i])
      return 0;
  return 1;
}

static int ret_true(void) {
  return 1;
}

static int training_stage(void *method) {
  return 3; /* NewTrainingCompletedStage.FirstMatchCompleted */
}

static int max_level(void) {
  return MAX_LEVEL;
}

typedef int (*tier_for_level_fn)(void *, int, void *);
static tier_for_level_fn tier_for_level;

/* Shop, pets, gadgets and armor lock items above the player's tier */
static int max_tier(void) {
  return tier_for_level(NULL, MAX_LEVEL, NULL);
}

/* TEMP: distinct Storager keys, to find remaining progression gates */
typedef struct {
  uint32_t seen[1024];
  int n;
} key_log;

static void log_key(key_log *t, const char *what, const il2cpp_string *key, int v) {
  uint32_t *seen = t->seen;
  if (!key || t->n >= 1024)
    return;
  uint32_t h = 2166136261u;
  for (int i = 0; i < key->length; i++)
    h = (h ^ key->chars[i]) * 16777619u;
  for (int i = 0; i < t->n; i++)
    if (seen[i] == h)
      return;
  seen[t->n++] = h;
  char a[96];
  int i;
  for (i = 0; i < key->length && i < 95; i++)
    a[i] = key->chars[i] < 128 ? key->chars[i] : '?';
  a[i] = 0;
  LOG("[store] %s %s = %d", what, a, v);
}

typedef int (*getint_fn)(void *, const il2cpp_string *, int, int, void *);
static getint_fn getint_orig;
static key_log getint_keys, hasitem_keys;

typedef int (*hasitem_fn)(void *, const il2cpp_string *, void *);
static hasitem_fn hasitem_orig;

/* Ownership is real now (game/unlock.c); log what is still missing */
static int storager_hasItem(void *unused, const il2cpp_string *id, void *method) {
  int v = hasitem_orig(unused, id, method);
  if (!v)
    log_key(&hasitem_keys, "HasItem", id, v);
  return v;
}

static int storager_getInt(void *unused, const il2cpp_string *key, int suppress, int direct, void *method) {
  if (str_is(key, "Coins") || str_is(key, "GemsCurrency") || str_is(key, "TicketsCurrency") ||
      str_is(key, "EventCurrency"))
    return CURRENCY;
  /* ExperienceController.currentLevelKey: the saved level everything reads */
  if (str_is(key, "currentLevelKey"))
    return MAX_LEVEL;
  int v = getint_orig(unused, key, suppress, direct, method);
  log_key(&getint_keys, "getInt", key, v);
  return v;
}

void progress_patch(void) {
  so_module *m = gamebin_il2cpp;
  uintptr_t b = m->base;
  so_hook_arm(b + GB_IL2CPP_TRAINING_COMPLETED, (const void *)ret_true);
  so_hook_arm(b + GB_IL2CPP_TRAINING_STAGE, (const void *)training_stage);
  so_hook_arm(b + GB_IL2CPP_EXP_PLAYERLEVEL, (const void *)max_level);
  so_hook_arm(b + GB_IL2CPP_EXP_GETCURRENTLEVEL, (const void *)max_level);
  so_hook_arm(b + GB_IL2CPP_EXP_LEVEL_CORRECTED, (const void *)max_level);
  so_hook_arm(b + GB_IL2CPP_EXP_CURRENTLEVEL, (const void *)max_level);
  tier_for_level = (tier_for_level_fn)(b + GB_IL2CPP_EXPC_TIERFORLEVEL);
  so_hook_arm(b + GB_IL2CPP_EXPC_OURTIER, (const void *)max_tier);
  so_hook_arm(b + GB_IL2CPP_EXPC_GETOURTIER, (const void *)max_tier);
  so_hook_arm(b + GB_IL2CPP_EXPC_OURTIERANYPLACE, (const void *)max_tier);
  hasitem_orig = gamebin_trampoline(m, b + GB_IL2CPP_STORAGER_HASITEM);
  if (hasitem_orig)
    so_hook_arm(b + GB_IL2CPP_STORAGER_HASITEM, (const void *)storager_hasItem);
  getint_orig = gamebin_trampoline(m, b + GB_IL2CPP_STORAGER_GETINT);
  if (getint_orig)
    so_hook_arm(b + GB_IL2CPP_STORAGER_GETINT, (const void *)storager_getInt);
  LOG("[progress] training skipped, level %d, max tier, currencies %d%s", MAX_LEVEL, CURRENCY,
      getint_orig ? "" : " (getInt hook failed)");
}
