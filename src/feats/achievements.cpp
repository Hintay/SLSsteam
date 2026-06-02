#include "achievements.hpp"

#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/EResult.hpp"
#include "../sdk/protobufs/slssteam_servicemethods.pb.h"
#include "../sdk/protobufs/steammessages_clientserver_userstats.pb.h"

#include "../config.hpp"
#include "../globals.hpp"
#include "../lua/LuaLoader.hpp"
#include "../log.hpp"

#include <mutex>
#include <unordered_map>

// User-stats interception (mirrors OST Hooks_NetPacket.cpp UserStats section).
//
// Steam fetches achievement/stat data for the signed-in account via two paths:
//   1. ServiceMethod (EMsg 151/147): Player.GetUserStats#1
//   2. Legacy client message (EMsg 818/819): CMsgClientGetUserStats
//
// For a controlled app the user does NOT genuinely own, we redirect the stats
// query to a configurable donor Steam ID so the donor account's achievements
// are fetched instead of the signed-in account's. The response is then cleared
// so Steam falls back to its local cache — the same as OST's approach.
//
// Redirect gate: shouldRedirectStats(appId) = isAddedAppId(appId) && !isSubscribed(appId)
//   - isAddedAppId covers both YAML addedAppIds and Lua ownedAppIds (merged at
//     init) — the "this app is managed by SLSsteam" set, consistent with the
//     gate used by Apps::checkAppOwnership and the DLC hooks.
//   - The added !isSubscribed() term excludes apps the user genuinely owns:
//     OST redirects every configured app unconditionally, but for a genuinely
//     owned app the real account already has valid achievements, so pulling a
//     donor's data would clobber them. Non-owned controlled apps still follow
//     OST's donor-redirect behaviour (donor = setstat() override, else default).
namespace Achievements
{
	namespace
	{
		// ServiceMethod target name for the Player.GetUserStats call.
		constexpr char TARGET_JOB_NAME[] = "Player.GetUserStats#1";

		// jobid_source (outgoing call) -> appId for pending ServiceMethod requests.
		// Protected by g_mutex; accessed from the Send hook and the InitFromPacket hook.
		// Bounded by the number of controlled apps; an orphaned entry (151 sent but
		// no 147 ever returns) is benign — at most one per app per session.
		std::unordered_map<uint64_t, uint32_t> g_pending;
		std::mutex g_mutex;

		// Returns true if appId is controlled by SLSsteam (in the added-app set).
		inline bool isControlled(uint32_t appId)
		{
			return g_config.isAddedAppId(appId);
		}

		// Returns true if a stats query for appId should be redirected to the
		// configured donor SteamID. We only redirect apps the user does NOT
		// genuinely own: for a genuinely-owned app (isSubscribed true) the real
		// account already has valid achievements, so pulling a stranger's data
		// would clobber them. Non-owned controlled apps still follow OST's
		// donor-redirect behaviour. isSubscribed is the same genuine-ownership
		// oracle used by Apps/DLC/FakeAppIds and is not spoofed for added apps.
		inline bool shouldRedirectStats(uint32_t appId)
		{
			return isControlled(appId) &&
			       !g_pSteamEngine->getUser(0)->isSubscribed(appId);
		}
	}

	// ── Outgoing hook ────────────────────────────────────────────────────────

