#include "achievements.hpp"

#include "apps.hpp"

#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"   // EMsgType enum + CMsgProtoBufHeader
#include "../sdk/EResult.hpp"
#include "../sdk/protobufs/slssteam_servicemethods.pb.h"
#include "../sdk/protobufs/steammessages_clientserver_userstats.pb.h"

#include "../lua/LuaLoader.hpp"
#include "../log.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

// User-stats interception at the raw packet layer. Steam fetches achievement/stat
// data via two paths:
//   1. ServiceMethod (EMsg 151/147): Player.GetUserStats#1
//   2. Legacy client message (EMsg 818/819): CMsgClientGetUserStats
//
// The ServiceMethod (151/147) path does NOT traverse CProtoBufMsgBase::Send/
// InitFromPacket on modern Steam (proven during the requestcode work — outgoing 151
// never reached that hook), so achievements is intercepted here at the raw layer:
//   outgoing CWebSocketConnection::BBuildAndAsyncSendFrame
//   incoming CCMConnection::RecvPkt(CNetPacket*)
//
// For added (lua/config) apps that are not genuinely owned, the outgoing query's
// steamid is rewritten to a donor (so the server returns a valid OK response with
// the achievement schema), and the response's stat values are then cleared (Steam
// keeps the schema but falls back to its local cache). Redirect only controlled
// apps that have not been proven genuinely owned by Steam's original ownership
// path. Do not use CUser::isSubscribed() here: package injection can make fake
// ownership look subscribed and would exclude exactly the apps that need redirecting.

namespace Achievements
{
	namespace
	{
		constexpr char TARGET_JOB_NAME[] = "Player.GetUserStats#1";

		constexpr uint32_t kMaxHdr  = 1024;
		constexpr uint32_t kMaxBody = 8192;
		constexpr uint32_t kMaxPkt  = sizeof(MsgHdr) + kMaxHdr + kMaxBody;
		constexpr int      kPoolSize = 8;

		// jobid_source (outgoing 151) -> appId. Touched from the send hook and the
		// recv hook; guard with g_mutex. g_pendingCount is a lock-free fast-path gate
		// for the recv hook (runs on every incoming packet); only written under g_mutex.
		std::unordered_map<uint64_t, uint32_t> g_pending;
		std::mutex          g_mutex;
		std::atomic<size_t> g_pendingCount{0};

		// Replacement-packet pools. The send pool must outlive the async send
		// (BBuildAndAsyncSendFrame copies synchronously, but a small ring keeps the
		// buffer lifetime safe regardless); the recv pool must outlive the oRecvPkt that
		// consumes the rewritten packet. Each pool is touched by a single thread
		// (net-send / cm-recv), so it needs no synchronisation of its own.
		uint8_t g_sendPool[kPoolSize][kMaxPkt];
		int     g_sendIdx = 0;
		uint8_t g_recvPool[kPoolSize][kMaxPkt];
		int     g_recvIdx = 0;

		// Controlled app, unless the original ownership path proved it is genuinely owned.
		inline bool shouldRedirectStats(uint32_t appId)
		{
			return Apps::shouldTreatAsFakeOwned(appId);
		}

		// Assemble [MsgHdr (original eMsg, new headerLength)][newHdr][newBody] into a
		// pooled buffer. Returns false if the parts exceed the pool bounds.
		bool assemble(uint8_t* buf, const MsgHdr* origHdr,
		              const std::string& hdr, const std::string& body,
		              const uint8_t*& out, uint32_t& outSize)
		{
			if (hdr.size() > kMaxHdr || body.size() > kMaxBody) return false;
			MsgHdr* mh = reinterpret_cast<MsgHdr*>(buf);
			mh->eMsg = origHdr->eMsg;
			mh->headerLength = static_cast<uint32_t>(hdr.size());
			memcpy(buf + sizeof(MsgHdr), hdr.data(), hdr.size());
			memcpy(buf + sizeof(MsgHdr) + hdr.size(), body.data(), body.size());
			out = buf;
			outSize = static_cast<uint32_t>(sizeof(MsgHdr) + hdr.size() + body.size());
			return true;
		}
	}

