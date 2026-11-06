// First-person POV: LT (Q on the keyboard) toggles a first-person view that keeps all of Conker's moves.
//
// What the game does (verified with a headless trace, XBE_TRACE_FPV=1):
//   0x46490 (cdecl, arg0 = the player's camera manager pm) builds the player's view matrix at
//   [0x762E44 + [0x7FA230]*4] + [pm+0x2A8]*64. Its look-at 0x29BCA0 takes the EYE at pm+0x330 and the TARGET
//   at pm+0x2F4 and makes a RIGHT-handed D3D-layout matrix (row vectors, translation in row 3): column 0 = right,
//   column 1 = up, column 2 = eye - target normalised (the camera looks down -Z). The follow path then multiplies
//   in shake / roll. 0x418F0 runs it, then inverts the result for the frustum and the per-viewport slots, so a
//   matrix written here, after the original, is what everything downstream sees.
//   [pm+0x3E0] is the player's actor (the same as [0x84A5F8]); actor+0x78 is the position at the feet,
//   actor+0x114 the facing yaw in degrees (camera yaw +180), actor+0x110 the forward speed.
//   The game's own LT look mode (pm+0xC0 = 0x100) settles its eye on the actor's x/z, 78.5 above the feet.
//   The follow camera's target is 100 above the feet and it looks down at it by ~20 degrees.
//
// What FPV does: the game's camera runs untouched (collision, smoothing, right-stick orbit, auto-follow), then
// its view matrix is replaced by one at Conker's eyes looking along the camera's yaw, the follow camera's
// downward tilt taken out. While Conker stands still his facing turns to the view, so an attack goes where you
// look; while he moves the game steers him and the follow camera swings the view behind him.
#include "common/fpv.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "common/cutscenes.h"
#include "common/mouse.h"
#include "common/warp.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"

namespace fpv {

bool g_allow_toggle = true;
bool g_active = false;
float g_eye_height = 78.5f;     // the game's own first-person eye (LT look mode)
float g_forward_offset = 14.0f; // past the face along the view, so the head is behind the near plane
uint32_t g_look_speed = 120;   // percent: the right stick's turn in first person
float g_item_forward = 22.0f;   // what he holds, moved forward along the view (out of the lens)
float g_item_down = 12.0f;      // and down
float g_pitch_limit = 80.0f;    // degrees up and down the first-person view can look

namespace {

constexpr uint32_t kViewTables = 0x762E44;  // [kViewTables + buffer*4] + player*64 = the player's view matrix
constexpr uint32_t kViewBuffer = 0x7FA230;  // int8 buffer index
constexpr float kDeg = 3.14159265f / 180.0f;

float s_pitch = 0.0f;       // the first-person view's own pitch, degrees, up positive
bool s_drawn = false;       // this frame's view is FPV's (0x46490 wrote it); Conker hidden only then
float s_pitch_frame = 0.0f; // this frame's pitch turn (NotePitch), taken by the next view (0x46490)

bool Mapped(uint32_t a, uint32_t size) { return mem::IsRangeCommitted(a, size); }

uint32_t ViewMatrix(uint32_t pm) {
    int8_t buffer = kernel::G<int8_t>(kViewBuffer);
    uint32_t table = kernel::R32(kViewTables + uint32_t(int32_t(buffer)) * 4);
    uint32_t m = table + uint32_t(kernel::G<uint8_t>(pm + 0x2A8)) * 64;
    return table && Mapped(m, 64) ? m : 0;
}

// the game's look-at layout (0x29BCA0): right-handed, column 2 = -forward, translation in row 3
void LookRH(float m[16], const float eye[3], const float fwd[3]) {
    float z[3] = {-fwd[0], -fwd[1], -fwd[2]};
    // right = up x z, up = (0,1,0)
    float x[3] = {z[2], 0.0f, -z[0]};
    float xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (xl < 1e-5f) return;  // straight up or down: the caller clamps the pitch, so never
    x[0] /= xl, x[2] /= xl;
    float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    for (int i = 0; i < 3; i++) {
        m[i * 4 + 0] = x[i];
        m[i * 4 + 1] = y[i];
        m[i * 4 + 2] = z[i];
        m[i * 4 + 3] = 0.0f;
    }
    m[12] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
    m[13] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
    m[14] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
    m[15] = 1.0f;
}

}  // namespace

// a cutscene or a scripted moment: the game's camera, FPV kept for after it
bool Paused() {
    if (cutscenes::Playing()) return true;
    if (mem::IsCommitted(0x76EC82) && kernel::G<uint8_t>(0x76EC82) != 0) return true;
    if (mem::IsCommitted(0x771130) && kernel::G<uint8_t>(0x771130) != 0) return true;
    return false;
}

bool Viewing() { return g_active && !Paused(); }

bool IsInGameplay() {
    int lvl = warp::CurrentLevel();
    if (lvl < 0 || warp::Multiplayer()) return false;
    for (int fe : {29, 33, 34, 70, 71})
        if (lvl == fe) return false;
    if (mem::IsCommitted(0x7819D6) && kernel::G<uint8_t>(0x7819D6) != 0) return false;  // loading
    return true;
}

// Conker's facing to the view's yaw (degrees, the game's convention: direction (sin, cos) on x/z), only while he
// stands: moving, the game turns him toward the stick and forcing it would make every direction run forward.
void UpdateFacing(uint32_t actor, float view_yaw) {
    if (!Mapped(actor, 0x118)) return;
    if (std::fabs(kernel::G<float>(actor + 0x110)) > 0.01f) return;
    float yaw = std::fmod(view_yaw, 360.0f);
    if (yaw < 0.0f) yaw += 360.0f;
    kernel::G<float>(actor + 0x114) = yaw;
}

}  // namespace fpv

