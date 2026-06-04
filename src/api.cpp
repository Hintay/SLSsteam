#include "api.hpp"

#include "sdk/IClientAppManager.hpp"
#include "sdk/IClientApps.hpp"

#include "config.hpp"
#include "filewatcher.hpp"
#include "ownership.hpp"
#include "utils.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <ios>
#include <mutex>


namespace SLSAPI
{
	enum class InstallRequestState
	{
		PendingInstall,
		WaitingForAppInfo,
	};

	struct InstallRequest
	{
		uint32_t appId;
		int32_t libraryIndex;
		InstallRequestState state;
		uint32_t installAttempts;
		uint32_t appInfoAttempts;
		uint64_t nextCheckAtMs;
		bool requestedAppInfo;
		bool loggedWaitingForAppInfo;
	};

	constexpr size_t kMaxInstallAttemptsPerFrame = 1;
	constexpr uint32_t kMaxInstallAttempts = 3;
	constexpr uint32_t kMaxAppInfoAttempts = 20;
	constexpr uint64_t kAppInfoPollIntervalMs = 500;

	const char* path = "/tmp/SLSsteam.API";
	std::fstream fstream;
	CFileWatcher* watcher;

	std::atomic_bool hasPendingInstalls = false;
	std::atomic<uint64_t> nextPumpAtMs = 0;
	std::mutex installQueueMtx;
	std::deque<InstallRequest> installQueue;

	uint64_t nowMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	void storeRequeued(std::deque<InstallRequest>& requeued)
	{
		if (requeued.empty())
		{
			return;
		}

		uint64_t nextWake = 0;
		for (const InstallRequest& request : requeued)
		{
			if (request.state == InstallRequestState::PendingInstall || request.nextCheckAtMs == 0)
			{
				nextWake = 0;
				break;
			}

			if (nextWake == 0 || request.nextCheckAtMs < nextWake)
			{
				nextWake = request.nextCheckAtMs;
			}
		}

		bool hasNewRequests = false;
		{
			std::lock_guard<std::mutex> lock(installQueueMtx);
			hasNewRequests = !installQueue.empty();
			installQueue.insert(installQueue.end(), requeued.begin(), requeued.end());
			if (hasNewRequests)
			{
				nextWake = 0;
			}

			nextPumpAtMs.store(nextWake, std::memory_order_release);
			hasPendingInstalls.store(true, std::memory_order_release);
		}
	}

	void requeue(std::deque<InstallRequest>& requeued, InstallRequest& request)
	{
		requeued.push_back(request);
	}

	bool appInfoCommonReady(uint32_t appId, bool logMissing)
	{
		if (!g_pClientApps)
		{
			if (logMissing)
			{
				g_pLog->info("API g_pClientApps is nullptr; delaying appinfo check for %u.\n", appId);
			}
			return false;
		}

		char sectionData[1] = {};
		const int32_t commonSize = g_pClientApps->getAppDataSection(appId, APPINFOSECTION_COMMON, sectionData, sizeof(sectionData));
		if (commonSize < 0 && logMissing)
		{
			g_pLog->info("API Waiting for appinfo %u common=%i\n", appId, commonSize);
		}

		return commonSize >= 0;
	}
}

bool SLSAPI::isEnabled()
{
	return g_config.api.get() && fstream.is_open();
}

void SLSAPI::onFileChange(const std::string& changedPath, uint32_t mask)
{
	(void)changedPath;
	(void)mask;
	//Hot reload support :)
	if (!isEnabled())
	{
		return;
	}

	//Shitty way to reopen the stream. We have to do this, otherwise the fstream gets invalidated when running echo >
	fstream.close();
	fstream.open(SLSAPI::path);

	char cmd[128];
	fstream.getline(cmd, sizeof(cmd));

	g_pLog->debug("API Running %s\n", cmd);

	auto split = Utils::strsplit(cmd, "|");
	if (split.size() > 2 && strcmp(split[0].c_str(), "install") == 0)
	{
		try
		{
			uint32_t appId = std::strtoul(split[1].c_str(), nullptr, 10);
			int32_t libraryIndex = std::strtol(split[2].c_str(), nullptr, 10);

			{
				std::lock_guard<std::mutex> lock(installQueueMtx);
				installQueue.push_back({appId, libraryIndex, InstallRequestState::PendingInstall, 0, 0, 0, false, false});
			}
			nextPumpAtMs.store(0, std::memory_order_release);
			hasPendingInstalls.store(true, std::memory_order_release);

			g_pLog->info("API Queued install %s to %s\n", split[1].c_str(), split[2].c_str());
		}
		catch(...)
		{
			g_pLog->info("API Failed to parse %s or %s!\n", split[1].c_str(), split[2].c_str());
		}
	}
}

