// Gripline Lite for Forza Horizon 1: ReImagined - time of day and how fast it runs.
// Copyright (c) 2026 Gripline contributors. SPDX-License-Identifier: GPL-3.0-or-later
//
// TU4 (as ReImagined's tools/render_test.py and docs/render-tests.md describe it): the world is
// *(*(*0x833CAAE4 + 4) + 4) and its time-of-day object *(world + 232), whose first word points
// back at the world. The clock is a float of seconds after midnight at +10488; the game advances
// it every frame at its own rate (+10492, about 28 game seconds per second by day, 40 at night,
// set by the game every frame). Lighting, sky and the traffic's day/night limits follow the clock.
//
// Free roam offline only (the title-published presence is 1, kernel/live.h LiveLocalActivity):
// races, Rivals, menus and online sessions keep the game's own time.
// - A chosen time of day is set when it is picked in the Gripline menu (F6) and again whenever
//   free roam starts (after loading or a race).
// - The speed scales how far the clock moved since the last frame: stopped holds it, slow is a
//   quarter, fast four times the game's own pace.
// FH1 has no weather system (ReImagined's docs: "FH1 has no weather"), so there is no weather.
#include "gripline_settings.h"

#include "../kernel/live.h"
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace forza::gripline {

namespace {
// Seconds after midnight; the same times as ReImagined's Custom Race (platform/custom_race.h).
constexpr float kTimes[] = {0.0f,  // 0: the game's own time
                            23400.0f, 32400.0f, 45000.0f, 55800.0f, 70200.0f, 86100.0f};
constexpr int kTimeCount = int(sizeof(kTimes) / sizeof(kTimes[0]));
constexpr float kSpeeds[] = {1.0f, 0.25f, 0.0f, 4.0f};  // normal, slow, stopped, fast
constexpr int kSpeedCount = int(sizeof(kSpeeds) / sizeof(kSpeeds[0]));
constexpr float kDay = 86400.0f;

std::atomic<bool> jump_request{false};
std::atomic<float> shown_clock{-1.0f};

bool Valid(uint32_t a) { return a >= 0x00100000 && a < 0xF0000000 && !(a & 3); }
uint32_t Load32(uint8_t* base, uint32_t a) {
  const auto* p = forza::GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float LoadFloat(uint8_t* base, uint32_t a) {
  const uint32_t bits = Load32(base, a);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}
void StoreFloat(uint8_t* base, uint32_t a, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  uint8_t* p = forza::GuestPtr(base, a);
  p[0] = uint8_t(bits >> 24), p[1] = uint8_t(bits >> 16), p[2] = uint8_t(bits >> 8), p[3] = uint8_t(bits);
}

// The clock's guest address, or 0 while there is no world (title screen, loading).
uint32_t ClockAddress(uint8_t* base) {
#if FORZA_TU4
  const uint32_t root = Load32(base, 0x833CAAE4);
  const uint32_t holder = Valid(root) ? Load32(base, root + 4) : 0;
  const uint32_t world = Valid(holder) ? Load32(base, holder + 4) : 0;
  const uint32_t tod = Valid(world) ? Load32(base, world + 232) : 0;
  if (!Valid(tod) || Load32(base, tod) != world) return 0;
  const uint32_t clock = tod + 10488;
  const float v = LoadFloat(base, clock);
  return std::isfinite(v) && v >= 0.0f && v <= kDay ? clock : 0;
#else
  (void)base;
  return 0;
#endif
}

bool OfflineFreeRoam() {
  uint32_t presence = 0, region = 0;
  return forza::LiveLocalActivity(presence, region) && presence == 1;
}
}  // namespace

int TimeChoiceCount() { return kTimeCount; }
int TimeSpeedCount() { return kSpeedCount; }
float TimeChoiceSeconds(int choice) { return kTimes[std::clamp(choice, 0, kTimeCount - 1)]; }
void RequestTimeJump() { jump_request = true; }
float ShownClock() { return shown_clock.load(std::memory_order_relaxed); }

void ApplyTime(uint8_t* base) {
  static uint32_t last_clock = 0;   // the clock's address last frame (0: none / not ours)
  static float last_value = -1.0f;  // what it read (or we wrote) last frame
  static bool logged = false;
  const uint32_t clock = ClockAddress(base);
  if (!clock) {
    last_clock = 0;
    shown_clock.store(-1.0f, std::memory_order_relaxed);
    return;
  }
  float now = LoadFloat(base, clock);
  shown_clock.store(now, std::memory_order_relaxed);
  if (!OfflineFreeRoam()) {
    last_clock = 0;  // the game's own time; set the chosen time again back in free roam
    return;
  }
  auto& g = Get();
  const int choice = std::clamp(g.time_of_day.load(), 0, kTimeCount - 1);
  const float speed = kSpeeds[std::clamp(g.time_speed.load(), 0, kSpeedCount - 1)];
  const bool entered = clock != last_clock || last_value < 0.0f;
  float moved = 0.0f;
  if (!entered) {
    moved = now - last_value;
    if (moved < -kDay / 2) moved += kDay;  // past midnight
  }
  // The game's own frame steps are under a game minute; anything else is the game setting the
  // clock itself (loading a save sets its time a moment after free roam starts).
  const bool game_set = !entered && (moved < 0.0f || moved >= 60.0f);
  const bool jump = jump_request.exchange(false) || entered || game_set;
  if (jump && choice > 0) {
    now = kTimes[choice];
    StoreFloat(base, clock, now);
    REXLOG_INFO("Gripline: time of day set to {:02d}:{:02d}", int(now) / 3600, int(now) % 3600 / 60);
  } else if (!entered && !game_set && speed != 1.0f) {
    float want = last_value + moved * speed;
    if (want >= kDay) want -= kDay;
    if (want != now) StoreFloat(base, clock, want);
    now = want;
  }
  if (!logged) {
    logged = true;
    REXLOG_INFO("Gripline: time of day clock found at {:08X} ({:.0f} s)", clock, now);
  }
  last_clock = clock;
  last_value = now;
}

}  // namespace forza::gripline