namespace fpv {
void NoteView(const float eye[3], const float fwd[3], const float m[16]);
}

// The toggle is read off the pad itself, as the game polls it (keyboard.cpp's pad chain, Q already LT): the camera
// update that reads LT (0x3CE30, from 0x3CA94) is skipped on several of its paths (0x41AE0, 0x41BB0, [0x8472A0],
// [[pm+4]+0x700]), and with something in Conker's hands it is, so a toggle there never came while he held a weapon.
// A press (rising past kPress, let go below kRelease) on any port toggles FPV, and while FPV may toggle LT is kept
// from the game, so its look mode (which stops Conker) never starts. In a cutscene LT reaches the game (it skips).
void fpv::Pad(int port, input::Pad& pad) {
    constexpr uint8_t kPress = 96, kRelease = 40;
    static bool held[input::kPorts] = {};
    if (port < 0 || port >= input::kPorts || input::t_presence) return;
    const uint8_t lt = pad.analog[6];
    const bool gameplay = g_allow_toggle && IsInGameplay();
    if (!gameplay) g_active = false;  // the pub's menus, a load, multiplayer: the game's camera, and FPV off after
    // a cutscene (LT skips it) or one of the game's menus (the pause screen): LT is the game's, no toggle
    static const bool trace = std::getenv("XBE_TRACE_FPV") != nullptr;
    if (trace && lt >= kPress && !held[port])
        LOGR("fpv: LT on port %d: gameplay %d (level %d, load %d), cutscene %d, busy %d %d, menu %d", port,
             int(gameplay), warp::CurrentLevel(), int(kernel::G<uint8_t>(0x7819D6)), int(cutscenes::Playing()),
             int(kernel::G<uint8_t>(0x76EC82)), int(kernel::G<uint8_t>(0x771130)), int(mouse::MenuShown()));
    if (!gameplay || Paused() || mouse::MenuShown()) {
        held[port] = lt >= kPress;  // a press made in a cutscene, a menu or a load is not a toggle after it
        return;
    }
    if (!held[port] && lt >= kPress) {
        held[port] = true;
        g_active = !g_active;
        s_pitch = 0.0f;  // on level
        s_pitch_frame = 0.0f;
        LOGR("fpv: first-person view %s", g_active ? "on" : "off");
    } else if (held[port] && lt < kRelease) {
        held[port] = false;
    }
    pad.analog[6] = 0;
}

