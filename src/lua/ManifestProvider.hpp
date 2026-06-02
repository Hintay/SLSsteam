#pragma once

#include <cstdint>
#include <string>

// Built-in HTTP providers for manifest request-code lookup.
// Mirrors OST ManifestClient.cpp / kProviders model.
// Default provider: "wudrm".
//
// YAML wiring (selecting provider by name from config) is handled in T9;
// for now consumers call setProvider() directly or rely on the default.
namespace ManifestProvider
{
    // Switch the active provider by name ("wudrm", "steamrun").
    // Returns false if the name is not recognised (active provider unchanged).
    bool setProvider(const std::string& name);

    // Name of the currently active provider (null-terminated, static lifetime).
    const char* activeProviderName();

    // Fetch the manifest request code for the given GID from the active provider.
    // Builds the provider URL, GETs it via Curl::request, parses the body as a
    // non-zero uint64.  Returns true and writes outCode on success.
    bool fetchFromProvider(uint64_t gid, uint64_t& outCode);

} // namespace ManifestProvider
