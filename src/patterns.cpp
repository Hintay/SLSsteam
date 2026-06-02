#include "patterns.hpp"

#include "globals.hpp"
#include "memhlp.hpp"

#include "libmem/libmem.h"

#include <algorithm>
#include <memory>


Pattern_t::Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, lm_module_t* module)
	:
	Pattern_t(name, pattern, followMode, std::vector<uint8_t>(), module)
{
}

Pattern_t::Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, std::vector<uint8_t> prologue, lm_module_t* module)
	:
	name(name),
	pattern(pattern),
	followMode(followMode),
	prologue(prologue),
	module(module)
{
	Patterns::patterns.emplace_back(this);
}

bool Pattern_t::find()
{
	address = MemHlp::searchSignature(name.c_str(), pattern.c_str(), module ? *module : g_modSteamClient , followMode, &prologue[0], prologue.size());
	return address != LM_ADDRESS_BAD;
}

bool Patterns::init()
{
	bool found = true;

	for(auto& pattern : patterns)
	{
		if (!pattern->find())
		{
			found = false;
		}
	}

	return found;
}

using SigFollowMode = MemHlp::SigFollowMode;

namespace Patterns
{
	Pattern_t FamilyGroupRunningApp
	{
		"FamilyGroupRunningApp",
		"E8 ? ? ? ? 83 C4 10 83 EC 08 C7 46 ? 01 00 00 00 C6 46 ? 01 56 57 E8 ? ? ? ? 83 C4 1C B8 01 00 00 00 5B 5E 5F 5D C3 ? ? ? ? ? ? ? 83 EC 04",
		SigFollowMode::Relative
	};
	Pattern_t StopPlayingBorrowedApp
	{
		"StopPlayingBorrowedApp",
		"8B 40 ? 83 EC 0C 89 F3 8B 95",
		SigFollowMode::PrologueUpwards,
		std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
	};

	Pattern_t GetDepotDecryptionKey
	{
		"GetDepotDecryptionKey",
		// Entry of the depot-key local loader (3-arg, ESP-relative, no EBP frame).
		// The bare prologue is shared by ~4 sites, so the signature is extended with
		// function-specific body bytes (stack canary store, arg3 load into ESI,
		// PUSH 0x80 / PUSH ESI) to resolve to a single location. The PIC thunk call
		// and the ADD EBX,GOT displacement are wildcarded (build-relative).
		"57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC 18 01 00 00 65 A1 14 00 00 00 89 84 24 14 01 00 00 31 C0 8B B4 24 30 01 00 00 68 80 00 00 00 56",
		SigFollowMode::None
	};

	Pattern_t TraceIPC
	{
		"TraceIPC",
		"E8 ? ? ? ? 83 C4 10 85 FF 74 ? 8B 07 83 EC 04 FF B5 ? ? ? ? FF B5 ? ? ? ? 57 FF 10 83 C4 10 8D 45 ? 83 EC 04 89 F3 6A 04 50 FF 75",
		SigFollowMode::Relative
	};

	namespace CAPIJob
	{
		Pattern_t GetPlayerStats
		{
			"CAPIJob::GetPlayerStats",
			"E8 ? ? ? ? 83 C4 10 89 C5 E9 ? ? ? ? ? ? 80 BE ? ? ? ? 00",
			SigFollowMode::Relative
		};
	}

	namespace CProtoBufMsgBase
	{
		Pattern_t InitFromPacket
		{
			"CProtoBufMsgBase::InitFromPacket",
			"E8 ? ? ? ? 58 8B 45 ? 8B 8D",
			SigFollowMode::Relative
		};
		Pattern_t Send
		{
			"CProtoBufMsgBase::Send",
			"E8 ? ? ? ? 59 5A 50 56 E8 ? ? ? ? 83 C4 0C",
			SigFollowMode::Relative
		};
	};

	namespace CSteamEngine
	{
		Pattern_t Init
		{
			"CSteamEngine::Init",
			"E8 ? ? ? ? 83 C4 10 8D 83 ? ? ? ? 83 EC 0C 89 AB",
			SigFollowMode::Relative
		};
		Pattern_t SetAppIdForCurrentPipe
		{
			"CSteamEngine::SetAppIdForCurrentPipe",
			"E8 ? ? ? ? E9 ? ? ? ? ? ? ? ? ? 8B 85 ? ? ? ? 83 EC 08 FF B5",
			SigFollowMode::Relative
		};
		Pattern_t Offset_User
		{
			"CSteamEngine::m_pUser",
			"8B 80 ? ? ? ? FF 75 ? 8D 34",
			SigFollowMode::None
		};
	}

