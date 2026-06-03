#pragma once

#include <cstdint>

struct CNetPacket;

namespace Achievements
{
	// Raw outgoing hook (from hkCWebSocketConnection_BBuildAndAsyncSendFrame). For a
	// Player.GetUserStats#1 ServiceMethod call (EMsg 151) or a legacy
	// CMsgClientGetUserStats (EMsg 818) of a redirected app, rewrite the donor
	// steamid into the request and (151) track jobid_source -> appId. If the frame
	// must be re-sent modified, returns true and points outData/outSize at a pooled
	// replacement packet (valid until the next call); the caller sends that instead.
	// Returns false to send the original frame unchanged.
	//
	// The ServiceMethod (151) path provably does NOT traverse CProtoBufMsgBase::Send
	// on modern Steam, so it MUST be intercepted here at the raw packet layer (same
	// reason requestcode moved here). The stats CAPIJob fallback stays a no-op.
	bool onSendFrame(const uint8_t* pubData, uint32_t cubData,
	                 const uint8_t*& outData, uint32_t& outSize);

	// Raw incoming hook (from hkCCMConnection_RecvPkt). For a matching
	// Player.GetUserStats#1 response (EMsg 147, jobid_target pending) or a legacy
	// CMsgClientGetUserStatsResponse (EMsg 819) of a redirected app, clear the stat
	// values and force eresult = OK so Steam keeps the schema but falls back to its
	// local achievement cache. Rewrites pkt->m_pubData / m_cubData in place.
	void onRecvPacket(CNetPacket* pkt);

	// Called from hkCAPIJob_GetPlayerStats. No-op: the network-layer redirect above
	// handles stats; the detour is kept only for hook-wiring compatibility.
	void getPlayerStats(uint32_t& eresult);
}