// 0x3CE30 reads the player's buttons on the camera manager (esi): [esi+0x38C] held, [esi+0x390] pressed this
// frame, bit 0 LT / Q, which in the game holds the stationary look mode 0x100. fpv::Pad keeps LT from the game; the
// bit is cleared here as well, so nothing else that sets it starts the look mode.
X86_EXTERN_FUNC(__imp__sub_0003CE30);
X86_FUNC(sub_0003CE30) {
    if (!fpv::g_allow_toggle || !fpv::IsInGameplay()) {
        fpv::g_active = false;
        __imp__sub_0003CE30(ctx, base);
        return;
    }
    static const bool force = std::getenv("XBE_FPV_FORCE") != nullptr;  // testing: on without a press
    if (force) fpv::g_active = true;
    const uint32_t pm = ctx.esi;
    if (!fpv::Paused() && fpv::Mapped(pm, 0x3E8)) {
        kernel::W32(pm + 0x390, kernel::R32(pm + 0x390) & ~1u);
        kernel::W32(pm + 0x38C, kernel::R32(pm + 0x38C) & ~1u);
    }
    __imp__sub_0003CE30(ctx, base);
}

// 0x46490: the player's camera and view matrix (see the top). The original runs whole; then, in FPV, player 0's
// view matrix is replaced.
X86_EXTERN_FUNC(__imp__sub_00046490);
X86_FUNC(sub_00046490) {
    const uint32_t pm = kernel::R32(ctx.esp + 4);
    __imp__sub_00046490(ctx, base);
    // player 0's view: FPV's only once it is written below (an early return leaves the game's, Conker shown)
    if (!fpv::Mapped(pm, 0x3E8) || kernel::G<uint8_t>(pm + 0x2A8) == 0) fpv::s_drawn = false;

    if (!fpv::g_active) return;
    if (!fpv::g_allow_toggle || !fpv::IsInGameplay()) {
        fpv::g_active = false;
        return;
    }
    if (!fpv::Viewing()) return;  // a cutscene: its own camera, FPV back after it
    if (!fpv::Mapped(pm, 0x3E8) || kernel::G<uint8_t>(pm + 0x2A8) != 0) return;
    const uint32_t actor = kernel::R32(pm + 0x3E0);
    const uint32_t m = fpv::ViewMatrix(pm);
    if (!fpv::Mapped(actor, 0x118) || !m) return;

    // the game camera's view direction from its final matrix (column 2 is eye - target)
    float g[16];
    std::memcpy(g, kernel::HostPtr(m), sizeof g);
    float bx = g[2], by = g[6], bz = g[10];
    float flat = std::sqrt(bx * bx + bz * bz);
    if (flat < 1e-4f) return;
    float yaw = std::atan2(-bx, -bz);  // forward = -column 2
    // the pitch is first person's own: the follow camera's tilt only goes a few degrees, as it orbits Conker
    fpv::s_pitch = std::fmax(-fpv::g_pitch_limit, std::fmin(fpv::g_pitch_limit, fpv::s_pitch + fpv::s_pitch_frame));
    fpv::s_pitch_frame = 0.0f;
    const float pitch = fpv::s_pitch * fpv::kDeg;
    float fwd[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};

    // the eye: the actor's position, the game's first-person eye height, a little forward on the level
    float eye[3] = {kernel::G<float>(actor + 0x78) + std::sin(yaw) * fpv::g_forward_offset,
                    kernel::G<float>(actor + 0x7C) + fpv::g_eye_height,
                    kernel::G<float>(actor + 0x80) + std::cos(yaw) * fpv::g_forward_offset};
    if (!std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2])) return;

    float v[16];
    std::memcpy(v, g, sizeof v);
    fpv::LookRH(v, eye, fwd);
    fpv::NoteView(eye, fwd, v);
    std::memcpy(kernel::HostPtr(m), v, sizeof v);
    fpv::s_drawn = true;

    fpv::UpdateFacing(actor, yaw / fpv::kDeg);

    static const bool trace = std::getenv("XBE_TRACE_FPV") != nullptr;
    static int n = 0;
    if (trace && (++n % 120) == 0)
        LOGR("fpv: eye (%.1f %.1f %.1f) yaw %.1f pitch %.1f (game's %.1f), feet (%.1f %.1f %.1f) facing %.1f speed %.2f", eye[0],
             eye[1], eye[2], yaw / fpv::kDeg, pitch / fpv::kDeg, std::atan2(-by, flat) / fpv::kDeg, kernel::G<float>(actor + 0x78),
             kernel::G<float>(actor + 0x7C), kernel::G<float>(actor + 0x80), kernel::G<float>(actor + 0x114),
             kernel::G<float>(actor + 0x110));
}

