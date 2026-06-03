#include "requestcode.hpp"

#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"   // EMsgType enum + CMsgProtoBufHeader
#include "../sdk/EResult.hpp"
#include "../sdk/protobufs/slssteam_servicemethods.pb.h"

#include "../lua/LuaLoader.hpp"

#include "../globals.hpp"
#include "../log.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <future>
#include <mutex>
#include <vector>

// Manifest request-code interception at the raw packet layer (OST-style; see
// docs/superpowers/notes/pattern-rederivation-runbook.md §2.B.1).
//
// Unlike OST (which lets the request reach the CM and only rewrites the reply),
// this DROPS the outgoing ContentServerDirectory.GetManifestRequestCode#1 frame
// so the CM never sees the request, then fabricates the ServiceMethodResponse
// entirely client-side and injects it through the recv path by borrowing the
// next incoming packet as a carrier (OST's g_InjectPkt technique). The fabricated
// response header is derived from the request's OWN header (same connection
// context) so it is well-formed.
//
// Hook points: outgoing CWebSocketConnection::BBuildAndAsyncSendFrame (opcode 0x2
// binary frames), incoming CCMConnection::RecvPkt(CNetPacket*).

namespace
{
	constexpr char TARGET_JOB_NAME[] = "ContentServerDirectory.GetManifestRequestCode#1";

	constexpr uint32_t kMaxHdr  = 1024;
	constexpr uint32_t kMaxBody = 8192;
	constexpr uint32_t kMaxPkt  = sizeof(MsgHdr) + kMaxHdr + kMaxBody;

	// A dropped request awaiting its async code fetch. reqHdr is the request's own
	// protobuf header bytes, transformed into the response header at inject time.
	struct Pending {
		uint64_t                    jobId;
		std::shared_future<uint64_t> future;
		std::vector<uint8_t>        reqHdr;
	};

	std::vector<Pending> g_pending;
	std::mutex           g_mutex;
	// Lock-free fast-path gate for the recv hook (runs on every incoming packet):
	// mirrors g_pending.size(); only written under g_mutex.
	std::atomic<size_t>  g_pendingCount{0};

	// Fabricated packet buffer. Written and consumed on the single CM recv thread
	// within one RecvPkt call, so it needs no synchronisation of its own.
	uint8_t g_injectPkt[kMaxPkt];
	uint32_t g_cbInjectPkt = 0;

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
		hdr.set_eresult(code ? ERESULT_OK : ERESULT_ACCESS_DENIED);

		CContentServerDirectory_GetManifestRequestCode_Response resp;
		if (code) resp.set_manifest_request_code(code);

		const size_t hl = hdr.ByteSizeLong();
		const size_t bl = resp.ByteSizeLong();
		if (hl > kMaxHdr || bl > kMaxBody) return false;

		MsgHdr* mh = reinterpret_cast<MsgHdr*>(g_injectPkt);
		mh->eMsg = static_cast<uint32_t>(EMSG_SERVICE_METHOD_RESPONSE) | kMsgHdrProtoFlag;
		mh->headerLength = static_cast<uint32_t>(hl);
		if (!hdr.SerializeToArray(g_injectPkt + sizeof(MsgHdr), static_cast<int>(hl))) return false;
		if (!resp.SerializeToArray(g_injectPkt + sizeof(MsgHdr) + hl, static_cast<int>(bl))) return false;
		g_cbInjectPkt = static_cast<uint32_t>(sizeof(MsgHdr) + hl + bl);
		return true;
	}
}

namespace RequestCode
{
	bool onSendFrame(const uint8_t* pubData, uint32_t cubData)
	{
		uint16_t eMsg = 0;
		const uint8_t *pHdr = nullptr, *pBody = nullptr;
		uint32_t cbHdr = 0, cbBody = 0;
		if (!netpacket::unpackRaw(pubData, cubData, eMsg, pHdr, cbHdr, pBody, cbBody)) return false;
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
			g_pLog->info("RequestCode: injected fabricated response code=%llu for jobid=%llu\n",
			             static_cast<unsigned long long>(code), static_cast<unsigned long long>(ready.jobId));
		else
			g_pLog->warn("RequestCode: no code for jobid=%llu, fabricated denial\n",
			             static_cast<unsigned long long>(ready.jobId));

		outData = g_injectPkt;
		outSize = g_cbInjectPkt;
		return true;
	}
}
