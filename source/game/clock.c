/*
 * FriendsController keeps the game's one clock: get_ServerTime is the server's
 * Unix time, -1 until FriendsController.GetTimeFromServer has an answer, and
 * Update ticks it once a second after that. Offline the request is never even
 * made (InitController stops at account creation), so everything timed (lobby
 * crafts, eggs, gifts, upgrades, offers) waits forever. Use the console's UTC
 * clock instead, set the way GetTimeFromServer's success path sets it, and
 * resync when the two drift apart (Update doesn't run while suspended).
 */
#include <stdint.h>

#include "bionic/bionic.h"
#include "game/gamebin.h"
#include "game/managed.h"
#include "util/log.h"

#define KLASS_STATIC_FIELDS 0x4C
#define FRIENDS_TIME_UPDATED 0x38   /* static Action ServerTimeUpdated */
#define FRIENDS_SERVER_TIME 0x50    /* static long localServerTime */
#define FRIENDS_SERVER_TICK 0x58    /* static float tickForServerTime */
#define FRIENDS_SERVER_TIME_SET 0x5C /* static bool isUpdateServerTimeAfterRun */
#define LAST_VISITED_KEY "_last-visited-time-in-seconds"
#define MAX_DRIFT 2

#define FIELD(o, off, type) (*(type *)((uint8_t *)(o) + (off)))

typedef void (*void_fn)(void *self, void *method);
static void_fn friends_update_orig;

static struct {
  void *friends, *has_key, *get_int, *set_int, *set_last_visited;
} mi;

static int resolve(void) {
  if (mi.friends)
    return 1;
  void *storager = managed_class("", "Storager");
  void *friends = managed_class("", "FriendsController");
  mi.has_key = managed_method(storager, "hasKey", 1);
  mi.get_int = managed_method(storager, "getInt", 3);
  mi.set_int = managed_method(storager, "setInt", 3);
  mi.set_last_visited = managed_method(friends, "set_LastVisitedInterval", 1);
  if (!friends || !mi.has_key || !mi.get_int || !mi.set_int || !mi.set_last_visited)
    return 0;
  mi.friends = friends;
  return 1;
}

/* What GetTimeFromServer does with a parsed answer, bar the request */
static void first_answer(int64_t now) {
  void *key = managed_string(LAST_VISITED_KEY), *boxed = NULL;
  int64_t interval = 0;
  void *one[] = { key };
  if (!managed_invoke(mi.has_key, NULL, one, &boxed) && boxed && *(uint8_t *)managed_unbox(boxed)) {
    int no = 0;
    void *args[] = { key, &no, &no };
    boxed = NULL;
    if (!managed_invoke(mi.get_int, NULL, args, &boxed) && boxed)
      interval = now - *(int *)managed_unbox(boxed);
  }
  void *iv[] = { &interval };
  managed_invoke(mi.set_last_visited, NULL, iv, NULL);
  int stamp = (int)now, no = 0;
  void *set[] = { key, &stamp, &no };
  managed_invoke(mi.set_int, NULL, set, NULL);
}

static void sync_clock(void) {
  void *statics = FIELD(mi.friends, KLASS_STATIC_FIELDS, void *);
  if (!statics)
    return;
  int64_t now = bionic_realtime_us() / 1000000;
  int set = FIELD(statics, FRIENDS_SERVER_TIME_SET, uint8_t);
  int64_t drift = now - FIELD(statics, FRIENDS_SERVER_TIME, int64_t);
  if (set && drift >= -MAX_DRIFT && drift <= MAX_DRIFT)
    return;
  static int answered;
  FIELD(statics, FRIENDS_SERVER_TIME, int64_t) = now;
  if (!answered) {
    answered = 1;
    first_answer(now);
  }
  FIELD(statics, FRIENDS_SERVER_TICK, float) = 0;
  FIELD(statics, FRIENDS_SERVER_TIME_SET, uint8_t) = 1;
  if (set) {
    LOG("[clock] server time resynced (%lld s off)", (long long)drift);
    return;
  }
  LOG("[clock] server time set from the console clock: %lld", (long long)now);
  /* GetTimeFromServerLoop raises this after each answer */
  void *updated = FIELD(statics, FRIENDS_TIME_UPDATED, void *);
  if (updated)
    managed_invoke(managed_method(*(void **)updated, "Invoke", 0), updated, NULL, NULL);
}

/* After Update: it adds the frame's delta to the tick, which would count a
 * long frame twice right after a resync */
static void friends_update(void *self, void *method) {
  friends_update_orig(self, method);
  if (resolve())
    sync_clock();
}

void clock_patch(void) {
  so_module *m = gamebin_il2cpp;
  friends_update_orig = gamebin_trampoline(m, m->base + GB_IL2CPP_FRIENDS_UPDATE);
  if (!friends_update_orig) {
    LOG("[clock] trampoline allocation failed");
    return;
  }
  so_hook_arm(m->base + GB_IL2CPP_FRIENDS_UPDATE, (const void *)friends_update);
}