// Conker's own model in first-person view: his draw runs whole, so his skeleton (which what he holds is placed on)
// and the fur's bookkeeping go on, but its geometry is left out of the picture (d3d::SetDrawHidden).
//   0x7F758 draws a character in one viewport (edi the actor, esi the viewport) through 0x7FAB0 (esi the actor,
//   viewport and alpha on the stack) -> 0x7FB00 (model [actor+0x60], viewport, matrix, alpha, ...), which draws the
//   body as its first DrawModel (0x3F0270, through [0x81BD70]: from 0x80053, or the wrapper 0x3ED380), and the
//   fur and the rest itself.
//   The water reflection calls 0x7FAB0 from 0x8E87C and keeps him. Any other model drawn within his draw is shown.
namespace {
int s_own_draw = 0;     // inside Conker's own draw in player 0's view, in FPV
uint32_t s_body_sg = 0;  // his body's scenegraph, the first model drawn in it
constexpr uint32_t kHeldReturn = 0x77F60;  // 0x780E0 called by 0x77F10, his held item (below)
uint32_t s_attach_from = 0;  // within 0x780E0 in Conker's own draw: where it was called from
int s_attach_outside = 0;    // within 0x780E0 for Conker in viewport 0 but outside his own draw (0x7FAB0)
uint16_t s_attach_model = 0; // and the attached object's model id (its record's +2 word; names at [0x6A6108 + id*4])

// models Conker wears on his head or body, hidden in first person (ids from the name table at 0x6A6108): the gas
// mask he wears (0x42 gasmask, not 0x43 gasmaskinhand), his tail (0x259 ConkerTail), hats and helmets, and the
// cigar in his mouth (0x1D; 0x1E is the one in his hand)
bool WornModel(uint16_t id) {
    switch (id) {
        case 0x42:   // gasmask
        case 0x259:  // ConkerTail
        case 0x26E:  // conkerhat
        case 0x5E:   // effects_models_conker_ugga_hat
        case 0x28:   // effects_models_mine_helmet
        case 0x2B:   // effects_models_conker_crown
        case 0x85:   // effects_models_army_hat
        case 0x1D:   // effects_models_conker_cigar
            return true;
        default:
            return false;
    }
}

const char* ModelName(uint16_t id) {
    const uint32_t p = mem::IsCommitted(0x6A6108u + id * 4u) ? kernel::R32(0x6A6108u + id * 4u) : 0;
    return p && mem::IsRangeCommitted(p, 1) ? reinterpret_cast<const char*>(kernel::HostPtr(p)) : "?";
}
}

X86_EXTERN_FUNC(__imp__sub_0007FAB0);
X86_FUNC(sub_0007FAB0) {
    const bool own = fpv::Viewing() && fpv::s_drawn && kernel::R32(ctx.esp) == 0x7F797u && kernel::R32(ctx.esp + 4) == 0 && ctx.esi &&
                     ctx.esi == kernel::R32(0x84A5F8);
    if (!own) {
        __imp__sub_0007FAB0(ctx, base);
        return;
    }
    s_own_draw++;
    s_body_sg = 0;
    const bool before = d3d::SetDrawHidden(true);
    __imp__sub_0007FAB0(ctx, base);
    d3d::SetDrawHidden(before);
    s_own_draw--;
}

