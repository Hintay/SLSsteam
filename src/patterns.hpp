#pragma once

#include "memhlp.hpp"

#include "libmem/libmem.h"

#include <string>
#include <vector>


struct Pattern_t
{
public:
	const std::string name;
	const std::string pattern;
	const MemHlp::SigFollowMode followMode;
	std::vector<uint8_t> prologue;
	const bool optional;

	lm_address_t address;
	lm_module_t* module;

	Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, lm_module_t* module = nullptr, bool optional = false);
	Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, std::vector<uint8_t> prologue, lm_module_t* module = nullptr, bool optional = false);
	//~CPattern();

	bool find();
};

namespace Patterns
{
	extern Pattern_t FamilyGroupRunningApp;
	extern Pattern_t StopPlayingBorrowedApp;

	extern Pattern_t LoadDepotDecryptionKey;
	extern Pattern_t BuildDepotDependency;
	extern Pattern_t BUpdateAppDownloadPlan;

	extern Pattern_t TraceIPC;

	namespace CAPIJob
	{
		extern Pattern_t GetPlayerStats;
	}

	namespace CProtoBufMsgBase
	{
		extern Pattern_t InitFromPacket;
		extern Pattern_t Send;
	};

	namespace CSteamEngine
	{
		extern Pattern_t Init;
		extern Pattern_t SetAppIdForCurrentPipe;

		extern Pattern_t Offset_User;
	}

	namespace CSteamMatchmakingServers
	{
		extern Pattern_t GetServerDetails;
		extern Pattern_t RequestInternetServerList;
	}

	namespace CUser
	{
		//TODO: Order & Convert old patterns
		extern Pattern_t CheckAppOwnership;
		extern Pattern_t GetSubscribedApps;
		extern Pattern_t PostCallback;
		extern Pattern_t UpdateAppOwnershipTicket;
		extern Pattern_t MarkLicenseAsChanged;
		extern Pattern_t ProcessPendingLicenseUpdates;
	}

	namespace IClientAppManager
	{
		extern Pattern_t RunIPCFrame;
		extern Pattern_t BCanRemotePlayTogether;
	}

	namespace IClientApps
	{
		extern Pattern_t RunIPCFrame;
	}

	namespace IClientRemoteStorage
	{
		extern Pattern_t RunIPCFrame;
	}

	namespace CAutoCloudManager
	{
		extern Pattern_t StartSync;
	}

	namespace IClientUser
	{
		extern Pattern_t RunIPCFrame;

		extern Pattern_t BLoggedOn;
		extern Pattern_t BUpdateAppOwnershipTicket;
		extern Pattern_t GetAppOwnershipTicketExtendedData;
		extern Pattern_t GetSteamId;
		extern Pattern_t IsUserSubscribedAppInTicket;
		extern Pattern_t RequiresLegacyCDKey;
	}

	namespace IClientUGC
	{
		extern Pattern_t RunIPCFrame;
	}

	namespace IClientUserStats
	{
		extern Pattern_t RunIPCFrame;
	}

	namespace IClientUtils
	{
		extern Pattern_t RunIPCFrame;
		extern Pattern_t Offset_GetPipeIndex;
	}

	namespace CPackageInfo
	{
		extern Pattern_t GetPackageInfo;
	}

	namespace CAppInfoCache
	{
		extern Pattern_t GetOrAddAppData;   // 3-arg cdecl appinfo lookup/insert
	}

	namespace CUtlMemory
	{
		extern Pattern_t Grow;   // CUtlMemory<uint32>::Grow(mem*, int) via Relative anchor
	}

	namespace CWebSocketConnection
	{
		extern Pattern_t BBuildAndAsyncSendFrame;   // outgoing raw packet (manifest request-code)
	}

	namespace CCMConnection
	{
		extern Pattern_t RecvPkt;                   // incoming raw packet (CNetPacket*)
	}

	//steamui.so
	namespace ISteamMatchmakingPingResponse
	{
		extern Pattern_t ServerResponded;
	}

	namespace CSteamUIAppController
	{
		extern Pattern_t GetAppByID;
		extern Pattern_t FillInAppOverview;
	}
	namespace CUpdateManager
	{
		extern Pattern_t MarkAppChange;
	}

	extern std::vector<Pattern_t*> patterns;
	bool init();
}
