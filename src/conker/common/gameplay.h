#pragma once

// Gameplay options (src/conker/game/gameplay.cpp).
namespace gameplay {

// settings.toml bat_10x_damage (overpowered_pan before): everything Conker hits takes 10x damage (bat, melee, guns,
// explosives) (story only, not in
// multiplayer). Read on each hit, so the F1 menu applies it live.
extern bool g_bat_10x_damage;

}  // namespace gameplay
