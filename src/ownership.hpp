#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace Ownership {

	// Aggregated ownership query API. Lua and yaml can both feed the final
	// controlled app set, while this layer tracks genuine ownership discovered
	// from Steam's original ownership path.
	bool isControlledApp(uint32_t appId);
	bool isYamlAdditionalApp(uint32_t appId);
	bool isYamlOnlyAdditionalApp(uint32_t appId);
	bool shouldSpoofOwnership(uint32_t appId);

	bool isGenuinelyOwned(uint32_t appId);
	void markGenuinelyOwned(uint32_t appId);
	void unmarkGenuinelyOwned(uint32_t appId);
	void setGenuinelyOwned(uint32_t appId, bool owned);

	std::vector<uint32_t> getControlledAppIds();
	// One snapshot of the controlled-app set for batched membership tests, so a
	// loop does not pay a full set copy per id (MTVariable::get copies on each call).
	std::unordered_set<uint32_t> getControlledAppIdSet();
	uint32_t getPurchaseTime(uint32_t appId);

} // namespace Ownership