	bool onSendFrame(const uint8_t* pubData, uint32_t cubData,
	                 const uint8_t*& outData, uint32_t& outSize)
	{
		uint16_t eMsg = 0;
		const uint8_t *pHdr = nullptr, *pBody = nullptr;
		uint32_t cbHdr = 0, cbBody = 0;
		if (!netpacket::unpackRaw(pubData, cubData, eMsg, pHdr, cbHdr, pBody, cbBody)) return false;

		// ── Path 1: ServiceMethod Player.GetUserStats#1 (EMsg 151) ────────────
		if (eMsg == EMSG_SERVICE_METHOD_CALL_FROM_CLIENT)
		{
			CMsgProtoBufHeader hdr;
			if (!hdr.ParseFromArray(pHdr, static_cast<int>(cbHdr))) return false;
			if (!hdr.has_target_job_name() || hdr.target_job_name() != TARGET_JOB_NAME) return false;

			CPlayer_GetUserStats_Request req;
			if (!req.ParseFromArray(pBody, static_cast<int>(cbBody))) return false;
			// Initial live-stats fetch: app id is present and this is not a schema-only probe.
			if (!req.has_appid() || req.has_sha_schema()) return false;

			const uint32_t appId = req.appid();
			if (!shouldRedirectStats(appId)) return false;
			if (!hdr.has_jobid_source()) return false;

			const uint64_t jobId = hdr.jobid_source();
			const uint64_t donor = LuaLoader::getStatSteamId(appId);
			req.set_steamid(donor);

			std::string newBody;
			if (!req.SerializeToString(&newBody)) return false;
			const std::string keepHdr(reinterpret_cast<const char*>(pHdr), cbHdr); // header unchanged

			{
				std::lock_guard<std::mutex> lock(g_mutex);
				g_pending[jobId] = appId;
				g_pendingCount.store(g_pending.size(), std::memory_order_release);
			}

			const bool ok = assemble(g_sendPool[g_sendIdx], reinterpret_cast<const MsgHdr*>(pubData),
			                         keepHdr, newBody, outData, outSize);
			g_sendIdx = (g_sendIdx + 1) % kPoolSize;
			if (ok)
				g_pLog->debug("Achievements: raw 151 redirect app=%u steamid=%llu (jobid=%llu)\n",
				              appId, static_cast<unsigned long long>(donor), static_cast<unsigned long long>(jobId));
			return ok;
		}

		// ── Path 2: Legacy CMsgClientGetUserStats (EMsg 818) ──────────────────
		if (eMsg == EMSG_REQUEST_USERSTATS)
		{
			CMsgClientGetUserStats req;
			if (!req.ParseFromArray(pBody, static_cast<int>(cbBody))) return false;
			if (!req.has_game_id()) return false;
			// schema_local_version == -1 signals an initial live-stats fetch.
			if (!req.has_schema_local_version() || req.schema_local_version() != -1) return false;

			const uint32_t appId = static_cast<uint32_t>(req.game_id());
			if (!shouldRedirectStats(appId)) return false;

			const uint64_t donor = LuaLoader::getStatSteamId(appId);
			req.set_steam_id_for_user(donor);

			std::string newBody;
			if (!req.SerializeToString(&newBody)) return false;
			const std::string keepHdr(reinterpret_cast<const char*>(pHdr), cbHdr);

			const bool ok = assemble(g_sendPool[g_sendIdx], reinterpret_cast<const MsgHdr*>(pubData),
			                         keepHdr, newBody, outData, outSize);
			g_sendIdx = (g_sendIdx + 1) % kPoolSize;
			if (ok)
				g_pLog->debug("Achievements: raw 818 redirect app=%u steamid=%llu\n",
				              appId, static_cast<unsigned long long>(donor));
			return ok;
		}

		return false;
	}

