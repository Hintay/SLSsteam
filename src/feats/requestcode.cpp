#include "requestcode.hpp"

#include "../sdk/CProtoBufMsgBase.hpp"   // EMsgType enum + CMsgProtoBufHeader
#include "../sdk/EResult.hpp"
#include "../sdk/RawNetPacket.hpp"
#include "slssteam_messages.pb.h"

#include "../lua/LuaLoader.hpp"

#include "../ownership.hpp"

#include "../globals.hpp"
#include "../log.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Manifest request-code interception at the raw packet layer.
//
// Drop the outgoing ContentServerDirectory.GetManifestRequestCode#1 frame so the
// CM never sees the request, then fabricate the ServiceMethodResponse entirely
// client-side and inject it through the recv path by borrowing the next incoming
// packet as a carrier. The fabricated response header is derived from the
// request's own header (same connection context) so it is well-formed.
//
// Hook points: outgoing CWebSocketConnection::BBuildAndAsyncSendFrame (opcode 0x2
// binary frames), incoming CCMConnection::RecvPkt(CNetPacket*).

namespace
{
	constexpr char TARGET_JOB_NAME[] = "ContentServerDirectory.GetManifestRequestCode#1";

	// A dropped request awaiting its async code fetch. reqHdr is the request's own
	// protobuf header bytes, transformed into the response header at inject time.
	struct Pending {
		uint64_t                    jobId;
		uint64_t                    gid;
		std::shared_future<uint64_t> future;
		std::vector<uint8_t>        reqHdr;
	};

	std::vector<Pending> g_pending;
	std::mutex           g_mutex;

	// gids already warned about (no code -> fabricated denial). The notification fires once per gid:
	// each retry of the same depot arrives with a fresh jobid, so without this the same failing
	// download would pop a critical notify-send on every attempt.
	std::mutex                   g_deniedGidMtx;
	std::unordered_set<uint64_t> g_deniedGids;

	// Lock-free fast-path gate for the recv hook (runs on every incoming packet):
	// mirrors g_pending.size(); only written under g_mutex.
	std::atomic<size_t>  g_pendingCount{0};

	// Fabricated packet buffer. Written and consumed on the single CM recv thread
	// within one RecvPkt call, so it needs no synchronisation of its own.
	std::vector<uint8_t> g_injectPkt;

	// Build a 147 ServiceMethodResponse into g_injectPkt: response header derived
	// from the dropped request's header (jobid_target = request jobid_source,
	// eresult set, jobid_source cleared), body carries manifest_request_code on
	// success. On fetch failure, eresult=ACCESS_DENIED + no code so Steam fails the
	// download gracefully (same as the real server denial we replaced).
	bool buildInject(const Pending& p, uint64_t code)
	{
		CMsgProtoBufHeader hdr;
		if (!hdr.ParseFromArray(p.reqHdr.data(), static_cast<int>(p.reqHdr.size())))
			return false;
		hdr.set_jobid_target(p.jobId);   // response targets the request's source job
		hdr.clear_jobid_source();
		if (code)
		{
			hdr.set_eresult(ERESULT_OK);
		}
		else
		{
			// Match the real CM denial response byte-for-byte (captured from a live CM Access Denied):
			// a failed ServiceMethod RPC carries eresult=ACCESS_DENIED PLUS transport_error=1 and
			// seq_num=1. Without transport_error Steam does not treat the packet as a proper RPC
			// failure, so the download manager never gets a clean result and the request loops.
			hdr.set_eresult(ERESULT_ACCESS_DENIED);
			hdr.set_transport_error(1);
			hdr.set_seq_num(1);
		}

		CContentServerDirectory_GetManifestRequestCode_Response resp;
		if (code) resp.set_manifest_request_code(code);

		std::string hdrBytes, bodyBytes;
		if (!hdr.SerializeToString(&hdrBytes) || !resp.SerializeToString(&bodyBytes)) return false;

		const uint8_t* outData = nullptr;
		uint32_t outSize = 0;
		return netpacket::AssembleRaw(g_injectPkt,
		                              static_cast<uint32_t>(EMSG_SERVICE_METHOD_RESPONSE) | kMsgHdrProtoFlag,
		                              hdrBytes.data(), hdrBytes.size(),
		                              bodyBytes.data(), bodyBytes.size(),
		                              outData, outSize);
	}
}