	namespace CSteamMatchmakingServers
	{
		Pattern_t GetServerDetails
		{
			"CSteamMatchmakingServers::GetServerDetails",
			"89 45 ? 83 C4 10 83 EC 0C 89 F3",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t RequestInternetServerList
		{
			"CSteamMatchmakingServers::RequestInternetServerList",
			"C7 04 24 50 03 00 00 E8 ? ? ? ? 5A 89 45 ? 59 FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? 6A 01",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0xe8, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace CUser
	{
		Pattern_t CheckAppOwnership
		{
			"CUser::CheckAppOwnership",
			"E8 ? ? ? ? 88 45 ? 83 C4 10 84 C0 0F 84 ? ? ? ? 8B 45 ? 80 7D ? 00",
			SigFollowMode::Relative
		};
		Pattern_t GetSubscribedApps
		{
			"CUser::GetSubscribedApps",
			"E8 ? ? ? ? 89 C6 83 C4 10 85 C0 0F 84 ? ? ? ? 8B 9D ? ? ? ? 39 D8",
			SigFollowMode::Relative
		};
		Pattern_t PostCallback
		{
			"CSteamEngine::PostCallback",
			"E8 ? ? ? ? 8D 86 ? ? ? ? 83 C4 18 68 F6 01 00 00",
			SigFollowMode::Relative
		};
		Pattern_t UpdateAppOwnershipTicket
		{
			"IClientUser::UpdateAppOwnershipTicket",
			"E8 ? ? ? ? E9 ? ? ? ? ? ? ? ? ? ? 8D 45 ? 89 45 ? EB",
			SigFollowMode::Relative
		};
	}

	namespace IClientAppManager
	{
		Pattern_t RunIPCFrame
		{
			"IClientAppManager::RunIPCFrame",
			"FF B5 ? ? ? ? 50 8D 86 ? ? ? ? 68 90 09 00 00",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t BCanRemotePlayTogether
		{
			"IClientAppManager::BCanRemotePlayTogether",
			"58 5A FF 74 24 ? 56 E8 ? ? ? ? 83 C4 10 85 C0 74",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0xe8, 0x53, 0x56, 0x57 }
		};
	}

	namespace IClientApps
	{
		Pattern_t RunIPCFrame
		{
			"IClientApps::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 39 9C 88 A6",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace IClientRemoteStorage
	{
		Pattern_t RunIPCFrame
		{
			"IClientRemoteStorage::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 6E E8 2F 87",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace IClientUser
	{
		Pattern_t RunIPCFrame
		{
			"IClientUser::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 10 A3 86 73",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};

		Pattern_t BLoggedOn
		{
			"IClientUser::BLoggedOn",
			"E9 ? ? ? ? ? ? ? ? ? ? 5B 5E 5F FF E0",
			SigFollowMode::Relative
		};
		Pattern_t BUpdateAppOwnershipTicket
		{
			"IClientUser::BUpdateAppOwnershipTicket",
			"83 EC 0C 89 F3 8B 7D ? FF 30 E8 ? ? ? ? 83 C4 10 83 FF 01 77 ? 84 C0 75 ? 80 7D ? 00 74 ? 80 7D ? 00 0F 84 ? ? ? ? 8D 65",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t GetAppOwnershipTicketExtendedData
		{
			"IClientUser::GetAppOwnershipTicketExtendedData",
			"83 EC 24 FF 74 24 ? 8B 44 24",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x53, 0x56, 0x57, 0x55 }
		};
		Pattern_t GetSteamId
		{
			"IClientUser::GetSteamID",
			//Not unique. All matches point to correct function though
			"E8 ? ? ? ? 89 D8 83 C4 0C 83 C4 08 5B C2 04 00 ? 83 EC 08 50 53 FF D2 89 D8 83 C4 0C 83 C4 08 5B C2 04 00",
			SigFollowMode::Relative
		};
		Pattern_t IsUserSubscribedAppInTicket
		{
			"IClientUser::IsUserSubscribedAppInTicket",
			"E8 ? ? ? ? 89 C3 83 C4 20 8B ? ? ? ? ? 8B",
			SigFollowMode::Relative
		};
		Pattern_t RequiresLegacyCDKey
		{
			"IClientUser::RequiresLegacyCDKey",
			"75 ? 83 C4 1C 31 C0 5B 5E 5F 5D C3 ? ? ? ? ? 8B 44 24 ? 83 C4 1C 89 F9 89 F2 5B 5E 5F 5D 2D D8 18 00 00",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x53, 0x56, 0x57, 0x55 }
		};
	}

	namespace IClientUGC
	{
		Pattern_t RunIPCFrame
		{
			"IClientUGC::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 67 0C D2 71",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace IClientUserStats
	{
		Pattern_t RunIPCFrame
		{
			"IClientUserStats::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 89 65 6D 87",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace IClientUtils
	{
		Pattern_t RunIPCFrame
		{
			"IClientUtils::RunIPCFrame",
			"83 EC 08 89 F3 50 57 E8 ? ? ? ? 58 FF B5 ? ? ? ? E8 ? ? ? ? 58 8D 45",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t Offset_GetPipeIndex
		{
			"IClientUtils::m_PipeIndex",
			"8B 91 ? ? ? ? 83 F8 FF 74 ? 8B 89 ? ? ? ? EB ? ? ? ? 8B 00 83 F8 FF 74 ? 8D 04 ? 8D 04 ? 3B 50",
			SigFollowMode::None,
		};
	}

	//steamui.so
	namespace ISteamMatchmakingPingResponse
	{
		Pattern_t ServerResponded
		{
			"ISteamMatchmakingPingResponse::ServerResponded",
			"8B 85 ? ? ? ? 8B 40 ? 85 C0 0F 84 ? ? ? ? 39 46",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x57, 0xe5, 0x89, 0x55 },
			&g_modSteamUI
		};
	}

	std::vector<Pattern_t*> patterns;
}

