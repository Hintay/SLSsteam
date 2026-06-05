// Smoke: validates netpacket raw helpers on the MsgHdr+header+body layout.
// Self-contained: includes only RawNetPacket.hpp/.cpp. Run on the Deck:
//   make netpacket_smoke && ./bin/netpacket_smoke
#include "../../src/sdk/RawNetPacket.hpp"
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
    bool ok = netpacket::UnpackRaw(pkt.data(), uint32_t(pkt.size()), eMsg, pHdr, cbHdr, pBody, cbBody);
    assert(ok && "valid packet must parse");
    assert(eMsg == 151);
    assert(cbHdr == 3 && memcmp(pHdr, "abc", 3) == 0);
    assert(cbBody == 3 && memcmp(pBody, "DEF", 3) == 0);

    uint32_t wideEMsg = 0;
    assert(netpacket::UnpackRaw(pkt.data(), uint32_t(pkt.size()), wideEMsg, pHdr, cbHdr, pBody, cbBody));
    assert(wideEMsg == 151);

    netpacket::RawPacketView view;
    assert(netpacket::UnpackRaw(pkt.data(), uint32_t(pkt.size()), view));
    assert(view.eMsg == 151);
    assert(view.msgHdr.eMsg == (151u | kMsgHdrProtoFlag));
    assert(view.msgHdr.headerLength == 3);
    assert(view.headerSize == 3 && memcmp(view.header, "abc", 3) == 0);
    assert(view.bodySize == 3 && memcmp(view.body, "DEF", 3) == 0);

    const uint8_t* sendData = nullptr;
    uint32_t sendSize = 0;
    assert(netpacket::ReplaceSendPacket(pkt.data(), uint32_t(pkt.size()), "xy", 2, "BODY", 4, sendData, sendSize));
    const uint8_t* firstSendData = sendData;
    assert(sendData && sendSize == sizeof(MsgHdr) + 2 + 4);
    assert(netpacket::UnpackRaw(sendData, sendSize, eMsg, pHdr, cbHdr, pBody, cbBody));
    assert(eMsg == 151);
    assert(cbHdr == 2 && memcmp(pHdr, "xy", 2) == 0);
    assert(cbBody == 4 && memcmp(pBody, "BODY", 4) == 0);

    const uint8_t* secondSendData = nullptr;
    uint32_t secondSendSize = 0;
    assert(netpacket::ReplaceSendPacket(pkt.data(), uint32_t(pkt.size()), "zz", 2, "NEXT", 4, secondSendData, secondSendSize));
    assert(firstSendData && secondSendData && firstSendData != secondSendData);
    assert(sendSize == sizeof(MsgHdr) + 2 + 4 && memcmp(firstSendData + sizeof(MsgHdr), "xy", 2) == 0);

    CNetPacket carrier{0, pkt.data(), uint32_t(pkt.size())};
    assert(netpacket::ReplaceRecvPacket(&carrier, "h", 1, "recv", 4));
    assert(carrier.m_pubData && carrier.m_cubData == sizeof(MsgHdr) + 1 + 4);
    assert(netpacket::UnpackRaw(carrier.m_pubData, carrier.m_cubData, eMsg, pHdr, cbHdr, pBody, cbBody));
    assert(eMsg == 151);
    assert(cbHdr == 1 && memcmp(pHdr, "h", 1) == 0);
    assert(cbBody == 4 && memcmp(pBody, "recv", 4) == 0);

    // Non-protobuf message (proto flag clear) is rejected.
    std::vector<uint8_t> raw = pkt; raw[3] = 0x00; // clear top byte of eMsg
    assert(!netpacket::UnpackRaw(raw.data(), uint32_t(raw.size()), eMsg, pHdr, cbHdr, pBody, cbBody));

    // headerLength larger than the buffer is rejected (no OOB).
    std::vector<uint8_t> trunc = pkt; trunc[4] = 0xFF; // headerLength = 0xFF
    assert(!netpacket::UnpackRaw(trunc.data(), uint32_t(trunc.size()), eMsg, pHdr, cbHdr, pBody, cbBody));

    // Buffer smaller than MsgHdr is rejected.
    assert(!netpacket::UnpackRaw(pkt.data(), 4, eMsg, pHdr, cbHdr, pBody, cbBody));
    assert(!netpacket::ReplaceSendPacket(pkt.data(), 4, "xy", 2, "BODY", 4, sendData, sendSize));
    assert(sendData == nullptr && sendSize == 0);

    std::vector<uint8_t> widePkt;
    auto pushWideU32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) widePkt.push_back(uint8_t(v >> (8 * i))); };
    pushWideU32(0x12345u | kMsgHdrProtoFlag);
    pushWideU32(0);
    assert(netpacket::UnpackRaw(widePkt.data(), uint32_t(widePkt.size()), wideEMsg, pHdr, cbHdr, pBody, cbBody));
    assert(wideEMsg == 0x12345u);

    printf("netpacket_smoke OK\n");
    return 0;
}
