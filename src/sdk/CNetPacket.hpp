#pragma once
#include <cstdint>
#include <cstddef>

// Steam's raw on-wire message prefix: an 8-byte MsgHdr precedes the protobuf
// header and protobuf body inside a network packet payload. A protobuf message
// has kMsgHdrProtoFlag OR'd into eMsg. Mirrors OST's Hooks_NetPacket MsgHdr.
struct MsgHdr {
    uint32_t eMsg;          // EMsg, OR'd with kMsgHdrProtoFlag for protobuf messages
    uint32_t headerLength;  // byte length of the protobuf header that follows
};
static constexpr uint32_t kMsgHdrProtoFlag = 0x80000000u;

// CCMConnection's network packet. Offsets verified on the live build against
// OST's Structs.h CNetPacket (m_hConnection@0, m_pubData@+0x04, m_cubData@+0x08)
// and the CCMConnection::RecvPkt -> classifier disassembly (reads [pkt+4]/[pkt+8]).
// Only the fields the request-code hook touches are modeled.
struct CNetPacket {
    uint32_t m_hConnection;   // +0x00
    uint8_t* m_pubData;       // +0x04  — MsgHdr + protobuf header + protobuf body
    uint32_t m_cubData;       // +0x08
    // remaining fields (m_cRef, m_pubNetworkBuffer, m_pNext) intentionally omitted
};

namespace netpacket {
    // Parse the raw packet layout: [MsgHdr][protobuf header][protobuf body].
    // Returns false unless it is a protobuf message whose header fits within size.
    inline bool unpackRaw(const uint8_t* data, uint32_t size,
                          uint16_t& eMsg, const uint8_t*& pHdr, uint32_t& cbHdr,
                          const uint8_t*& pBody, uint32_t& cbBody)
    {
        if (!data || size < sizeof(MsgHdr)) return false;
        const MsgHdr* hdr = reinterpret_cast<const MsgHdr*>(data);
        if (!(hdr->eMsg & kMsgHdrProtoFlag)) return false;
        eMsg  = static_cast<uint16_t>(hdr->eMsg & ~kMsgHdrProtoFlag);
        cbHdr = hdr->headerLength;
        // size >= sizeof(MsgHdr) is guaranteed above, so (size - sizeof(MsgHdr))
        // can't underflow; written this way to avoid sizeof(MsgHdr)+cbHdr overflowing
        // for a hostile/huge headerLength.
        if (cbHdr > size - sizeof(MsgHdr)) return false;
        const uint32_t off = sizeof(MsgHdr) + cbHdr;
        pHdr   = data + sizeof(MsgHdr);
        pBody  = data + off;
        cbBody = size - off;
        return true;
    }
}
