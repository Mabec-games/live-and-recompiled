// The guest lock, see gil.h
#include "kernel/gil.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "core/log.h"

// read by recompiled code at every function entry (cpu/x86_ops.h)
extern "C" volatile int x86_gil_contended = 0;

namespace gil {

namespace {
std::mutex g_m;
std::condition_variable g_cv;
uint64_t g_next = 0, g_serving = 0;  // the ticket queue, under g_m
thread_local int t_depth = 0;
thread_local bool t_gone = false;

void Lock() {
    std::unique_lock<std::mutex> lock(g_m);
    const uint64_t me = g_next++;
    while (me != g_serving)
        if (g_cv.wait_for(lock, std::chrono::milliseconds(1)) == std::cv_status::timeout)
            x86_gil_contended = 1;  // a millisecond and still waiting: the holder yields at its next function entry
}

void Unlock() {
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_serving++;
        if (g_serving == g_next) x86_gil_contended = 0;  // nobody behind
    }
    g_cv.notify_all();
}
}  // namespace

bool On() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_GIL");
        bool v = e && *e == '1';
        if (v) LOGR("gil: guest threads take turns (XBE_GIL=1)");
        return v;
    }();
    return on;
}

void Enter() {
    if (!On() || t_gone) return;
    if (t_depth++ == 0) Lock();
}

void Leave() {
    if (!On() || t_gone || t_depth == 0) return;
    if (--t_depth == 0) Unlock();
}

int Suspend() {
    if (!On() || t_gone || t_depth == 0) return 0;
    int d = t_depth;
    t_depth = 0;
    Unlock();
    return d;
}

void Resume(int depth) {
    if (!On() || t_gone || depth == 0) return;
    Lock();
    t_depth = depth;
}

void Abandon() {
    if (!On()) return;
    if (t_depth) {
        t_depth = 0;
        Unlock();
    }
    t_gone = true;
}

}  // namespace gil

extern "C" void x86_gil_preempt() {
    int d = gil::Suspend();
    if (!d) return;
    std::this_thread::yield();
    gil::Resume(d);
}