// What he holds, moved out of the lens: a model drawn within his draw places its bones from the 64-byte world
// matrices at [data+4] (translation at +0x30, the last column 0 0 0 1). Its bones near the eye (within kNear) are
// shifted in the view's frame, forward and down (g_item_forward, g_item_down), for the draw only, then put back, so
// the game's hits and his hand keep the real places.
namespace {
constexpr int kMaxBones = 64;
constexpr float kNear = 45.0f;
float s_eye[3], s_fwd[3], s_down[3];  // this frame's FPV eye, forward and down (sub_00046490)
float s_head[3];                      // his head's centre: the eye less the forward offset, on the level
bool s_have_view = false;

// the model's bones ([data+4]): how many, the nearest one's distance to the eye and to his head's centre
int Bones(uint32_t data, uint32_t& bones, float& nearest, float& head) {
    head = 1e9f;
    bones = data ? kernel::R32(data + 4) : 0;
    nearest = 1e9f;
    if (!s_have_view || !fpv::Mapped(bones, 64)) return 0;
    int n = 0;
    for (; n < kMaxBones && fpv::Mapped(bones + uint32_t(n) * 64, 64); n++) {
        const uint32_t m = bones + uint32_t(n) * 64;
        if (kernel::G<float>(m + 0x0C) != 0.0f || kernel::G<float>(m + 0x1C) != 0.0f ||
            kernel::G<float>(m + 0x2C) != 0.0f || kernel::G<float>(m + 0x3C) != 1.0f)
            break;
        float d[3];
        for (int i = 0; i < 3; i++) d[i] = kernel::G<float>(m + 0x30 + uint32_t(i) * 4) - s_eye[i];
        const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist > 400.0f) break;  // not a bone of something by him
        nearest = std::fmin(nearest, dist);
        float h = 0.0f;
        for (int i = 0; i < 3; i++) {
            const float e = kernel::G<float>(m + 0x30 + uint32_t(i) * 4) - s_head[i];
            h += e * e;
        }
        head = std::fmin(head, std::sqrt(h));
    }
    return n;
}

int ShiftHeld(uint32_t data, float saved[kMaxBones][3], uint32_t& bones) {
    float nearest, head;
    const int n = Bones(data, bones, nearest, head);
    if (!n || nearest > kNear) return 0;
    float shift[3];
    for (int i = 0; i < 3; i++) shift[i] = s_fwd[i] * fpv::g_item_forward + s_down[i] * fpv::g_item_down;
    for (int b = 0; b < n; b++)
        for (int i = 0; i < 3; i++) {
            float& t = kernel::G<float>(bones + uint32_t(b) * 64 + 0x30 + uint32_t(i) * 4);
            saved[b][i] = t;
            t += shift[i];
        }
    return n;
}
}  // namespace

// DrawModel(scenegraph, root, flags, data) has two bodies the game switches [0x81BD70] between: 0x3F0270, and
// 0x3EE100 (the attached-object code's second path, 0x7866A, and the character draw's, 0x7F5DE: the gas mask
// went through it, missed while only 0x3F0270 was watched). Both get the same judgement.
using DrawFn = void (*)(X86Context& __restrict, uint8_t*);
X86_EXTERN_FUNC(__imp__sub_003F0270);
X86_EXTERN_FUNC(__imp__sub_003EE100);
namespace {
void DrawWithin(X86Context& __restrict ctx, uint8_t* base, DrawFn draw) {
    if (!s_own_draw && !s_attach_outside) {
        draw(ctx, base);
        return;
    }
    // his body is the first model his draw draws (from 0x80053, or through the wrapper 0x3ED380): it and any repeat
    // of it are hidden, anything else (what he holds) is shown
    const uint32_t sg = kernel::R32(ctx.esp + 4), data = kernel::R32(ctx.esp + 16);
    // anything his own draw draws itself, not through the attached-object code (0x780E0), is part of him: the body
    // (from 0x80053, or the wrapper 0x3ED380) and its extra pieces, like the gas mask after he first puts it on
    // (Windmill: model 7FCD73E0 from 0x3ED3D7, seen shown when only the first model drawn was taken as the body)
    if (s_own_draw && !s_body_sg) s_body_sg = sg;
    const bool body = s_own_draw && (sg == s_body_sg || s_attach_from == 0);
    // an attached model he wears (WornModel: the gas mask, his tail, hats) is hidden; anything else attached from
    // elsewhere is hidden where it would sit over the eye; what he holds in his hand is shown, moved out of the lens
    float nearest = 1e9f, head = 1e9f;
    uint32_t bones = 0;
    const int count = body ? 0 : Bones(data, bones, nearest, head);
    const bool worn = WornModel(s_attach_model);
    const bool held = !body && !worn && s_attach_from == kHeldReturn;
    float saved[kMaxBones][3];
    const int shifted = held ? ShiftHeld(data, saved, bones) : 0;
    const bool hide = body || worn || (!held && count && nearest <= kNear);
    static const bool trace = std::getenv("XBE_TRACE_FPV") != nullptr;
    static int n = 0;
    // each model logged when first seen, then every 5 s
    static uint32_t seen_sg[16] = {};
    static int64_t seen_at[16] = {};
    const int64_t now = int64_t(std::clock()) * 1000 / CLOCKS_PER_SEC;
    bool log = false;
    if (trace && !body) {
        int slot = -1;
        for (int i = 0; i < 16 && slot < 0; i++)
            if (seen_sg[i] == sg || !seen_sg[i]) slot = i;
        if (slot < 0) slot = int(++n % 16);
        if (seen_sg[slot] != sg || now - seen_at[slot] > 5000) {
            seen_sg[slot] = sg, seen_at[slot] = now;
            log = true;
        }
    }
    if (log)
        LOGR("fpv: model %08X (%X %s) drawn %s Conker's (from %08X, attached from %08X): %s, %d bones, nearest %.1f from "
             "the eye, %.1f from the head, %d moved out of the lens", sg, s_attach_model,
             s_attach_from ? ModelName(s_attach_model) : "-", s_own_draw ? "within" : "outside",
             kernel::R32(ctx.esp), s_attach_from,
             worn ? "worn, hidden" : held ? "held" : hide ? "hidden" : "shown", count, nearest, head, shifted);
    const bool before = d3d::SetDrawHidden(hide);
    draw(ctx, base);
    d3d::SetDrawHidden(before);
    for (int b = 0; b < shifted; b++)
        for (int i = 0; i < 3; i++) kernel::G<float>(bones + uint32_t(b) * 64 + 0x30 + uint32_t(i) * 4) = saved[b][i];
}
}  // namespace

