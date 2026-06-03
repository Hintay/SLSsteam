#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// In-memory accessors for Steam's CAppData (steamclient.so, the value stored in a
// CAppInfoCache entry). We do NOT model the full struct; only the offsets proven by
// static RE on the current Linux build are exposed — see
// docs/superpowers/notes/ost-linux-appinfo-cache-hooks.md. Re-derive on a new build.
//
//   +0x04  AppId_t nAppID
//   +0x10  bSkipFlag (byte)         — Steam's PICS "known-unknown / skip" marker
//   +0x1c  appinfo SHA1 (20 bytes)  — all-zero == appinfo not yet resolved
namespace AppData {
    static constexpr size_t kSkipFlagOff = 0x10; // byte
    static constexpr size_t kSha1Off     = 0x1c;
    static constexpr size_t kSha1Len     = 20;

    // True when the appinfo SHA1 is still all-zero (Steam never PICS-resolved it).
    inline bool hasEmptyAppInfoSha(void* pData) {
        static const uint8_t kZeroSha1[kSha1Len] = {0};
        return std::memcmp(reinterpret_cast<char*>(pData) + kSha1Off, kZeroSha1, kSha1Len) == 0;
    }

    inline bool skipFlag(void* pData) {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(pData) + kSkipFlagOff) != 0;
    }

    // Mark this entry as a PICS known-unknown so license/readiness checks stop
    // waiting for its appinfo to resolve (readiness test reads this byte first).
    inline void setSkipFlag(void* pData) {
        *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(pData) + kSkipFlagOff) = 1;
    }

    // Mirrors OST CAppData::IsUnresolvedAppInfo(): empty appinfo SHA1 AND not yet
    // marked skip. Such an entry, for an injected (fake-owned) app, blocks
    // ProcessPendingLicenseUpdates — setting skip unblocks it (Task 8 crash fix).
    inline bool isUnresolvedAppInfo(void* pData) {
        return hasEmptyAppInfoSha(pData) && !skipFlag(pData);
    }
}
