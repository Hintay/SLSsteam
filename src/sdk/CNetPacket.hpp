#pragma once
#include <cstdint>
#include <cstddef>

// Steam's raw on-wire message prefix: an 8-byte MsgHdr precedes the protobuf
// header and protobuf body inside a network packet payload. A protobuf message
// has kMsgHdrProtoFlag OR'd into eMsg.
struct MsgHdr {
    uint32_t eMsg;          // EMsg, OR'd with kMsgHdrProtoFlag for protobuf messages
    uint32_t headerLength;  // byte length of the protobuf header that follows
};
static constexpr uint32_t kMsgHdrProtoFlag = 0x80000000u;

// CCMConnection's network packet. Field layout (m_hConnection@0, m_pubData@+0x04,
// m_cubData@+0x08) matches the CCMConnection::RecvPkt -> classifier disassembly
// (reads [pkt+4]/[pkt+8]). Only the fields the request-code hook touches are modeled.
struct CNetPacket {
    uint32_t m_hConnection;   // +0x00
    uint8_t* m_pubData;       // +0x04  — MsgHdr + protobuf header + protobuf body
    uint32_t m_cubData;       // +0x08
    // remaining fields (m_cRef, m_pubNetworkBuffer, m_pNext) intentionally omitted
};
