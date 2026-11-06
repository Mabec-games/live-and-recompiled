#pragma once

// In-engine cutscene skipping (src/conker/game/hooks.cpp, the hook on 0x4FF00).
namespace cutscenes {

// settings.toml skippable_cutscenes: LT skips any in-engine cutscene, seen or not, not just the ones seen this
// playthrough; XBE_SKIPPABLE_CUTSCENES turns it on as well. Read on each test, so the F1 menu applies it live.
extern bool g_skippable;

// settings.toml cutscene_volume: the in-engine cutscenes louder by this many dB (0 to 12, 0 as the game has them),
// while one plays: the hook on 0x4FF00, which the game asks each frame of one, saw it in the last 0.3 s
extern uint32_t g_volume_db;
// the gain for audio::g_title_gain
float AudioGain();

// an in-engine cutscene plays: the game asked 0x4FF00 (each frame of one) in the last 0.25 s
bool Playing();

}  // namespace cutscenes
