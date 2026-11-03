// The game's own controller options, Invert Aim and Vibration, taken over by the settings menu: each player's are set
// before the game reads them, and its two Options pages, the pause menu's and the frontend's, open the settings menu
// instead.
#include <algorithm>
#include <cstdint>

#include "cpu/x86_ops.h"
#include "kernel/memory.h"
#include "xdk/hle.h"
#include "core/log.h"

#include "common/gamepad.h"
#include "common/menus.h"
#include "common/mouse.h"
#include "common/ui.h"
#include "common/warp.h"

// 0x4D1F0: player edx's control scheme (*eax), invert aim (*edi, which the scheme 0x4D470/0x4D6E0 takes to negate
// the stick's y) and vibration (0x806FE0 + port*0x14, which 0x3D2910 needs set to start a rumble). Out of Xbox Live
// they come from the bytes the pause and frontend menus show, INVERT_AIM at 0x84B140 (zero inverts) and VIBRATION at
// 0x84B141 (non-zero rumbles), one pair for all players, kept in the save (the 0x118 bytes at 0x84B130, new game
// defaults by 0x352410). The function runs for each player in turn (0x4D320, the player whose actions struct
// 0x763260 + edx*0x1B0 it fills), so the player's own settings go into the bytes before each read; the game's menus
// and a save loaded don't change them.
// In multiplayer ([0x7819D5] set) it reads neither byte: players 1 and 2 take theirs from their multiplayer profile
// ([0x862CD0 + edx*4]: scheme +0x11B4, invert +0x11B5, vibration +0x11B6, made by 0x351D90 with invert off) and
// players 3 and 4 get invert off. Multiplayer's control schemes (0x68C308) take *edi the other way round from single
// player's (0x8470F8): 0 there already turns the stick's up into looking down, so the game's own default inverted the
// multiplayer camera (issue #13). So the player's invert is written into *edi once it returns, 1 in single player and 0
// in multiplayer for invert_aim on, and their vibration is applied where the motors are set (Rumble): invert_aim and
// vibration are one setting each, and up looks up in both modes unless invert_aim is on.
bool gamepad::g_invert_aim[input::kPorts] = {};
bool gamepad::g_vibration[input::kPorts] = {true, true, true, true};
uint32_t gamepad::g_vibration_percent[input::kPorts] = {100, 100, 100, 100};

// the motors' speeds the game sets (XInputSetState, the player's port) scaled; a change applies from the next rumble
void gamepad::Rumble(int port, uint16_t& left, uint16_t& right) {
    if (port < 0 || port >= input::kPorts) return;
    if (!g_vibration[port]) {  // multiplayer starts rumbles from its profile's vibration, not the bytes above
        left = right = 0;
        return;
    }
    const uint32_t percent = std::min(g_vibration_percent[port], 100u);
    left = uint16_t(uint32_t(left) * percent / 100);
    right = uint16_t(uint32_t(right) * percent / 100);
}

X86_EXTERN_FUNC(__imp__sub_0004D1F0);
// the path 0x4D1F0 takes: multiplayer's when [0x7819D5] is set, unless the player's entry in the table at
// [0x84A5F8] (0x6D0 bytes each, +0x564) points at a zero byte
static bool MultiplayerPath(uint32_t player) {
    const uint8_t* m = mem::Base();
    if (!m[0x7819D5]) return false;
    const uint32_t entry = kernel::R32(kernel::R32(0x84A5F8) + player * 0x6D0 + 0x564);
    return !(entry && !m[entry]);
}

X86_FUNC(sub_0004D1F0) {
    const uint32_t player = ctx.edx;
    const uint32_t invert = ctx.edi;  // the byte it fills with the player's invert
    if (player < uint32_t(input::kPorts)) {
        base[0x84B140] = gamepad::g_invert_aim[player] ? 0 : 1;
        base[0x84B141] = gamepad::g_vibration[player] ? 1 : 0;
    }
    const bool multiplayer = MultiplayerPath(player);
    __imp__sub_0004D1F0(ctx, base);
    if (player < uint32_t(input::kPorts)) {
        const uint8_t value = (gamepad::g_invert_aim[player] != multiplayer) ? 1 : 0;
        base[invert] = value;
        // each player's when it changes: which path, what the setting is, what the scheme gets (1 normal in
        // multiplayer, inverted in single player)
        static int last[input::kPorts] = {-1, -1, -1, -1};
        const int now = (multiplayer ? 4 : 0) | (gamepad::g_invert_aim[player] ? 2 : 0) | value;
        if (last[player] != now) {
            last[player] = now;
            LOGR("input: player %u invert aim %s, %s, the scheme gets %u (up looks %s)", player + 1,
                 gamepad::g_invert_aim[player] ? "on" : "off", multiplayer ? "multiplayer" : "single player", value,
                 gamepad::g_invert_aim[player] ? "down" : "up");
        }
    }
}

