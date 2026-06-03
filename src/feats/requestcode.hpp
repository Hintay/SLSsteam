#pragma once
#include <cstdint>

struct CNetPacket;

namespace RequestCode
{
	// hkBBuildAndAsyncSendFrame (outgoing WebSocket binary frame). If the frame is
	// the ContentServerDirectory.GetManifestRequestCode#1 ServiceMethod call
	// (EMsg 151), kick off an async fetch of the manifest request code keyed by the
	// header's jobid_source. The outgoing frame is never modified.
	void onSendFrame(const uint8_t* pubData, uint32_t cubData);

	// hkRecvPkt (incoming CNetPacket). If the packet is the matching ServiceMethod
	// response (EMsg 147 whose jobid_target is pending), wait (bounded) for the
	// fetch and splice the code in by rewriting pkt->m_pubData / pkt->m_cubData
	// (ring-buffer pool). Mutates pkt in place; cheap to call on every packet
	// (fast-paths out unless a manifest request-code fetch is outstanding).
	void onRecvPacket(CNetPacket* pkt);
}
