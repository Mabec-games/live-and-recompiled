// Atomics on guest memory, for locked instructions in recompiled code and the kernel's Interlocked* exports.
// x86 does a lock-prefixed read-modify-write at any address; AArch64's atomics take an alignment fault on one that
// isn't naturally aligned (a SIGBUS that kills the game), and Xbox titles do misaligned interlocked ops now and then.
// On arm64 an aligned address keeps the real atomic; a misaligned one goes through one spinlock, atomic against every
// other misaligned access (the only kind that can reach those bytes short of overlapping aligned ones). The same
// approach as xboxrecomp's RECOMP_ATOMIC_* (nfsu2-ios). On x86 hosts each one is just the builtin.
#pragma once
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
inline volatile int x86_unaligned_atomic_lock = 0;

struct X86UnalignedAtomicGuard {
    X86UnalignedAtomicGuard() {
        while (__atomic_exchange_n(&x86_unaligned_atomic_lock, 1, __ATOMIC_ACQUIRE))
            while (__atomic_load_n(&x86_unaligned_atomic_lock, __ATOMIC_RELAXED)) __asm__ volatile("yield");
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    ~X86UnalignedAtomicGuard() {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        __atomic_store_n(&x86_unaligned_atomic_lock, 0, __ATOMIC_RELEASE);
    }
};

template <class T>
inline bool x86_atomic_aligned(const T* p) {
    return (reinterpret_cast<uintptr_t>(p) & (sizeof(T) - 1)) == 0;
}
template <class T>
inline T x86_ua_get(const T* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
template <class T>
inline void x86_ua_set(T* p, T v) {
    std::memcpy(p, &v, sizeof(T));
}

template <class T>
inline T x86_atomic_load(T* p, int order) {
    if (x86_atomic_aligned(p)) return __atomic_load_n(p, __ATOMIC_SEQ_CST);
    X86UnalignedAtomicGuard g;
    return x86_ua_get(p);
}
template <class T, class V>
inline bool x86_atomic_cas(T* p, T* expected, V desired, bool, int, int) {
    if (x86_atomic_aligned(p))
        return __atomic_compare_exchange_n(p, expected, T(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    X86UnalignedAtomicGuard g;
    T cur = x86_ua_get(p);
    if (cur == *expected) {
        x86_ua_set(p, T(desired));
        return true;
    }
    *expected = cur;
    return false;
}
template <class T, class V>
inline T x86_atomic_xchg(T* p, V v, int) {
    if (x86_atomic_aligned(p)) return __atomic_exchange_n(p, T(v), __ATOMIC_SEQ_CST);
    X86UnalignedAtomicGuard g;
    T old = x86_ua_get(p);
    x86_ua_set(p, T(v));
    return old;
}

#define X86_ATOMIC_RMW(name, builtin, expr, ret)                     \
    template <class T, class V>                                      \
    inline T name(T* p, V v, int) {                                  \
        if (x86_atomic_aligned(p)) return builtin(p, T(v), __ATOMIC_SEQ_CST); \
        X86UnalignedAtomicGuard g;                                   \
        T a = x86_ua_get(p);                                         \
        T r = T(expr);                                               \
        x86_ua_set(p, r);                                            \
        return ret;                                                  \
    }
X86_ATOMIC_RMW(x86_atomic_fetch_add, __atomic_fetch_add, a + T(v), a)
X86_ATOMIC_RMW(x86_atomic_fetch_and, __atomic_fetch_and, a & T(v), a)
X86_ATOMIC_RMW(x86_atomic_fetch_or, __atomic_fetch_or, a | T(v), a)
X86_ATOMIC_RMW(x86_atomic_fetch_xor, __atomic_fetch_xor, a ^ T(v), a)
X86_ATOMIC_RMW(x86_atomic_add_fetch, __atomic_add_fetch, a + T(v), r)
X86_ATOMIC_RMW(x86_atomic_sub_fetch, __atomic_sub_fetch, a - T(v), r)
#undef X86_ATOMIC_RMW

#else  // x86 hosts: misaligned locked ops are fine in hardware

#define x86_atomic_load __atomic_load_n
#define x86_atomic_cas __atomic_compare_exchange_n
#define x86_atomic_xchg __atomic_exchange_n
#define x86_atomic_fetch_add __atomic_fetch_add
#define x86_atomic_fetch_and __atomic_fetch_and
#define x86_atomic_fetch_or __atomic_fetch_or
#define x86_atomic_fetch_xor __atomic_fetch_xor
#define x86_atomic_add_fetch __atomic_add_fetch
#define x86_atomic_sub_fetch __atomic_sub_fetch

#endif