namespace RequestCode
{
	bool onSendFrame(const uint8_t* pubData, uint32_t cubData)
	{
		uint16_t eMsg = 0;
		const uint8_t *pHdr = nullptr, *pBody = nullptr;
		uint32_t cbHdr = 0, cbBody = 0;
		if (!netpacket::UnpackRaw(pubData, cubData, eMsg, pHdr, cbHdr, pBody, cbBody)) return false;
		if (eMsg != EMSG_SERVICE_METHOD_CALL_FROM_CLIENT) return false;

		CMsgProtoBufHeader hdr;
		if (!hdr.ParseFromArray(pHdr, static_cast<int>(cbHdr))) return false;
		if (!hdr.has_target_job_name() || hdr.target_job_name() != TARGET_JOB_NAME) return false;
		if (!hdr.has_jobid_source()) return false;

		CContentServerDirectory_GetManifestRequestCode_Request req;
		if (!req.ParseFromArray(pBody, static_cast<int>(cbBody))) return false;
		if (!req.has_depot_id() || !req.has_manifest_id()) return false;

		const uint64_t jobId   = hdr.jobid_source();
		const uint32_t depotId = req.depot_id();
		const uint64_t gid     = req.manifest_id();
		const uint32_t appId   = req.has_app_id() ? req.app_id() : 0;

		// Only fabricate request codes for apps we spoof ownership of (lua/yaml-injected and NOT
		// genuinely owned). Genuine apps must reach the CM so Steam fetches their real code —
		// intercepting them and then failing to produce a code (not in the lua set, or a provider
		// error like opensteamtool 403) fabricates a denial and BLOCKS a legitimate download.
		// shouldSpoofOwnership is a config-set + genuine-owned-set lookup, safe to call on this frame
		// thread (it never makes a Steam client call). appId==0 (absent in the request) -> pass through.
		if (!appId || !Ownership::shouldSpoofOwnership(appId))
			return false;

		g_pLog->debug("RequestCode: drop+fabricate for app=%u depot=%u gid=%llu (jobid=%llu)\n",
		              appId, depotId, static_cast<unsigned long long>(gid),
		              static_cast<unsigned long long>(jobId));

		// Async fetch the code off-thread. The lambda swallows every exception so
		// nothing unwinds through the C trampoline.
		std::shared_future<uint64_t> future;
		try
		{
			future = std::async(std::launch::async,
				[appId, depotId, gid]() -> uint64_t
				{
					try
					{
						uint64_t code = 0;
						if (LuaLoader::fetchManifestCode(appId, depotId, gid, code)) return code;
					}
					catch (const std::exception& e) { g_pLog->warn("RequestCode: fetch threw: %s\n", e.what()); }
					catch (...) { g_pLog->warn("RequestCode: fetch threw an unknown exception\n"); }
					return 0;
				}).share();
		}
		catch (const std::exception& e)
		{
			// Could not launch the fetch: let the request go to the CM normally
			// (graceful fallback) rather than dropping it with no fabricated reply.
			g_pLog->warn("RequestCode: failed to launch fetch for jobid=%llu: %s — passing request through\n",
			             static_cast<unsigned long long>(jobId), e.what());
			return false;
		}

		Pending p;
		p.jobId  = jobId;
		p.gid    = gid;
		p.future = std::move(future);
		p.reqHdr.assign(pHdr, pHdr + cbHdr);

		std::lock_guard<std::mutex> lock(g_mutex);
		g_pending.push_back(std::move(p));
		g_pendingCount.store(g_pending.size(), std::memory_order_release);
		return true; // DROP the outgoing frame
	}

	bool nextInjection(const uint8_t*& outData, uint32_t& outSize)
	{
		if (g_pendingCount.load(std::memory_order_acquire) == 0) return false; // fast path

		Pending ready;
		bool have = false;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			for (auto it = g_pending.begin(); it != g_pending.end(); ++it)
			{
				if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
					continue;
				ready = std::move(*it);
				g_pending.erase(it);
				g_pendingCount.store(g_pending.size(), std::memory_order_release);
				have = true;
				break;
			}
		}
		if (!have) return false;

		uint64_t code = 0;
		try { code = ready.future.get(); }
		catch (const std::exception& e)
		{
			g_pLog->warn("RequestCode: fetch exception for jobid=%llu\n",
			             static_cast<unsigned long long>(ready.jobId));
			(void)e;
		}

		if (!buildInject(ready, code))
		{
			g_pLog->warn("RequestCode: failed to build fabricated response for jobid=%llu\n",
			             static_cast<unsigned long long>(ready.jobId));
			return false;
		}

		if (code)
		{
			// Clear any prior denial mark for this gid: a later failure of the same depot should
			// re-notify, and successfully-fetched gids shouldn't accumulate in the set for the
			// process lifetime.
			{
				std::lock_guard<std::mutex> lk(g_deniedGidMtx);
				g_deniedGids.erase(ready.gid);
			}
			g_pLog->info("RequestCode: injected fabricated response code=%llu for jobid=%llu\n",
			             static_cast<unsigned long long>(code), static_cast<unsigned long long>(ready.jobId));
		}
		else
		{
			bool firstForGid;
			{
				std::lock_guard<std::mutex> lk(g_deniedGidMtx);
				firstForGid = g_deniedGids.insert(ready.gid).second;
			}
			// Warn (critical notify) once per gid; later same-gid denials (new jobids on retry) go to
			// debug so a single failing download doesn't pop a notification on every attempt.
			if (firstForGid)
				g_pLog->warn("RequestCode: no manifest code for gid=%llu (all providers failed) — fabricated Access Denied; this download will fail\n",
				             static_cast<unsigned long long>(ready.gid));
			else
				g_pLog->debug("RequestCode: no code for gid=%llu jobid=%llu, fabricated Access Denied (already warned)\n",
				              static_cast<unsigned long long>(ready.gid),
				              static_cast<unsigned long long>(ready.jobId));
		}

		outData = g_injectPkt.data();
		outSize = static_cast<uint32_t>(g_injectPkt.size());
		return true;
	}
}
