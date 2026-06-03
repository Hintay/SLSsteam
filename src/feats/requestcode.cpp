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
#include <exception>
#include <future>
#include <mutex>
#include <unordered_map>

// Manifest request-code interception at the raw packet layer (OST-style; see
// docs/superpowers/notes/pattern-rederivation-runbook.md §2.B.1).
//
// SLSsteam previously dispatched this from CProtoBufMsgBase::Send / InitFromPacket,
// but the live build proved the GetManifestRequestCode ServiceMethod never
// traverses that path: the outgoing EMsg 151 is sent by a ServiceMethod sender
// straight to the WebSocket/CM transport, so CProtoBufMsgBase::Send sees only
// classic CMsg sends. The request reached the server unintercepted -> 'Access
// Denied' -> download cancelled. The interception now lives on the raw hooks,
// matching OST's BBuildAndAsyncSendFrame / RecvPkt.

namespace
{
	constexpr char TARGET_JOB_NAME[] = "ContentServerDirectory.GetManifestRequestCode#1";
	constexpr auto MAX_WAIT = std::chrono::seconds(12);

	constexpr uint32_t kMaxHdr   = 1024;
	constexpr uint32_t kMaxBody  = 8092;
	constexpr int      kPoolSize = 8;

	// jobid_source (outgoing) -> pending fetch future. Touched from the send hook
	// and the recv hook only; guard every access with g_mutex.
	std::unordered_map<uint64_t, std::shared_future<uint64_t>> g_pending;
	std::mutex g_mutex;
	// Lock-free fast-path gate for the recv hook (runs on every incoming packet):
	// mirrors g_pending.size(); only written under g_mutex.
	std::atomic<size_t> g_pendingCount{0};

	// Ring-buffer pool for rewritten incoming packets — the rewritten bytes must
	// outlive the oRecvPkt call that consumes them. RecvPkt is single-threaded per
	// CM connection (same assumption OST relies on).
	uint8_t g_recvPool[kPoolSize][sizeof(MsgHdr) + kMaxHdr + kMaxBody];
	int     g_recvPoolIdx = 0;
}

namespace RequestCode
{
	void onSendFrame(const uint8_t* pubData, uint32_t cubData)
	{
		uint16_t eMsg = 0;
		const uint8_t *pHdr = nullptr, *pBody = nullptr;
		uint32_t cbHdr = 0, cbBody = 0;
		if (!netpacket::unpackRaw(pubData, cubData, eMsg, pHdr, cbHdr, pBody, cbBody)) return;
		if (eMsg != EMSG_SERVICE_METHOD_CALL_FROM_CLIENT) return;

		CMsgProtoBufHeader hdr;
		if (!hdr.ParseFromArray(pHdr, static_cast<int>(cbHdr))) return;
		if (!hdr.has_target_job_name() || hdr.target_job_name() != TARGET_JOB_NAME) return;
		if (!hdr.has_jobid_source()) return;

		CContentServerDirectory_GetManifestRequestCode_Request req;
		if (!req.ParseFromArray(pBody, static_cast<int>(cbBody))) return;
		if (!req.has_depot_id() || !req.has_manifest_id()) return;

		const uint64_t jobId   = hdr.jobid_source();
		const uint32_t depotId = req.depot_id();
		const uint64_t gid     = req.manifest_id();
		const uint32_t appId   = req.has_app_id() ? req.app_id() : 0;

		g_pLog->debug("RequestCode: fetching code for app=%u depot=%u gid=%llu (jobid=%llu)\n",
		              appId, depotId, static_cast<unsigned long long>(gid),
		              static_cast<unsigned long long>(jobId));

		// Launch the fetch off-thread. The lambda swallows every exception
		// (returning 0) so nothing unwinds through the C trampoline; the std::async
		// call itself is guarded against thread-creation failure for the same reason.
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
			g_pLog->warn("RequestCode: failed to launch fetch for jobid=%llu: %s\n",
			             static_cast<unsigned long long>(jobId), e.what());
			return;
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		g_pending[jobId] = std::move(future);
		g_pendingCount.store(g_pending.size(), std::memory_order_release);
	}

	void onRecvPacket(CNetPacket* pkt)
	{
		if (!pkt) return;
		if (g_pendingCount.load(std::memory_order_acquire) == 0) return; // fast path

		uint16_t eMsg = 0;
		const uint8_t *pHdr = nullptr, *pBody = nullptr;
		uint32_t cbHdr = 0, cbBody = 0;
		if (!netpacket::unpackRaw(pkt->m_pubData, pkt->m_cubData, eMsg, pHdr, cbHdr, pBody, cbBody)) return;
		if (eMsg != EMSG_SERVICE_METHOD_RESPONSE) return;

		CMsgProtoBufHeader hdr;
		if (!hdr.ParseFromArray(pHdr, static_cast<int>(cbHdr)) || !hdr.has_jobid_target()) return;

		const uint64_t jobId = hdr.jobid_target();

		std::shared_future<uint64_t> future;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const auto it = g_pending.find(jobId);
			if (it == g_pending.end()) return;   // not one of ours
			future = it->second;
			g_pending.erase(it);                  // always clean up
			g_pendingCount.store(g_pending.size(), std::memory_order_release);
		}

		if (future.wait_for(MAX_WAIT) != std::future_status::ready)
		{
			g_pLog->warn("RequestCode: fetch timed out for jobid=%llu\n",
			             static_cast<unsigned long long>(jobId));
			return;
		}

		uint64_t code = 0;
		try { code = future.get(); }
		catch (const std::exception& e)
		{
			g_pLog->warn("RequestCode: fetch exception for jobid=%llu: %s\n",
			             static_cast<unsigned long long>(jobId), e.what());
			return;
		}
		if (!code)
		{
			g_pLog->warn("RequestCode: no code for jobid=%llu, passing response through\n",
			             static_cast<unsigned long long>(jobId));
			return;
		}

		// Header: force eresult OK (the ServiceMethod result lives in the header).
		hdr.set_eresult(ERESULT_OK);
		const size_t newHdrLen = hdr.ByteSizeLong();

		// Body: set manifest_request_code.
		CContentServerDirectory_GetManifestRequestCode_Response resp;
		resp.set_manifest_request_code(code);
		const size_t newBodyLen = resp.ByteSizeLong();

		if (newHdrLen > kMaxHdr || newBodyLen > kMaxBody) return;

		// Rebuild [MsgHdr][newHdr][newBody] into a pooled buffer; keep the original
		// eMsg (proto flag intact), update headerLength to the new header size.
		uint8_t* buf = g_recvPool[g_recvPoolIdx];
		MsgHdr* outHdr = reinterpret_cast<MsgHdr*>(buf);
		outHdr->eMsg         = reinterpret_cast<const MsgHdr*>(pkt->m_pubData)->eMsg;
		outHdr->headerLength = static_cast<uint32_t>(newHdrLen);
		if (!hdr.SerializeToArray(buf + sizeof(MsgHdr), static_cast<int>(newHdrLen))) return;
		if (!resp.SerializeToArray(buf + sizeof(MsgHdr) + newHdrLen, static_cast<int>(newBodyLen))) return;

		pkt->m_pubData = buf;
		pkt->m_cubData = static_cast<uint32_t>(sizeof(MsgHdr) + newHdrLen + newBodyLen);
		g_recvPoolIdx = (g_recvPoolIdx + 1) % kPoolSize;

		g_pLog->info("RequestCode: injected code=%llu for jobid=%llu\n",
		             static_cast<unsigned long long>(code), static_cast<unsigned long long>(jobId));
	}
}