	void onRecvPacket(CNetPacket* pkt)
	{
		if (!pkt) return;
		uint16_t eMsg = 0;
		const uint8_t *pHdr = nullptr, *pBody = nullptr;
		uint32_t cbHdr = 0, cbBody = 0;
		if (!netpacket::unpackRaw(pkt->m_pubData, pkt->m_cubData, eMsg, pHdr, cbHdr, pBody, cbBody)) return;

		// ── Path 1: ServiceMethod response (EMsg 147) ─────────────────────────
		if (eMsg == EMSG_SERVICE_METHOD_RESPONSE)
		{
			if (g_pendingCount.load(std::memory_order_acquire) == 0) return; // fast path

			CMsgProtoBufHeader hdr;
			if (!hdr.ParseFromArray(pHdr, static_cast<int>(cbHdr)) || !hdr.has_jobid_target()) return;
			const uint64_t jobId = hdr.jobid_target();

			uint32_t appId = 0;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				const auto it = g_pending.find(jobId);
				if (it == g_pending.end()) return;   // not one of ours
				appId = it->second;
				g_pending.erase(it);
				g_pendingCount.store(g_pending.size(), std::memory_order_release);
			}

			CPlayer_GetUserStats_Response resp;
			if (!resp.ParseFromArray(pBody, static_cast<int>(cbBody))) return;
			resp.clear_stats();                 // keep schema, drop the donor's stat values
			hdr.set_eresult(ERESULT_OK);

			std::string newHdr, newBody;
			if (!hdr.SerializeToString(&newHdr) || !resp.SerializeToString(&newBody)) return;

			const uint8_t* out = nullptr; uint32_t outSize = 0;
			if (assemble(g_recvPool[g_recvIdx], reinterpret_cast<const MsgHdr*>(pkt->m_pubData),
			             newHdr, newBody, out, outSize))
			{
				pkt->m_pubData = const_cast<uint8_t*>(out);
				pkt->m_cubData = outSize;
				g_recvIdx = (g_recvIdx + 1) % kPoolSize;
				g_pLog->debug("Achievements: raw 147 cleared app=%u (jobid=%llu)\n",
				              appId, static_cast<unsigned long long>(jobId));
			}
			return;
		}

		// ── Path 2: Legacy response CMsgClientGetUserStatsResponse (EMsg 819) ──
		if (eMsg == EMSG_REQUEST_USERSTATS_RESPONSE)
		{
			CMsgClientGetUserStatsResponse resp;
			if (!resp.ParseFromArray(pBody, static_cast<int>(cbBody)) || !resp.has_game_id()) return;
			const uint32_t appId = static_cast<uint32_t>(resp.game_id());
			if (!shouldRedirectStats(appId)) return;

			resp.clear_stats();
			resp.clear_achievement_blocks();
			resp.set_eresult(ERESULT_OK);

			std::string newBody;
			if (!resp.SerializeToString(&newBody)) return;
			const std::string keepHdr(reinterpret_cast<const char*>(pHdr), cbHdr);

			const uint8_t* out = nullptr; uint32_t outSize = 0;
			if (assemble(g_recvPool[g_recvIdx], reinterpret_cast<const MsgHdr*>(pkt->m_pubData),
			             keepHdr, newBody, out, outSize))
			{
				pkt->m_pubData = const_cast<uint8_t*>(out);
				pkt->m_cubData = outSize;
				g_recvIdx = (g_recvIdx + 1) % kPoolSize;
				g_pLog->debug("Achievements: raw 819 cleared app=%u\n", appId);
			}
			return;
		}
	}

	void getPlayerStats(uint32_t& eresult)
	{
		// No-op: the raw network hooks redirect stats per-app; the CAPIJob detour is
		// kept only for hook-wiring compatibility.
		(void)eresult;
	}
}
