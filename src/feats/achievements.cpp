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
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

		constexpr auto kLegacyPendingTtl = std::chrono::seconds(60);
		constexpr size_t kMaxSchemaFailures = 16;

		struct LegacyPending {
			size_t count = 0;
			std::chrono::steady_clock::time_point lastTouched;
		};

		// jobid_source (outgoing 151) -> appId. Touched from the send hook and the
		// recv hook; guard with g_mutex. The atomics are lock-free fast-path gates for
		// the recv hook (runs on every incoming packet); only written under g_mutex.
		std::unordered_map<uint64_t, uint32_t> g_pending;
		std::unordered_map<uint32_t, LegacyPending> g_legacyPending;
		std::mutex          g_mutex;
		std::atomic<size_t> g_pendingCount{0};
		std::atomic<size_t> g_legacyPendingCount{0};

		struct SchemaFailureResponse {
			uint64_t jobId = 0;
			uint32_t appId = 0;
			std::vector<uint8_t> packet;
		};

		std::deque<SchemaFailureResponse> g_schemaFailures;
		std::atomic<size_t> g_schemaFailureCount{0};

		// Replacement-packet buffers. thread_local avoids cross-thread races while still
		// keeping rewritten packet bytes alive until the immediate trampoline call
		// consumes them. Dynamic sizing is intentional: achievement schemas can exceed
		// the small fixed buffers used by manifest request-code responses.
		thread_local std::vector<uint8_t> t_sendBuf;
		thread_local std::vector<uint8_t> t_recvBuf;
		thread_local std::vector<uint8_t> t_injectBuf;

		// Controlled app, unless the original ownership path proved it is genuinely owned.
		inline bool shouldRedirectStats(uint32_t appId)
		{
			return Apps::shouldTreatAsFakeOwned(appId);
		}

		void publishCountsLocked()
		{
			g_pendingCount.store(g_pending.size(), std::memory_order_release);
			size_t legacyTotal = 0;
			for (const auto& it : g_legacyPending)
			{
				legacyTotal += it.second.count;
			}
			g_legacyPendingCount.store(legacyTotal, std::memory_order_release);
			g_schemaFailureCount.store(g_schemaFailures.size(), std::memory_order_release);
		}

		void purgeExpiredLegacyLocked(std::chrono::steady_clock::time_point now)
		{
			for (auto it = g_legacyPending.begin(); it != g_legacyPending.end();)
			{
				if (now - it->second.lastTouched > kLegacyPendingTtl)
					it = g_legacyPending.erase(it);
				else
					++it;
			}
		}

		void addServicePending(uint64_t jobId, uint32_t appId)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_pending[jobId] = appId;
			publishCountsLocked();
		}

		bool peekServicePending(uint64_t jobId, uint32_t& appId)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const auto it = g_pending.find(jobId);
			if (it == g_pending.end()) return false;
			appId = it->second;
			return true;
		}

		bool consumeServicePending(uint64_t jobId)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const auto erased = g_pending.erase(jobId);
			if (!erased) return false;
			publishCountsLocked();
			return true;
		}

		void addLegacyPending(uint32_t appId)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const auto now = std::chrono::steady_clock::now();
			purgeExpiredLegacyLocked(now);
			auto& pending = g_legacyPending[appId];
			pending.count++;
			pending.lastTouched = now;
			publishCountsLocked();
		}

		bool hasLegacyPending(uint32_t appId)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const auto now = std::chrono::steady_clock::now();
			purgeExpiredLegacyLocked(now);
			publishCountsLocked();
			const auto it = g_legacyPending.find(appId);
			return it != g_legacyPending.end() && it->second.count > 0;
		}

		bool consumeLegacyPending(uint32_t appId)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const auto now = std::chrono::steady_clock::now();
			purgeExpiredLegacyLocked(now);
			const auto it = g_legacyPending.find(appId);
			if (it == g_legacyPending.end() || it->second.count == 0)
			{
				publishCountsLocked();
				return false;
			}
			it->second.count--;
			if (it->second.count == 0)
				g_legacyPending.erase(it);
			else
				it->second.lastTouched = now;
			publishCountsLocked();
			return true;
		}

		// Assemble [MsgHdr (original eMsg, new headerLength)][newHdr][newBody] into a
		// caller-provided buffer. Returns false only if the new packet is too large for
		// CNetPacket's uint32_t size fields.
		bool assemble(std::vector<uint8_t>& buf, const MsgHdr* origHdr,
		              const std::string& hdr, const std::string& body,
		              const uint8_t*& out, uint32_t& outSize)
		{
			if (hdr.size() > std::numeric_limits<uint32_t>::max()) return false;
			const size_t prefixSize = sizeof(MsgHdr) + hdr.size();
			if (prefixSize < hdr.size()) return false;
			const size_t total = prefixSize + body.size();
			if (total < prefixSize || total > std::numeric_limits<uint32_t>::max()) return false;

			buf.resize(total);
			MsgHdr* mh = reinterpret_cast<MsgHdr*>(buf.data());
			mh->eMsg = origHdr->eMsg;
			mh->headerLength = static_cast<uint32_t>(hdr.size());
			memcpy(buf.data() + sizeof(MsgHdr), hdr.data(), hdr.size());
			memcpy(buf.data() + sizeof(MsgHdr) + hdr.size(), body.data(), body.size());
			out = buf.data();
			outSize = static_cast<uint32_t>(total);
			return true;
		}

		bool assembleWithEMsg(std::vector<uint8_t>& buf, uint32_t eMsg,
		                      const std::string& hdr, const std::string& body)
		{
			const uint8_t* out = nullptr;
			uint32_t outSize = 0;
			MsgHdr origHdr;
			origHdr.eMsg = eMsg;
			origHdr.headerLength = 0;
			return assemble(buf, &origHdr, hdr, body, out, outSize);
		}

		bool buildSchemaFailurePacket(CMsgProtoBufHeader hdr, uint64_t jobId,
		                              std::vector<uint8_t>& packet)
		{
			hdr.set_jobid_target(jobId);
			hdr.clear_jobid_source();
			hdr.set_eresult(ERESULT_NO_CONNECTION);

			CPlayer_GetUserStats_Response resp;
			std::string newHdr, newBody;
			if (!hdr.SerializeToString(&newHdr) || !resp.SerializeToString(&newBody)) return false;

			return assembleWithEMsg(packet,
			                    static_cast<uint32_t>(EMSG_SERVICE_METHOD_RESPONSE) | kMsgHdrProtoFlag,
			                    newHdr, newBody);
		}

		bool queueSchemaFailure(uint32_t appId, uint64_t jobId, CMsgProtoBufHeader hdr)
		{
			SchemaFailureResponse failure;
			failure.appId = appId;
			failure.jobId = jobId;
			if (!buildSchemaFailurePacket(std::move(hdr), jobId, failure.packet)) return false;

			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_schemaFailures.size() >= kMaxSchemaFailures) return false;
			g_schemaFailures.push_back(std::move(failure));
			publishCountsLocked();
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
			if (!req.has_appid()) return false;

			const uint32_t appId = req.appid();
			if (req.has_sha_schema())
			{
				if (!shouldRedirectStats(appId)) return false;
				if (!hdr.has_jobid_source()) return false;

				const uint64_t jobId = hdr.jobid_source();
				if (!queueSchemaFailure(appId, jobId, hdr))
				{
					g_pLog->warn("Achievements: failed to queue raw 151 schema no-connection app=%u (jobid=%llu), dropping anyway\n",
					             appId, static_cast<unsigned long long>(jobId));
					outData = nullptr;
					outSize = 0;
					return true;
				}

				outData = nullptr;
				outSize = 0;
				g_pLog->debug("Achievements: raw 151 drop schema probe app=%u sha_schema_len=%zu (jobid=%llu), will inject no-connection\n",
				              appId, req.sha_schema().size(), static_cast<unsigned long long>(jobId));
				return true;
			}

			// Initial live-stats fetch: app id is present and this is not a schema-only probe.
			if (!shouldRedirectStats(appId)) return false;
			if (!hdr.has_jobid_source()) return false;

			const uint64_t jobId = hdr.jobid_source();
			const uint64_t donor = LuaLoader::getStatSteamId(appId);
			req.set_steamid(donor);

			std::string newBody;
			if (!req.SerializeToString(&newBody)) return false;
			const std::string keepHdr(reinterpret_cast<const char*>(pHdr), cbHdr); // header unchanged

			const bool ok = assemble(t_sendBuf, reinterpret_cast<const MsgHdr*>(pubData),
			                         keepHdr, newBody, outData, outSize);
			if (!ok)
			{
				g_pLog->warn("Achievements: failed to assemble raw 151 redirect app=%u (jobid=%llu)\n",
				             appId, static_cast<unsigned long long>(jobId));
				return false;
			}

			addServicePending(jobId, appId);
			g_pLog->debug("Achievements: raw 151 redirect app=%u steamid=%llu (jobid=%llu)\n",
			              appId, static_cast<unsigned long long>(donor), static_cast<unsigned long long>(jobId));
			return true;
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

			const bool ok = assemble(t_sendBuf, reinterpret_cast<const MsgHdr*>(pubData),
			                         keepHdr, newBody, outData, outSize);
			if (!ok)
			{
				g_pLog->warn("Achievements: failed to assemble raw 818 redirect app=%u\n", appId);
				return false;
			}

			addLegacyPending(appId);
			g_pLog->debug("Achievements: raw 818 redirect app=%u steamid=%llu\n",
			              appId, static_cast<unsigned long long>(donor));
			return true;
		}

		return false;
	}

	bool nextInjection(const uint8_t*& outData, uint32_t& outSize)
	{
		if (g_schemaFailureCount.load(std::memory_order_acquire) == 0) return false;

		SchemaFailureResponse failure;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_schemaFailures.empty())
			{
				publishCountsLocked();
				return false;
			}

			failure = std::move(g_schemaFailures.front());
			g_schemaFailures.pop_front();
			publishCountsLocked();
		}

		t_injectBuf = std::move(failure.packet);
		outData = t_injectBuf.data();
		outSize = static_cast<uint32_t>(t_injectBuf.size());
		g_pLog->debug("Achievements: raw 147 injected no-connection for schema probe app=%u (jobid=%llu)\n",
		              failure.appId, static_cast<unsigned long long>(failure.jobId));
		return outData && outSize;
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
			if (!peekServicePending(jobId, appId)) return;   // not one of ours

			CPlayer_GetUserStats_Response resp;
			if (!resp.ParseFromArray(pBody, static_cast<int>(cbBody)))
			{
				g_pLog->warn("Achievements: failed to parse raw 147 response app=%u (jobid=%llu)\n",
				             appId, static_cast<unsigned long long>(jobId));
				return;
			}
			resp.clear_stats();                 // keep schema, drop the donor's stat values
			hdr.set_eresult(ERESULT_OK);

			std::string newHdr, newBody;
			if (!hdr.SerializeToString(&newHdr) || !resp.SerializeToString(&newBody))
			{
				g_pLog->warn("Achievements: failed to serialize raw 147 response app=%u (jobid=%llu)\n",
				             appId, static_cast<unsigned long long>(jobId));
				return;
			}

			const uint8_t* out = nullptr; uint32_t outSize = 0;
			if (!assemble(t_recvBuf, reinterpret_cast<const MsgHdr*>(pkt->m_pubData),
			              newHdr, newBody, out, outSize))
			{
				g_pLog->warn("Achievements: failed to assemble raw 147 response app=%u (jobid=%llu)\n",
				             appId, static_cast<unsigned long long>(jobId));
				return;
			}

			if (consumeServicePending(jobId))
			{
				pkt->m_pubData = const_cast<uint8_t*>(out);
				pkt->m_cubData = outSize;
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

			// Legacy 818/819 does not reliably give us a raw-layer outgoing request to
			// correlate with. Use pending when we saw the 818, but preserve the old
			// working fallback of clearing redirected apps by response appid alone.
			const bool matchedPending = g_legacyPendingCount.load(std::memory_order_acquire) != 0
			                         && hasLegacyPending(appId);

			resp.clear_stats();
			resp.clear_achievement_blocks();
			resp.set_eresult(ERESULT_OK);

			std::string newBody;
			if (!resp.SerializeToString(&newBody))
			{
				g_pLog->warn("Achievements: failed to serialize raw 819 response app=%u\n", appId);
				return;
			}
			const std::string keepHdr(reinterpret_cast<const char*>(pHdr), cbHdr);

			const uint8_t* out = nullptr; uint32_t outSize = 0;
			if (!assemble(t_recvBuf, reinterpret_cast<const MsgHdr*>(pkt->m_pubData),
			              keepHdr, newBody, out, outSize))
			{
				g_pLog->warn("Achievements: failed to assemble raw 819 response app=%u\n", appId);
				return;
			}

			if (matchedPending) consumeLegacyPending(appId);
			pkt->m_pubData = const_cast<uint8_t*>(out);
			pkt->m_cubData = outSize;
			g_pLog->debug("Achievements: raw 819 cleared app=%u%s\n",
			              appId, matchedPending ? "" : " (fallback)");
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
