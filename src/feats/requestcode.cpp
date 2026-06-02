#include "requestcode.hpp"

#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/EResult.hpp"
#include "../sdk/protobufs/slssteam_servicemethods.pb.h"

#include "../lua/LuaLoader.hpp"

#include "../globals.hpp"
#include "../log.hpp"

#include <chrono>
#include <exception>
#include <future>
#include <mutex>
#include <unordered_map>

// Manifest request-code interception (mirrors OST Hooks_NetPacket_Manifest).
//
// Steam asks the CM for a per-manifest "request code" before it can download
// a depot manifest. For depots we unlocked the server returns a failure, so
// we fetch the code ourselves (Lua provider / built-in HTTP) and splice it in.
//
// Flow:
//   - OUTGOING (sendMsg): on ServiceMethodCallFromClient (151) whose header
//     target_job_name == "ContentServerDirectory.GetManifestRequestCode#1",
//     parse {app_id, depot_id, manifest_id} and launch an async fetch. The
//     future is stored keyed by the header's jobid_source. The request is left
//     untouched so it still reaches the server.
//   - INCOMING (recvMsg): on ServiceMethodResponse (147), look up the pending
//     entry by the header's jobid_target. If found, wait (bounded) for the
//     fetch; on success set the body's manifest_request_code and force the
//     header eresult to OK (the ServiceMethod result lives in the header).
namespace RequestCode
{
	namespace
	{
		// ServiceMethod target name we intercept.
		constexpr char TARGET_JOB_NAME[] = "ContentServerDirectory.GetManifestRequestCode#1";

		// Upper bound on how long the recv handler blocks the incoming-message
		// dispatch thread waiting for the fetch. Matches OST's 12s. The curl
		// total timeout (10s) sits just under this so a slow-but-valid fetch
		// still lands, while keeping the dispatch-thread stall short enough not
		// to risk the CM heartbeat / connection (the old 35s could).
		constexpr auto MAX_WAIT = std::chrono::seconds(12);

		// jobid_source (from the outgoing call) -> pending fetch future.
		// Touched from the send hook and the recv hook only (the async lambda
		// returns a value and never touches this map); guard every access with g_mutex.
		std::unordered_map<uint64_t, std::shared_future<uint64_t>> g_pending;
		std::mutex g_mutex;
	}

	void sendMsg(CProtoBufMsgBase* msg)
	{
		if (msg->type != EMSG_SERVICE_METHOD_CALL_FROM_CLIENT || !msg->header)
		{
			return;
		}

		if (!msg->header->has_target_job_name() || msg->header->target_job_name() != TARGET_JOB_NAME)
		{
			return;
		}

		const auto body = msg->getBody<CContentServerDirectory_GetManifestRequestCode_Request>();
		if (!body->has_app_id() || !body->has_depot_id() || !body->has_manifest_id())
		{
			return;
		}

		// jobid_source is always set on a real ServiceMethodCallFromClient, but
		// guard against a malformed packet whose unset field would default to
		// k_GIDNil and later spuriously match an unset jobid_target on recv.
		if (!msg->header->has_jobid_source())
		{
			return;
		}

		const uint64_t jobId = msg->header->jobid_source();
		const uint32_t appId = body->app_id();
		const uint32_t depotId = body->depot_id();
		const uint64_t gid = body->manifest_id();

		g_pLog->debug
		(
			"RequestCode: fetching code for app=%u depot=%u gid=%llu (jobid=%llu)\n",
			appId, depotId, static_cast<unsigned long long>(gid), static_cast<unsigned long long>(jobId)
		);

		// Launch the fetch off-thread so the original request is not blocked.
		// The lambda swallows every exception (returning 0 on failure) so nothing
		// can propagate out of future.get() into the Steam message hook, and the
		// std::async call itself is guarded against thread-creation failure — an
		// exception escaping either site would unwind through a C trampoline and
		// std::terminate the client.
		std::shared_future<uint64_t> future;
		try
		{
			future = std::async
			(
				std::launch::async,
				[appId, depotId, gid]() -> uint64_t
				{
					try
					{
						uint64_t code = 0;
						if (LuaLoader::fetchManifestCode(appId, depotId, gid, code))
						{
							return code;
						}
					}
					catch (const std::exception& e)
					{
						g_pLog->warn("RequestCode: fetch threw: %s\n", e.what());
					}
					catch (...)
					{
						g_pLog->warn("RequestCode: fetch threw an unknown exception\n");
					}
					return 0;
				}
			).share();
		}
		catch (const std::exception& e)
		{
			g_pLog->warn("RequestCode: failed to launch fetch for jobid=%llu: %s\n",
			             static_cast<unsigned long long>(jobId), e.what());
			return;
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		g_pending[jobId] = std::move(future);
	}

	void recvMsg(CProtoBufMsgBase* msg)
	{
		if (msg->type != EMSG_SERVICE_METHOD_RESPONSE || !msg->header)
		{
			return;
		}

		const uint64_t jobId = msg->header->jobid_target();

		std::shared_future<uint64_t> future;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const auto it = g_pending.find(jobId);
			if (it == g_pending.end())
			{
				return;
			}
			future = it->second;
			g_pending.erase(it); //Always clean up, even on timeout/failure
		}

		if (future.wait_for(MAX_WAIT) != std::future_status::ready)
		{
			g_pLog->warn("RequestCode: fetch timed out for jobid=%llu\n", static_cast<unsigned long long>(jobId));
			return;
		}

		uint64_t code = 0;
		try
		{
			code = future.get();
		}
		catch (const std::exception& e)
		{
			g_pLog->warn("RequestCode: fetch exception for jobid=%llu: %s\n",
			             static_cast<unsigned long long>(jobId), e.what());
			return;
		}
		if (!code)
		{
			//Fetch failed: leave the response untouched so the install fails gracefully.
			g_pLog->warn("RequestCode: no code for jobid=%llu, passing response through\n", static_cast<unsigned long long>(jobId));
			return;
		}

		//The ServiceMethod result lives in the header, not the body.
		msg->header->set_eresult(ERESULT_OK);

		const auto body = msg->getBody<CContentServerDirectory_GetManifestRequestCode_Response>();
		body->set_manifest_request_code(code);

		g_pLog->info
		(
			"RequestCode: injected code=%llu for jobid=%llu\n",
			static_cast<unsigned long long>(code), static_cast<unsigned long long>(jobId)
		);
	}
}
