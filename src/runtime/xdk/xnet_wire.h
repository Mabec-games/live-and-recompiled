#pragma once
// The system link port's datagram header, and which datagrams a copy takes. Kept apart from xnet.cpp so it can be
// tested on its own (tests/xnet_wire_test.cpp).
//
// Plain header (6 bytes), as every build before room keys sent it: kMagic, the sender's link port, the receiver's.
// Keyed header (12 bytes): kMagicKeyed, kWireVersion, the room key, the sender's link port, the receiver's. All big
// endian (network order).
//
// A copy with no room key (0) sends plain datagrams and takes both kinds: exactly the system link that was there
// before. A copy with a room key (an online room, joined or hosted with a code) sends keyed datagrams to addresses
// past its local network, plain ones on it, and takes from past its network only keyed datagrams of its own key
// and network version; on its network it takes both, so a LAN game needs no code.
#include <cstdint>
#include <cstring>

namespace xnet_wire {

constexpr uint16_t kMagic = 0x4B43;       // "CK"
constexpr uint16_t kMagicKeyed = 0x4B4B;  // "KK"
constexpr uint16_t kWireVersion = 1;      // keyed datagrams of another version are another build's: refused
constexpr int kPlainHeader = 6;
constexpr int kKeyedHeader = 12;
constexpr int kMaxHeader = kKeyedHeader;

inline void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
inline void Put32(uint8_t* p, uint32_t v) { Put16(p, uint16_t(v >> 16)), Put16(p + 2, uint16_t(v)); }
inline uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t Get32(const uint8_t* p) { return uint32_t(Get16(p)) << 16 | Get16(p + 2); }

// the header for a datagram from link port `from` to link port `to` (both in host order), keyed when key is set and
// the destination is past this copy's network; its size
inline int Write(uint8_t* buf, uint32_t key, bool to_local_network, uint16_t from, uint16_t to) {
    if (!key || to_local_network) {
        Put16(buf, kMagic);
        Put16(buf + 2, from);
        Put16(buf + 4, to);
        return kPlainHeader;
    }
    Put16(buf, kMagicKeyed);
    Put16(buf + 2, kWireVersion);
    Put32(buf + 4, key);
    Put16(buf + 8, from);
    Put16(buf + 10, to);
    return kKeyedHeader;
}

enum class Verdict {
    Accept,
    NotOurs,     // no header of ours: another program's datagram, or too short
    NoKey,       // a plain datagram from past the network, to a copy in a room
    WrongKey,    // another room's key (an old or another code)
    OtherBuild,  // a keyed datagram of another network version
};

struct Header {
    uint16_t from = 0, to = 0;  // link ports, host order
    int size = 0;
};

inline Verdict Read(const uint8_t* buf, int n, uint32_t key, bool from_local_network, Header* h) {
    if (n < kPlainHeader) return Verdict::NotOurs;
    const uint16_t magic = Get16(buf);
    if (magic == kMagic) {
        if (key && !from_local_network) return Verdict::NoKey;
        *h = {Get16(buf + 2), Get16(buf + 4), kPlainHeader};
        return Verdict::Accept;
    }
    if (magic != kMagicKeyed || n < kKeyedHeader) return Verdict::NotOurs;
    if (Get16(buf + 2) != kWireVersion) return Verdict::OtherBuild;
    // a copy without a key takes keyed datagrams too (a room's host on its LAN, a player who typed the address)
    if (key && Get32(buf + 4) != key) return Verdict::WrongKey;
    *h = {Get16(buf + 8), Get16(buf + 10), kKeyedHeader};
    return Verdict::Accept;
}

}  // namespace xnet_wire
