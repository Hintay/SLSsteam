#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Built-in HTTP providers for manifest request-code lookup.
// Mirrors OST ManifestClient.cpp / kProviders model. Built-ins: opensteamtool / wudrm / steamrun.
//
// The chain is selected from yaml `Manifest.Providers` in config.cpp:loadSettings(), which accepts
// either a single scalar (`Providers: wudrm`) or a list (`Providers: [opensteamtool, wudrm, ...]`).
// A single entry is a strict single provider (no fallback); if the key is absent the default chain
// opensteamtool -> wudrm -> steamrun is restored.
namespace ManifestProvider
{
	// Restore the built-in default chain: opensteamtool -> wudrm -> steamrun.
	void resetProviders();

    // Set the EXACT ordered chain to try. Unknown names are skipped (warned); returns false if no
    // valid entries (chain unchanged). A single-element list is a strict single provider.
    bool setProviders(const std::vector<std::string>& names);

    // Name of the first provider in the chain (null-terminated, static lifetime).
    const char* activeProviderName();

	// Human-readable provider chain, e.g. "opensteamtool -> wudrm -> steamrun".
	std::string activeProviderChainSummary();

    // Fetch the manifest request code for the given GID, trying each provider in the chain in order
    // until one returns a valid (non-zero) code. Returns true and writes outCode on success.
    bool fetchFromProvider(uint64_t gid, uint64_t& outCode);

} // namespace ManifestProvider
