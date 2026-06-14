#include "patterns.hpp"

#include "globals.hpp"
#include "memhlp.hpp"

#include "libmem/libmem.h"

#include <algorithm>
#include <memory>


Pattern_t::Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, lm_module_t* module, bool optional)
	:
	Pattern_t(name, pattern, followMode, std::vector<uint8_t>(), module, optional)
{
}

Pattern_t::Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, std::vector<uint8_t> prologue, lm_module_t* module, bool optional)
	:
	name(name),
	pattern(pattern),
	followMode(followMode),
	prologue(prologue),
	optional(optional),
	address(LM_ADDRESS_BAD),
	module(module)
{
	Patterns::patterns.emplace_back(this);
}

bool Pattern_t::find()
{
	address = MemHlp::searchSignature(name.c_str(), pattern.c_str(), module ? *module : g_modSteamClient , followMode, prologue.data(), prologue.size());
	return address != LM_ADDRESS_BAD;
}

bool Patterns::init()
{
	bool found = true;

	for(auto& pattern : patterns)
	{
		if (!pattern->find() && !pattern->optional)
		{
			found = false;
		}
	}

	return found;
}

using SigFollowMode = MemHlp::SigFollowMode;

namespace Patterns
{
	// MUST stay before every Pattern_t below. Each Pattern_t constructor
	// registers itself here via patterns.emplace_back(this). Defined AFTER the
	// Pattern_t objects (same TU), this vector's default-construction runs last
	// in definition order and wipes all registrations — a static-init-order bug
	// that leaves the container empty, so no pattern ever resolves (every hook
	// that relies on a signature silently fails). Keep it first.
	std::vector<Pattern_t*> patterns;

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

	Pattern_t LoadDepotDecryptionKey
	{
		"LoadDepotDecryptionKey",
		// Entry of the generic KV value reader Steam uses to load a depot
		// decryption key (5-arg cdecl: pObject, foo, KeyName, Key, KeySize;
		// reached via a vtable+0x18 virtual call). The bare prologue
		// (push ebp/edi/esi/ebx) is shared,
		// so the signature is extended with the distinctive argument-spill
		// sequence (sub esp,0x24; mov eax,[esp+0x44]; mov ebp,[esp+0x38]; ...)
		// that resolves to a single location. The PIC thunk call and the
		// ADD EBX,GOT displacement are wildcarded (build-relative).
		"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 83 EC 24 8B 44 24 44 8B 6C 24 38 8B 7C 24 3C 8B 74 24 40 89 44 24 10 8B 44 24 48 89 44 24 14",
		SigFollowMode::None
	};

	Pattern_t BuildDepotDependency
	{
		"BuildDepotDependency",
		// Entry of BuildDepotDependency (8-arg, cdecl, EBP frame). The bare
		// prologue (push ebp; mov ebp,esp; push edi; push esi) is shared by many
		// sites, so the signature is extended with the distinctive stack reserve
		// (sub esp,0x22c) and the argument-spill sequence that follows, which
		// resolves to a single location. The PIC thunk call and the ADD ESI,GOT
		// displacement are wildcarded (build-relative).
		"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 2C 02 00 00 8B 45 08 89 85 1C FE FF FF 8B 45 10 89 85 24 FE FF FF",
		SigFollowMode::None
	};

