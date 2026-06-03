// Smoke: validates netpacket::unpackRaw on the raw MsgHdr+header+body layout.
// Self-contained: includes only CNetPacket.hpp. Run on the Deck:
//   make netpacket_smoke && ./bin/netpacket_smoke
#include "../../src/sdk/CNetPacket.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main()
{
    // [eMsg=151|proto][headerLength=3]["abc"]["DEF"]
    std::vector<uint8_t> pkt;
    auto pushU32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) pkt.push_back(uint8_t(v >> (8 * i))); };
    pushU32(151u | kMsgHdrProtoFlag);
    pushU32(3);
    pkt.insert(pkt.end(), {'a','b','c'});
    pkt.insert(pkt.end(), {'D','E','F'});

    uint16_t eMsg = 0; const uint8_t *pHdr = nullptr, *pBody = nullptr; uint32_t cbHdr = 0, cbBody = 0;
    bool ok = netpacket::unpackRaw(pkt.data(), uint32_t(pkt.size()), eMsg, pHdr, cbHdr, pBody, cbBody);
    assert(ok && "valid packet must parse");
    assert(eMsg == 151);
    assert(cbHdr == 3 && memcmp(pHdr, "abc", 3) == 0);
    assert(cbBody == 3 && memcmp(pBody, "DEF", 3) == 0);

    // Non-protobuf message (proto flag clear) is rejected.
    std::vector<uint8_t> raw = pkt; raw[3] = 0x00; // clear top byte of eMsg
    assert(!netpacket::unpackRaw(raw.data(), uint32_t(raw.size()), eMsg, pHdr, cbHdr, pBody, cbBody));

    // headerLength larger than the buffer is rejected (no OOB).
    std::vector<uint8_t> trunc = pkt; trunc[4] = 0xFF; // headerLength = 0xFF
    assert(!netpacket::unpackRaw(trunc.data(), uint32_t(trunc.size()), eMsg, pHdr, cbHdr, pBody, cbBody));

    // Buffer smaller than MsgHdr is rejected.
    assert(!netpacket::unpackRaw(pkt.data(), 4, eMsg, pHdr, cbHdr, pBody, cbBody));

    printf("netpacket_smoke OK\n");
    return 0;
}