// 0x203B50: the pause menu's input, once a frame while paused (0x204060). Its page is [0x849AF8]: 0 the pause screen,
// 1 Options (the two options' Yes / No, layers 6 and 2 of the pause UI at [0x85E554]), 2 the quit confirmation, which
// goes back to the page in [0x849AFC]. Y (0x800 in the pausing player's buttons) turns between 0 and 1. The turn to
// Options is taken back before a frame draws it, and the settings menu opens on its Controls tab instead. While it
// runs the cursor is free (mouse::InMenu), and a click on a prompt presses its button (menus::Paused).
X86_EXTERN_FUNC(__imp__sub_00203B50);
X86_FUNC(sub_00203B50) {
    constexpr uint32_t kPage = 0x849AF8;
    const uint32_t was = kernel::R32(kPage);
    mouse::InMenu();
    menus::Paused();
    __imp__sub_00203B50(ctx, base);
    if (was == 0 && kernel::R32(kPage) == 1) {
        kernel::W32(kPage, 0);
        ui::Show(ui::Tab::Controls);
    }
}

// The frontend (level 29, the pub) shows the picture in focus, byte [0x77625E]: 0 Xbox Live, 1 Options (to Live's
// right), 2 Chapters, 3 to 5 the three games (0x205361 shows its UI page, the frontend's own at [0x85E550]). Options
// is a page of Yes / No settings, so A on its picture opens the settings menu instead and the game never sees it.
// Start there opens the settings menu instead and Back asks to quit (as Esc does) instead of the game's Xbox Live
// prompt, and Start with Back opens the settings menu anywhere (ui.cpp watches the pads for it), so the game never sees
// the two together.
void gamepad::Pad(int port, input::Pad& pad) {
    constexpr int kFrontend = 29;
    constexpr uint32_t kPicture = 0x77625E, kFrontendUi = 0x85E550;
    constexpr int kOptions = 1;
    constexpr uint16_t kStart = 1 << 4, kBack = 1 << 5;
    static bool was_down[input::kPorts] = {}, start_was[input::kPorts] = {}, back_was[input::kPorts] = {};
    if (port < 0 || port >= input::kPorts) return;
    if ((pad.buttons & (kStart | kBack)) == (kStart | kBack)) pad.buttons &= uint16_t(~(kStart | kBack));
    const bool frontend = warp::CurrentLevel() == kFrontend && kernel::R32(kFrontendUi);
    const bool start = pad.buttons & kStart;
    const bool start_pressed = start && !start_was[port];
    start_was[port] = start;
    // the launcher build has no settings menu: a pad's Start is the game's there, as on the console, and Esc (the
    // keyboard's Start) asks to quit, as Back does
    const bool esc = ui::kLauncherSettings && ui::EscStart();
    if (frontend && start && (!ui::kLauncherSettings || esc)) {
        pad.buttons &= uint16_t(~kStart);
        if (start_pressed) {
            if (ui::kLauncherSettings) ui::ShowQuit();
            else ui::Show(ui::Tab::Last, port);
        }
    }
    const bool back = pad.buttons & kBack;
    const bool back_pressed = back && !back_was[port];
    back_was[port] = back;
    // Back is the game's way into Xbox Live (sign-in, accounts) in the frontends and in a level alike, and that waits
    // forever without the service: the game never sees it. In the pub's own menu it asks to quit, and everywhere else
    // it does nothing. Each press is logged with where it was, should Back turn out to be wanted somewhere
    if (back) {
        pad.buttons &= uint16_t(~kBack);
        if (back_pressed) {
            LOGR("input: Back in level %d (frontend ui %08X)%s", warp::CurrentLevel(), kernel::R32(kFrontendUi),
                 frontend ? ", the quit prompt" : ", kept from the game (Xbox Live)");
            if (frontend) ui::ShowQuit();
        }
    }
    const bool down = pad.analog[0] > 0x40;  // A, past where the game takes it as pressed
    const bool pressed = down && !was_down[port];
    was_down[port] = down;
    if (!down || !frontend || int8_t(mem::Base()[kPicture]) != kOptions) return;
    pad.analog[0] = 0;
    if (pressed) ui::Show(ui::Tab::Controls, port);
}