void SLSAPI::runPendingInstallsOnAppManagerFrame()
{
	if (!hasPendingInstalls.load(std::memory_order_acquire))
	{
		return;
	}

	const uint64_t now = nowMs();
	const uint64_t nextPump = nextPumpAtMs.load(std::memory_order_acquire);
	if (nextPump != 0 && now < nextPump)
	{
		return;
	}

	if (!hasPendingInstalls.exchange(false, std::memory_order_acq_rel))
	{
		return;
	}
	nextPumpAtMs.store(0, std::memory_order_release);

	std::deque<InstallRequest> requests;
	{
		std::lock_guard<std::mutex> lock(installQueueMtx);
		requests.swap(installQueue);
	}

	std::deque<InstallRequest> requeued;
	size_t installAttemptsThisFrame = 0;

	while (!requests.empty())
	{
		InstallRequest request = requests.front();
		requests.pop_front();

		if (request.state == InstallRequestState::WaitingForAppInfo)
		{
			if (now < request.nextCheckAtMs)
			{
				requeue(requeued, request);
				continue;
			}

			if (!g_pClientApps)
			{
				if (!request.loggedWaitingForAppInfo)
				{
					g_pLog->info("API Waiting for g_pClientApps before appinfo check for %u.\n", request.appId);
					request.loggedWaitingForAppInfo = true;
				}
				request.nextCheckAtMs = now + kAppInfoPollIntervalMs;
				requeue(requeued, request);
				continue;
			}

			if (!request.requestedAppInfo)
			{
				const bool requested = g_pClientApps->requestAppInfoUpdate(request.appId);
				g_pLog->info("API RequestAppInfoUpdate(%u) -> %i after delayed appinfo request\n", request.appId, requested);
				if (!requested)
				{
					g_pLog->info("API RequestAppInfoUpdate(%u) failed; dropping queued install.\n", request.appId);
					continue;
				}

				request.requestedAppInfo = true;
				request.loggedWaitingForAppInfo = false;
				request.nextCheckAtMs = now + kAppInfoPollIntervalMs;
				requeue(requeued, request);
				continue;
			}

			const bool ready = appInfoCommonReady(request.appId, !request.loggedWaitingForAppInfo);
			request.loggedWaitingForAppInfo = true;
			if (ready)
			{
				request.state = InstallRequestState::PendingInstall;
				request.nextCheckAtMs = 0;
				requeue(requeued, request);
				continue;
			}

			request.appInfoAttempts++;
			if (request.appInfoAttempts >= kMaxAppInfoAttempts)
			{
				g_pLog->info("API appinfo wait timed out for %u after %u checks.\n", request.appId, request.appInfoAttempts);
				continue;
			}

			request.nextCheckAtMs = now + kAppInfoPollIntervalMs;
			requeue(requeued, request);
			continue;
		}

		if (Ownership::isYamlAdditionalApp(request.appId))
		{
			g_pLog->info("API InstallApp(%u, %i) blocked for YAML AdditionalApps entry.\n", request.appId, request.libraryIndex);
			continue;
		}

		if (!g_pClientAppManager)
		{
			g_pLog->info("API g_pClientAppManager is nullptr; delaying queued install for %u.\n", request.appId);
			requeue(requeued, request);
			continue;
		}

		if (installAttemptsThisFrame >= kMaxInstallAttemptsPerFrame)
		{
			requeue(requeued, request);
			continue;
		}

		request.installAttempts++;
		installAttemptsThisFrame++;
		const EAppUpdateError installResult = g_pClientAppManager->installApp(request.appId, request.libraryIndex);
		g_pLog->info("API InstallApp(%u, %i) -> %i\n", request.appId, request.libraryIndex, installResult);

		if (installResult == APP_UPDATE_ERROR_NONE)
		{
			continue;
		}

		if (installResult == APP_UPDATE_ERROR_MISSING_CONFIG)
		{
			if (request.installAttempts >= kMaxInstallAttempts)
			{
				g_pLog->info("API InstallApp(%u, %i) still missing configuration after %u attempts.\n", request.appId, request.libraryIndex, request.installAttempts);
				continue;
			}

			if (!g_pClientApps)
			{
				g_pLog->info("API g_pClientApps is nullptr; delaying appinfo request for %u.\n", request.appId);
				request.state = InstallRequestState::WaitingForAppInfo;
				request.nextCheckAtMs = now + kAppInfoPollIntervalMs;
				requeue(requeued, request);
				continue;
			}

			const bool requested = g_pClientApps->requestAppInfoUpdate(request.appId);
			g_pLog->info("API RequestAppInfoUpdate(%u) -> %i after missing configuration\n", request.appId, requested);
			if (!requested)
			{
				g_pLog->info("API RequestAppInfoUpdate(%u) failed; dropping queued install.\n", request.appId);
				continue;
			}

			request.requestedAppInfo = true;
			request.loggedWaitingForAppInfo = false;
			request.state = InstallRequestState::WaitingForAppInfo;
			request.nextCheckAtMs = now + kAppInfoPollIntervalMs;
			requeue(requeued, request);
			continue;
		}

		g_pLog->info("API InstallApp(%u, %i) failed with terminal result %i.\n", request.appId, request.libraryIndex, installResult);
	}

	storeRequeued(requeued);
}

void SLSAPI::init()
{
	fstream = std::fstream(path, std::ios::in | std::ios::out);

	watcher = new CFileWatcher(onFileChange);
	watcher->addFile(path);
	watcher->start();

	g_pLog->debug("SLSsteam API initialized!\n");
}
