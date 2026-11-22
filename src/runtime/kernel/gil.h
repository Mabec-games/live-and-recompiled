// The guest lock (XBE_GIL=1, off by default): guest threads take turns, as on the Xbox's single CPU.
//
// The Xbox has one core, and a title's threads can depend on taking turns without knowing it. On x86 hosts running
// them truly in parallel has held up (x86 keeps stores in order), but AArch64 reorders memory, so on Apple Silicon a
// race the console never had can show up as a rare crash or a hang. With the lock on, a guest thread holds it while it
// runs recompiled code and gives it up in every kernel call and every host wait (frame pacing, audio, the network),
// where Xbox threads switch too; a thread waiting over a millisecond asks the holder to yield at its next function
// entry (X86_LOAD_FLAGS), the pre-emption the Xbox's scheduler would do. Handing over is a ticket queue, so a busy
// thread can't take it straight back. The runtime's own threads (render, audio mixing, timers) never hold it.
// The design is xboxrecomp's guest lock (nfsu2-ios, kernel_bridge.c), whose comment tells the race that called for it.
#pragma once

namespace gil {

bool On();
void Enter();  // nests per thread
void Leave();
int Suspend();  // gives the lock up entirely, returns the nesting to Resume with
void Resume(int depth);
void Abandon();  // the thread is ending without unwinding (pthread_exit): it holds nothing from here on

struct Hold {
    Hold() { Enter(); }
    ~Hold() { Leave(); }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
};
// a host wait inside guest code: other guest threads run meanwhile
struct Unlocked {
    int depth;
    Unlocked() : depth(Suspend()) {}
    ~Unlocked() { Resume(depth); }
    Unlocked(const Unlocked&) = delete;
    Unlocked& operator=(const Unlocked&) = delete;
};

}  // namespace gil
