#pragma once

#include <cstdint>

namespace input {
struct Pad;
}

// First-person POV toggle (src/conker/game/fpv.cpp)
namespace fpv {

extern bool g_allow_toggle;    // the option: LT / Q toggles first-person view
extern bool g_active;          // first-person view on now
extern float g_eye_height;     // eye above Conker's feet (78.5, the game's own LT look mode)
extern float g_forward_offset; // eye forward of his centre along the view
extern float g_pitch_limit;    // degrees first person looks up and down at most
extern uint32_t g_look_speed;  // settings.toml fpv_look_speed: right stick turn in first person, percent (120)
extern float g_item_forward;   // what he holds, moved forward along the view out of the lens
extern float g_item_down;      // and down

// a cutscene or scripted moment: the game's own camera, FPV kept for after it
bool Paused();
// first-person view drawn now: on and not paused
bool Viewing();

// singleplayer gameplay running (no frontend or load)
bool IsInGameplay();

// 0x2F99A0's stick turn (actions, turn) scaled by g_look_speed in first person (mouse.cpp's hook calls it)
void BoostTurn(uint32_t actions, uint32_t turn);

// each port's pad as the game reads it (keyboard.cpp, after Q is mapped to LT): an LT press toggles FPV, and LT is
// kept from the game in gameplay (its look mode stops Conker)
void Pad(int port, input::Pad& pad);

// around 0x2F99A0 in first person: the camera's pitch limits opened for the call, then put back
bool BeginTurn(uint32_t actions, uint32_t turn, float saved[2]);
void EndTurn(uint32_t turn, const float saved[2]);

// after 0x2F99A0 and the mouse: the frame's pitch turn, for first person's own pitch (mouse.cpp's hook calls it)
float PitchTurn(uint32_t turn);  // the turn's frame pitch turn, +0x44
void NotePitch(uint32_t actions, uint32_t turn, float stick);  // stick: +0x44 before the mouse was added

// Conker's facing to the view's yaw (degrees) while he stands still
void UpdateFacing(uint32_t actor, float view_yaw);

}  // namespace fpv
