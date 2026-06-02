#pragma once

#include <cstdint>

class CProtoBufMsgBase;

namespace Achievements
{
	// Outgoing hook: intercepts Player.GetUserStats#1 ServiceMethod calls (EMsg 151)
	// and CMsgClientGetUserStats requests (EMsg 818). For controlled apps, redirects
	// the stats query to the configured Steam ID (from LuaLoader::getStatSteamId).
	// Tracks jobid_source -> appId for ServiceMethod path so recvMessage can patch
	// the corresponding response.
	void sendMessage(CProtoBufMsgBase* msg);

	// Incoming hook: patches Player.GetUserStats responses (EMsg 147) and
	// CMsgClientGetUserStatsResponse messages (EMsg 819) for controlled apps,
	// clearing stats/achievements and forcing eresult = OK so Steam reads from
	// its local cache instead of overwriting it with the target account's data.
	void recvMessage(const CProtoBufMsgBase* msg);

	// Called from hkCAPIJob_GetPlayerStats. Previously forced NO_CONNECTION to
	// block live stats; now a no-op since the network hooks handle redirection.
	// Kept for hook-wiring compatibility — removing the detour would require
	// changes in hooks.cpp and patterns that are outside T8 scope.
	void getPlayerStats(uint32_t& eresult);
}
