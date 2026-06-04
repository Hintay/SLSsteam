#include "ownership.hpp"

#include "config.hpp"
#include "log.hpp"

#include <mutex>
#include <unordered_set>

namespace {
	std::unordered_set<uint32_t> g_realOwnedAppIds;
	std::mutex g_realOwnedMtx;
}

namespace Ownership {

bool isGenuinelyOwned(uint32_t appId)
{
	std::lock_guard<std::mutex> lock(g_realOwnedMtx);
	return g_realOwnedAppIds.contains(appId);
}

void setGenuinelyOwned(uint32_t appId, bool owned)
{
	std::lock_guard<std::mutex> lock(g_realOwnedMtx);
	if (owned)
	{
		if (g_realOwnedAppIds.emplace(appId).second)
		{
			g_pLog->once("Marking %u as genuinely owned\n", appId);
		}
		return;
	}

	if (g_realOwnedAppIds.erase(appId))
	{
		g_pLog->once("Unmarking %u as genuinely owned\n", appId);
	}
}

void markGenuinelyOwned(uint32_t appId)
{
	setGenuinelyOwned(appId, true);
}

void unmarkGenuinelyOwned(uint32_t appId)
{
	setGenuinelyOwned(appId, false);
}

bool isControlledApp(uint32_t appId)
{
	return g_config.isAddedAppId(appId);
}

bool shouldSpoofOwnership(uint32_t appId)
{
	return isControlledApp(appId) && !isGenuinelyOwned(appId);
}

std::vector<uint32_t> getControlledAppIds()
{
	const auto ids = g_config.addedAppIds.get();
	return {ids.begin(), ids.end()};
}

uint32_t getPurchaseTime(uint32_t appId)
{
	const auto times = g_config.subscriptionTimestamps.get();
	auto it = times.find(appId);
	return it != times.end() ? it->second : 0;
}

} // namespace Ownership