X86_FUNC(sub_003F0270) { DrawWithin(ctx, base, __imp__sub_003F0270); }
X86_FUNC(sub_003EE100) { DrawWithin(ctx, base, __imp__sub_003EE100); }

// the FPV view of this frame, for ShiftHeld: down is minus the matrix's up (column 1)
void fpv::NoteView(const float eye[3], const float fwd[3], const float m[16]) {
    for (int i = 0; i < 3; i++) {
        s_eye[i] = eye[i];
        s_fwd[i] = fwd[i];
        s_down[i] = -m[i * 4 + 1];
    }
    const float flat = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    s_head[0] = eye[0] - (flat > 1e-4f ? fwd[0] / flat : 0.0f) * fpv::g_forward_offset;
    s_head[1] = eye[1];
    s_head[2] = eye[2] - (flat > 1e-4f ? fwd[2] / flat : 0.0f) * fpv::g_forward_offset;
    s_have_view = true;
}

// 0x2F99A0 (mouse.cpp hooks it) turned a camera by the right stick: the frame's turns at turn+0x08 (yaw, taken off
// +0x04) and +0x44 (pitch, added to +0x40 inside config+0x18..+0x1C), eased at +0x10 / +0x4C. In first person player
// 1's turn is made g_look_speed percent of it, before the mouse's is added.
void fpv::BoostTurn(uint32_t actions, uint32_t turn) {
    if (!Viewing() || actions != 0x763260u || g_look_speed == 100 || !Mapped(turn, 0x50)) return;
    if (!std::isfinite(kernel::G<float>(turn + 0x04)) || !std::isfinite(kernel::G<float>(turn + 0x08)) ||
        !std::isfinite(kernel::G<float>(turn + 0x40)) || !std::isfinite(kernel::G<float>(turn + 0x44)))
        return;  // a camera not set up: nothing to scale (and fmod of inf stays NaN)
    const uint32_t settings = kernel::R32(turn);
    if (!Mapped(settings, 0x20)) return;
    const float k = float(std::min<uint32_t>(g_look_speed, 300)) / 100.0f - 1.0f;
    auto F = [](uint32_t a) -> float& { return kernel::G<float>(a); };
    const float yaw = F(turn + 0x08) * k;
    F(turn + 0x08) += yaw;
    float y = std::fmod(F(turn + 0x04) - yaw, 360.0f);
    if (y < 0.0f) y += 360.0f;
    F(turn + 0x04) = y;
    F(turn + 0x10) -= yaw;
    const float pitch = F(turn + 0x44) * k;
    const float lo = F(settings + 0x18), hi = F(settings + 0x1C), was = F(turn + 0x40);
    float now = was - pitch;
    if (lo <= hi) now = std::fmax(std::fmin(lo, was), std::fmin(now, std::fmax(hi, was)));
    F(turn + 0x44) += pitch;
    F(turn + 0x40) = now;
    F(turn + 0x4C) += now - was;
}