	Pattern_t BUpdateAppDownloadPlan
	{
		"BUpdateAppDownloadPlan",
		// Internal steamclient helper, unofficial name. Plans or refreshes an
		// app's depot download/update work and calls BuildDepotDependency.
		// Entry at 00ff3250: PIC thunk; ADD GOT; EBP frame;
		// sub esp,0xdc; spill arg3; read arg1->flags. The PIC thunk call and
		// GOT displacement are wildcarded.
		"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 81 EC DC 00 00 00 89 85 50 FF FF FF 8B 45 10 89 85 40 FF FF FF 8B 45 08 8B 40 04",
		SigFollowMode::None,
		nullptr,
		true
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
		Pattern_t MarkLicenseAsChanged
		{
			"CUser::MarkLicenseAsChanged",
			"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 83 EC 2C 8B 74 24 40 8B 44 24 48 8B BE 18 1B 00 00 88 44 24 1C 8D 86 E8 1A 00 00",
			MemHlp::SigFollowMode::None
		};
		Pattern_t ProcessPendingLicenseUpdates
		{
			"CUser::ProcessPendingLicenseUpdates",
			"55 E8 ? ? ? ? 81 C5 ? ? ? ? 57 56 53 83 EC 0C 8B 7C 24 20 8B B7 D4 1B 00 00 83 EE 01 79 0F EB 5D",
			MemHlp::SigFollowMode::None
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

	namespace CPackageInfo
	{
		Pattern_t GetPackageInfo
		{
			"CPackageInfo::GetPackageInfo",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 3C 8B 55 14 8B 75 0C 89 45 CC 8B 45 10 89 55 DC 89 45 D8 8B 45 08",
			MemHlp::SigFollowMode::None
		};
	}

	namespace CConfigStore
	{
		Pattern_t SharedConfigWriteCallsite
		{
			"CConfigStore::FlushToDisk sharedconfig write callsite",
			// Store-2 sharedconfig.vdf branch. The indirect call at the end is intentionally
			// not hooked (Frida proved mid-call instrumentation crashes Steam); this anchor is
			// only used to validate the return address from WriteVdfFile.
			"8B 85 50 EF FF FF 83 C4 28 8B 80 50 3B 00 00 FF B5 9C EF FF FF FF B5 8C EF FF FF 53 6A 00 6A 07 FF B5 D4 EE FF FF FF 10 83 C4 20 83 F8 01",
			MemHlp::SigFollowMode::None,
			nullptr,
			true
		};

		Pattern_t WriteVdfFile
		{
			"CConfigStore::WriteVdfFile",
			// FileWrite-like function reached from FlushToDisk's sharedconfig write callsite.
			// Runtime validation: arg4 is the serialized UserRoamingConfigStore buffer and
			// arg5 its size; return address is the callsite+0x28 instruction after CALL [EAX].
			"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC EC 00 00 00 65 8B 0D 14 00 00 00 89 8C 24 DC 00 00 00 31 C9 8B B4 24 14 01 00 00 8B 94 24 00 01 00 00 8B 84 24 0C 01 00 00",
			MemHlp::SigFollowMode::None,
			nullptr,
			true
		};
	}

	namespace IClientRemoteStorage
	{
		Pattern_t SetCloudEnabledForApp
		{
			"IClientRemoteStorage::SetCloudEnabledForApp (idx25 identity check)",
			// Entry of the function the IClientRemoteStorage vtable[25] points to. Resolved at init only
			// to VALIDATE that the live vtable[25] still holds this exact function before the badge fix
			// calls it raw: a Steam-update VFT reorder makes vtable[25] != this address, so the call is
			// skipped (safe degradation) instead of crashing on a wrong function. The PIC pc-thunk CALL
			// rel32 and the following ADD EBX <GOT-imm32> are position-dependent and wildcarded; the
			// frame size (SUB ESP,0x12c) and the three arg loads at [ESP+0x140/0x144/0x148] make it
			// unique in steamclient (verified: single match at the function entry). Optional — if it
			// ever fails to resolve, the hook falls back to a weaker in-module slot sanity check.
			"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC 2C 01 00 00 8B 84 24 48 01 00 00 8B B4 24 44 01 00 00 8B AC 24 40 01 00 00",
			MemHlp::SigFollowMode::None,
			nullptr,
			true
		};
	}

	namespace CUtlMemory
	{
		// Anchor on a unique call-site; SigFollowMode::Relative follows the E8 to Grow.
		Pattern_t Grow
		{
			"CUtlMemory<uint32>::Grow",
			"E8 ? ? ? ? 8B 85 04 FF FF FF 83 C4 10 8B 40 54 89 85 FC FE FF FF",
			MemHlp::SigFollowMode::Relative
		};
	}

	namespace CWebSocketConnection
	{
		Pattern_t BBuildAndAsyncSendFrame
		{
			"CWebSocketConnection::BBuildAndAsyncSendFrame",
			// Entry: push ebp; mov ebp,esp; push edi; PIC thunk; add edi,GOT; push esi;
			// push ebx; sub esp,0xac; mov eax,[ebp+0x10] (3rd arg = pubData). PIC call +
			// GOT displacement wildcarded. Outgoing raw-packet send.
			"55 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 53 81 EC AC 00 00 00 8B 45 10",
			MemHlp::SigFollowMode::None
		};
	}

	namespace CCMConnection
	{
		Pattern_t RecvPkt
		{
			"CCMConnection::RecvPkt",
			// Entry: push ebp; mov ebp,esp; push edi; PIC thunk; add edi,GOT; push esi;
			// push ebx; sub esp,0x34; push 1; push [ebp+0xc] (CNetPacket* arg2); mov ebx,edi;
			// call (downstream classifier). PIC call + GOT displacement wildcarded.
			"55 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 53 83 EC 34 6A 01 FF 75 0C 89 FB E8 ? ? ? ?",
			MemHlp::SigFollowMode::None
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

	namespace CSteamUIAppController
	{
		Pattern_t GetAppByID
		{
			"CSteamUIAppController::GetAppByID",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 4C 8B 5D 10 8B 75 08 89 45 C8 8B 80 E0 09 00 00",
			MemHlp::SigFollowMode::None, &g_modSteamUI
		};
		Pattern_t FillInAppOverview
		{
			"CSteamUIAppController::FillInAppOverview",
			"55 89 E5 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC 8C 00 00 00 8B 7D 08 89 5D A4 E8 ? ? ? ? 84 C0",
			MemHlp::SigFollowMode::None, &g_modSteamUI
		};
	}
	namespace CUpdateManager
	{
		Pattern_t MarkAppChange
		{
			"CUpdateManager::MarkAppChange",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 3C 8B 5D 10 8B 75 0C 89 45 D4 8B 80 E0 09 00 00 85 DB 0F 94 C1",
			MemHlp::SigFollowMode::None, &g_modSteamUI
		};
	}
}
