// Gameplay options (common/gameplay.h): 10x damage for everything Conker hits (the bat, his other melee hits, his
// guns and explosives).
//
// (An earlier Overpowered Frying Pan option also swapped the bat's held object for the pan's, item 0x75's +0x58 from
// object 0x110 to 0x113 as each level started, and guarded 0x2B37D0 against levels without the pan's model. It is
// gone: the bat stays the bat, only its damage changes.)
#include <cstdint>
#include <cstdlib>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include "common/gameplay.h"
#include "common/warp.h"
#include "core/log.h"

bool gameplay::g_bat_10x_damage = false;

// 0x2EDDD0 is a hit on a character (the victim in eax and esi, the attacker its first stack argument): it works
// out the damage into [esp+0x28] and, at 0x2EE52F, takes it off: 0x2F7DF0(victim, damage, 0, 0) for a hit, or a
// negative one straight off the energy (word [chr+0x3AE], the debug overlay's ob_energy, 0x9DEFE). With the option
// on, every hit of Conker's there does 10x (his bat, his other melee hits, his guns and explosives), but not one on
// himself (his own explosion). (An earlier hook on 0xCA880 was wrong: that steps particles.)
void ConkerHitDamage(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!gameplay::g_bat_10x_damage || warp::Multiplayer()) return;
    const uint32_t conker = kernel::R32(0x84A5F8);
    // the frame is 0x48 deep there (sub esp 0x38, four pushes): the first argument is at +0x4C
    if (!conker || kernel::R32(ctx.esp + 0x4C) != conker || ctx.esi == conker) return;
    const uint32_t slot = ctx.esp + 0x28;
    const int32_t damage = int32_t(kernel::R32(slot));
    if (damage <= 0) return;
    const int32_t boosted = damage * 10 > 30000 ? 30000 : damage * 10;
    kernel::W32(slot, uint32_t(boosted));
    static const bool trace = std::getenv("XBE_TRACE_HITS") != nullptr;
    if (trace)
        LOGR("gameplay: Conker's hit on %08X (type %X, energy %d): %d damage, 10x %d", ctx.esi,
         mem::IsCommitted(ctx.esi + 0x6C) ? kernel::R32(ctx.esi + 0x68) : 0u,
         mem::IsCommitted(ctx.esi + 0x3B0) ? int(int16_t(kernel::R16(ctx.esi + 0x3AE))) : -1, damage, boosted);
}

// XBE_TRACE_HITS=1: every hit on a character (0x2EDDD0): its attacker, victim, and the energy before and after
X86_EXTERN_FUNC(__imp__sub_002EDDD0);
X86_FUNC(sub_002EDDD0) {
    static const bool trace = std::getenv("XBE_TRACE_HITS") != nullptr;
    if (!trace) {
        __imp__sub_002EDDD0(ctx, base);
        return;
    }
    const uint32_t victim = ctx.eax, attacker = kernel::R32(ctx.esp + 4), from = kernel::R32(ctx.esp);
    const bool ok = victim && mem::IsCommitted(victim + 0x3B0);
    const int before = ok ? int16_t(kernel::R16(victim + 0x3AE)) : -999;
    __imp__sub_002EDDD0(ctx, base);
    const int after = ok ? int16_t(kernel::R16(victim + 0x3AE)) : -999;
    LOGR("hit: from %08X attacker %08X (Conker %08X) victim %08X type %X: energy %d -> %d", from, attacker,
         kernel::R32(0x84A5F8), victim, ok ? kernel::R32(victim + 0x68) : 0u, before, after);
}
