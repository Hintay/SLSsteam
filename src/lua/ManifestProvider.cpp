#include "ManifestProvider.hpp"

#include "../curl.hpp"
#include "../log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ManifestProvider
{

// ── Response parsers ────────────────────────────────────────────────────────

// Parse a plain decimal uint64 from the entire body (wudrm format).
static bool parsePlainUint(std::string_view body, uint64_t& out)
{
    // Trim leading/trailing whitespace to tolerate trailing newlines.
    const char* first = body.data();
    const char* last  = body.data() + body.size();
    while (first < last && (*first == ' ' || *first == '\t' ||
                            *first == '\r' || *first == '\n'))
        ++first;
    while (last > first && (last[-1] == ' ' || last[-1] == '\t' ||
                            last[-1] == '\r' || last[-1] == '\n'))
        --last;
    if (first == last) return false;

    errno = 0;
    char* end = nullptr;
    unsigned long long v = strtoull(first, &end, 10);
    if (errno != 0 || end != last) return false;
    out = static_cast<uint64_t>(v);
    return true;
}

// Parse {"content":"<decimal_u64>"} JSON fragment (steam.run format).
// Minimal hand-rolled parse — no JSON library dependency.
static bool parseSteamRunJson(std::string_view body, uint64_t& out)
{
    // Find "content" key.
    size_t key = body.find("\"content\"");
    if (key == std::string_view::npos) return false;
    // Find the opening quote of the value.
    size_t q1 = body.find('"', key + 9);
    if (q1 == std::string_view::npos) return false;
    // Find the closing quote.
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string_view::npos) return false;
    return parsePlainUint(body.substr(q1 + 1, q2 - q1 - 1), out);
}

// ── Provider table ──────────────────────────────────────────────────────────

using Parser = bool (*)(std::string_view body, uint64_t& out);

struct Provider {
    const char* name;
    // URL template: contains exactly one "{gid}" placeholder.
    const char* urlTemplate;
    Parser      parse;
};

// Mirrors OST kProviders (wudrm default per task spec).
// opensteamtool removed — wudrm is the T6 default as specified.
static const Provider kProviders[] = {
    // Plain HTTP is intentional: matches the upstream OST provider URL.
    // The Steam chunk-hash check is the final integrity backstop; do NOT change to https
    // until the endpoint's https support has been verified.
    { "wudrm",     "http://gmrc.wudrm.com/manifest/{gid}",            parsePlainUint    },
    { "steamrun",  "https://manifest.steam.run/api/manifest/{gid}",   parseSteamRunJson },
};

static const Provider* g_active = &kProviders[0]; // default: wudrm

// ── Public API ──────────────────────────────────────────────────────────────

bool setProvider(const std::string& name)
{
    for (const auto& p : kProviders) {
        if (name == p.name) {
            g_active = &p;
            g_pLog->info("ManifestProvider: active provider set to '%s'\n", p.name);
            return true;
        }
    }
    g_pLog->warn("ManifestProvider: unknown provider '%s', keeping '%s'\n",
                 name.c_str(), g_active->name);
    return false;
}

const char* activeProviderName()
{
    return g_active->name;
}

// Build the request URL by replacing the single "{gid}" token in the template.
static std::string buildUrl(const char* tmpl, uint64_t gid)
{
    std::string url;
    url.reserve(std::strlen(tmpl) + 24);
    const char* p = tmpl;
    while (*p) {
        if (p[0] == '{' && p[1] == 'g' && p[2] == 'i' && p[3] == 'd' && p[4] == '}') {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(gid));
            url += buf;
            p += 5; // skip "{gid}"
        } else {
            url += *p++;
        }
    }
    return url;
}

bool fetchFromProvider(uint64_t gid, uint64_t& outCode)
{
    const Provider& p   = *g_active;
    const std::string   url = buildUrl(p.urlTemplate, gid);

    std::string body;
    long        status = 0;
    static const std::vector<std::pair<std::string, std::string>> noHeaders;
    static const std::string noBody;

    int rc = Curl::request("GET", url.c_str(), noHeaders, noBody, body, status);

    g_pLog->info("ManifestProvider: provider='%s' gid=%llu curlrc=%d status=%ld\n",
                 p.name, static_cast<unsigned long long>(gid), rc, status);

    if (rc != 0 || status != 200) return false;

    uint64_t code = 0;
    if (!p.parse(body, code)) {
        g_pLog->warn("ManifestProvider: failed to parse response body (len=%zu)\n", body.size());
        return false;
    }
    if (code == 0) {
        g_pLog->warn("ManifestProvider: provider returned code=0, rejecting\n");
        return false;
    }

    outCode = code;
    return true;
}

} // namespace ManifestProvider
