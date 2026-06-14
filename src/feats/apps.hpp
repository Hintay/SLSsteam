#pragma once

#include <cstddef>
#include <cstdint>
#include <map>

class CAppOwnershipInfo;
class CProtoBufMsgBase;
class CMsgClientGamesPlayed;
class CMsgClientPICSProductInfoRequest;

namespace Apps
{
	extern bool applistRequested;
	extern std::map<uint32_t, int> appIdOwnerOverride;

	bool unlockApp(uint32_t appId, CAppOwnershipInfo* info, uint32_t ownerId);
	bool unlockApp(uint32_t appId, CAppOwnershipInfo* info);

	// Cache of configured apps that Steam's original ownership path has confirmed
	// are genuinely owned. This avoids treating package-injected fake ownership
	// as real ownership.
	bool isGenuinelyOwned(uint32_t appId);
	void setGenuinelyOwned(uint32_t appId, bool owned);
	void markGenuinelyOwned(uint32_t appId);
	void unmarkGenuinelyOwned(uint32_t appId);
	bool shouldTreatAsFakeOwned(uint32_t appId);
	bool isGenuinelySubscribed(uint32_t appId);

	bool checkAppOwnership(uint32_t appId, CAppOwnershipInfo* info);
	void getSubscribedApps(uint32_t* appList, uint32_t size, uint32_t& count);

	bool shouldDisableCloud(uint32_t appId);
	bool shouldDisableCDKey(uint32_t appId);
	bool shouldDisableUpdates(uint32_t appId);

	void sendGamesPlayed(CMsgClientGamesPlayed* msg);
	void sendPICSInfoRequest(CMsgClientPICSProductInfoRequest* msg);
	void sendMsg(CProtoBufMsgBase* msg);
};