	void sendMessage(CProtoBufMsgBase* msg)
	{
		if (!msg)
		{
			return;
		}

		// ── Path 1: ServiceMethod Player.GetUserStats#1 (EMsg 151) ────────────
		if (msg->type == EMSG_SERVICE_METHOD_CALL_FROM_CLIENT && msg->header)
		{
			if (msg->header->has_target_job_name() &&
			    msg->header->target_job_name() == TARGET_JOB_NAME)
			{
				const auto body = msg->getBody<CPlayer_GetUserStats_Request>();

				// OST condition: has appid, no sha_schema (initial fetch, not schema-only probe).
				if (!body->has_appid() || body->has_sha_schema())
				{
					return;
				}

				const uint32_t appId = body->appid();
				if (!shouldRedirectStats(appId))
				{
					return;
				}

				if (!msg->header->has_jobid_source())
				{
					// Malformed packet — skip to avoid a stale k_GIDNil match on recv.
					return;
				}

				const uint64_t jobId = msg->header->jobid_source();
				const uint64_t statSteamId = LuaLoader::getStatSteamId(appId);

				body->set_steamid(statSteamId);

				g_pLog->debug
				(
					"Achievements: ServiceMethod redirect app=%u steamid=%llu (jobid=%llu)\n",
					appId,
					static_cast<unsigned long long>(statSteamId),
					static_cast<unsigned long long>(jobId)
				);

				{
					std::lock_guard<std::mutex> lock(g_mutex);
					g_pending[jobId] = appId;
				}
			}
			return;
		}

		// ── Path 2: Legacy client stats request CMsgClientGetUserStats (EMsg 818) ──
		if (msg->type == EMSG_REQUEST_USERSTATS)
		{
			const auto body = msg->getBody<CMsgClientGetUserStats>();

			if (!body->has_game_id())
			{
				return;
			}

			// schema_local_version == -1 signals an initial live-stats fetch
			// (not a schema-only probe). Mirror OST's condition.
			if (!body->has_schema_local_version() || body->schema_local_version() != -1)
			{
				return;
			}

			// game_id upper 32 bits = game type; low 32 bits = appId (same as OST).
			const uint32_t appId = static_cast<uint32_t>(body->game_id());
			if (!shouldRedirectStats(appId))
			{
				return;
			}

			const uint64_t statSteamId = LuaLoader::getStatSteamId(appId);
			body->set_steam_id_for_user(statSteamId);

			g_pLog->debug
			(
				"Achievements: legacy redirect app=%u steamid=%llu\n",
				appId,
				static_cast<unsigned long long>(statSteamId)
			);
		}
	}

	// ── Incoming hook ────────────────────────────────────────────────────────

	void recvMessage(CProtoBufMsgBase* msg)
	{
		if (!msg)
		{
			return;
		}

		// ── Path 1: ServiceMethod response (EMsg 147) ─────────────────────────
		if (msg->type == EMSG_SERVICE_METHOD_RESPONSE && msg->header)
		{
			const uint64_t jobId = msg->header->jobid_target();

			uint32_t appId = 0;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				const auto it = g_pending.find(jobId);
				if (it == g_pending.end())
				{
					return;
				}
				appId = it->second;
				g_pending.erase(it);
			}

			// Force OK so Steam processes the (now cleared) response instead of
			// treating it as an error and keeping stale/wrong data.
			msg->header->set_eresult(ERESULT_OK);

			const auto body = msg->getBody<CPlayer_GetUserStats_Response>();
			body->clear_stats();

			g_pLog->debug
			(
				"Achievements: ServiceMethod response cleared for app=%u (jobid=%llu)\n",
				appId,
				static_cast<unsigned long long>(jobId)
			);
			return;
		}

		// ── Path 2: Legacy response CMsgClientGetUserStatsResponse (EMsg 819) ──
		if (msg->type == EMSG_REQUEST_USERSTATS_RESPONSE)
		{
			const auto body = msg->getBody<CMsgClientGetUserStatsResponse>();

			if (!body->has_game_id())
			{
				return;
			}

			// game_id upper 32 bits = game type; low 32 bits = appId (same as OST).
			const uint32_t appId = static_cast<uint32_t>(body->game_id());
			if (!shouldRedirectStats(appId))
			{
				return;
			}

			// Clear server-side stats and achievement bits so Steam falls back to
			// its local offline cache (the target account's data fetched via steamid
			// override does not overwrite what the real user has locally).
			body->clear_stats();
			body->clear_achievement_blocks();
			body->set_eresult(ERESULT_OK);

			g_pLog->debug
			(
				"Achievements: legacy response cleared for app=%u\n",
				appId
			);
		}
	}

	// ── CAPIJob_GetPlayerStats hook ──────────────────────────────────────────
	// Previously forced ERESULT_NO_CONNECTION for all apps to prevent Steam from
	// overwriting the local achievement cache with server data. That approach is
	// replaced by the per-app network-level redirection above, so this function
	// is now a no-op. The detour in hooks.cpp is intentionally left in place to
	// avoid out-of-scope changes to patterns.cpp and hooks.cpp wiring.
	void getPlayerStats(uint32_t& eresult)
	{
		// No-op: network hooks redirect stats per-app via steamid substitution.
		(void)eresult;
	}
}
