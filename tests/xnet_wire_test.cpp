// The system link header rules (src/runtime/xdk/xnet_wire.h): g++ -std=c++20 -I src/runtime tests/xnet_wire_test.cpp
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>

#include "xdk/xnet_wire.h"

using namespace xnet_wire;
static int failures = 0;
#define CHECK(what, cond) do { std::printf("%-72s %s\n", what, (cond) ? "ok" : "FAILED"); failures += !(cond); } while (0)

int main() {
    uint8_t buf[64];
    Header h;
    const uint32_t room = 0x9ABCD, other = 0x12345;

    // 1. no key: byte for byte the header every build before room keys wrote (magic, then both ports, network order)
    {
        uint8_t old[6];
        const uint16_t magic = htons(0x4B43), from = htons(1000), to = htons(1001);
        std::memcpy(old, &magic, 2), std::memcpy(old + 2, &from, 2), std::memcpy(old + 4, &to, 2);
        const int n = Write(buf, 0, false, 1000, 1001);
        CHECK("no key: the old 6-byte header, byte for byte", n == 6 && std::memcmp(buf, old, 6) == 0);
        CHECK("no key: an old build's datagram from the internet is taken", Read(old, 6, 0, false, &h) == Verdict::Accept && h.from == 1000 && h.to == 1001 && h.size == 6);
    }
    // 2. a room: keyed past the network, plain on it
    CHECK("room: to the internet the header is keyed (12 bytes)", Write(buf, room, false, 1000, 1001) == 12);
    CHECK("room: the same room's datagram is taken, ports intact",
          Read(buf, 20, room, false, &h) == Verdict::Accept && h.from == 1000 && h.to == 1001 && h.size == 12);
    CHECK("room: another room's key (an old code) is refused", Read(buf, 20, other, false, &h) == Verdict::WrongKey);
    CHECK("room: a copy without a key still takes it (LAN host, typed address)", Read(buf, 20, 0, false, &h) == Verdict::Accept);
    CHECK("room: on the local network the header stays plain", Write(buf, room, true, 1000, 1001) == 6);
    // 3. strangers and old builds
    Write(buf, 0, false, 1000, 1001);
    CHECK("room host: a plain datagram from the internet (no code) is refused", Read(buf, 6, room, false, &h) == Verdict::NoKey);
    CHECK("room host: a plain datagram from its own network is taken", Read(buf, 6, room, true, &h) == Verdict::Accept);
    Write(buf, room, false, 1000, 1001);
    buf[3] = 2;  // network version 2: a later build
    CHECK("room: another network version is refused as another build", Read(buf, 20, room, false, &h) == Verdict::OtherBuild);
    // 4. damage
    CHECK("too short for any header", Read(buf, 5, 0, true, &h) == Verdict::NotOurs);
    Write(buf, room, false, 1000, 1001);
    CHECK("keyed magic but cut short is not ours", Read(buf, 11, room, false, &h) == Verdict::NotOurs);
    const uint8_t junk[8] = {0x12, 0x34, 0, 0, 0, 0, 0, 0};
    CHECK("another program's datagram is not ours", Read(junk, 8, 0, true, &h) == Verdict::NotOurs);
    std::printf("%s\n", failures ? "SOME FAILED" : "all passed");
    return failures != 0;
}
