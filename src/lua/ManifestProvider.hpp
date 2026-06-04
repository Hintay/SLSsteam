#pragma once

#include <cstdint>
#include <string>

// Built-in HTTP providers for manifest request-code lookup.
// Mirrors OST ManifestClient.cpp / kProviders model.
// Default provider: "opensteamtool".
//
// The provider is selected from yaml `Manifest.Provider` in config.cpp:loadSettings();
// call setProvider() directly to override at runtime.
namespace ManifestProvider
{
    // Switch the active provider by name ("opensteamtool", "wudrm", "steamrun").
    // Returns false if the name is not recognised (active provider unchanged).
    bool setProvider(const std::string& name);

    // Name of the currently active provider (null-terminated, static lifetime).
    const char* activeProviderName();

    // Fetch the manifest request code for the given GID from the active provider.
    // Builds the provider URL, GETs it via Curl::request, parses the body as a
    // non-zero uint64.  Returns true and writes outCode on success.
    bool fetchFromProvider(uint64_t gid, uint64_t& outCode);

} // namespace ManifestProvider