// 0x780E0(record, character, viewport, ...) draws an object attached to a character. What Conker holds (pan, bat, key,
// gun) is drawn from 0x77F10, which draws the records whose id ([rec]) is the character's held item ([chr+0x10B]),
// returning to 0x77F60; anything else on him (the gas mask, hats) comes from the other lists (0x77CF0).
X86_EXTERN_FUNC(__imp__sub_000780E0);
X86_FUNC(sub_000780E0) {
    // an object attached to Conker, for player 0's view, drawn outside his own draw (its args: record, character,
    // viewport): judged as one within it
    const bool outside = !s_own_draw && fpv::Viewing() && fpv::s_drawn && kernel::R32(ctx.esp + 8) &&
                         kernel::R32(ctx.esp + 8) == kernel::R32(0x84A5F8) && kernel::R32(ctx.esp + 12) == 0;
    if (!s_own_draw && !outside) {
        __imp__sub_000780E0(ctx, base);
        return;
    }
    const uint32_t was = s_attach_from;
    const uint16_t was_model = s_attach_model;
    s_attach_from = kernel::R32(ctx.esp);
    const uint32_t rec = kernel::R32(ctx.esp + 4);
    s_attach_model = rec && mem::IsRangeCommitted(rec, 4) ? kernel::R16(rec + 2) : 0;
    if (outside) s_attach_outside++;
    __imp__sub_000780E0(ctx, base);
    if (outside) s_attach_outside--;
    s_attach_from = was;
    s_attach_model = was_model;
}

// after 0x2F99A0 and the mouse (mouse.cpp): player 1's frame pitch turn +0x44, for first person's own pitch, with
// the stick's share (+0x44 before the mouse was added) told apart. The angle +0x40 comes off +0x44 and a higher angle
// tilts the game's view up, so in third person the stick is the orbit's way round (up tilts the view down, measured:
// stick down took it from -14 to +2 degrees) and the mouse a shooter's (up looks up). First person takes both the
// shooter's way: the stick's +0x44 is up, the mouse's minus it. The game's Invert Aim turns the stick's over.
// The game turns more than one camera some frames: the largest turn is taken.
float fpv::PitchTurn(uint32_t turn) { return Mapped(turn, 0x48) ? kernel::G<float>(turn + 0x44) : 0.0f; }

void fpv::NotePitch(uint32_t actions, uint32_t turn, float stick) {
    if (!Viewing() || actions != 0x763260u || !Mapped(turn, 0x50)) return;
    const float up = stick - (kernel::G<float>(turn + 0x44) - stick);
    if (std::isfinite(up) && std::fabs(up) > std::fabs(s_pitch_frame)) s_pitch_frame = up;
}

// 0x2F99A0 holds the pitch, and the frame's pitch turn +0x44 with it, inside the camera's limits ([turn]+0x18 ..
// +0x1C, about -35..35 on foot), so at a limit the stick asked for nothing more. In first person player 1's limits
// are opened for the call (BeginTurn) and put back after it (EndTurn), the camera's own pitch held inside them again,
// so the turn the stick asked for is whole and the follow camera stays as it was.
bool fpv::BeginTurn(uint32_t actions, uint32_t turn, float saved[2]) {
    if (!Viewing() || actions != 0x763260u || !Mapped(turn, 0x50)) return false;
    const uint32_t settings = kernel::R32(turn);
    if (!Mapped(settings, 0x20)) return false;
    saved[0] = kernel::G<float>(settings + 0x18);
    saved[1] = kernel::G<float>(settings + 0x1C);
    if (!(saved[0] <= saved[1])) return false;
    kernel::G<float>(settings + 0x18) = -1000.0f;
    kernel::G<float>(settings + 0x1C) = 1000.0f;
    return true;
}

void fpv::EndTurn(uint32_t turn, const float saved[2]) {
    const uint32_t settings = kernel::R32(turn);
    kernel::G<float>(settings + 0x18) = saved[0];
    kernel::G<float>(settings + 0x1C) = saved[1];
    float& pitch = kernel::G<float>(turn + 0x40);
    const float held = std::fmax(saved[0], std::fmin(saved[1], pitch));
    kernel::G<float>(turn + 0x4C) += held - pitch;
    pitch = held;
}
