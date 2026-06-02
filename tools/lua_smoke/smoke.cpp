// Smoke test for LuaLoader T1+T2+T6.
// Self-contained: no SLSsteam internal headers, no libmem, no yaml-cpp, no libcurl.
// Inlines the same Lua VM setup logic and real T2/T6 binding implementations
// as LuaLoader.cpp to verify:
//   T1: The Lua VM can be created.
//   T1: The case-insensitive __index metamethod resolves mixed-case names.
//   T1: All 14 bindings are reachable without "attempt to call a nil value".
//   T2: addappid / addtoken / setmanifestid populate the in-process tables.
//   T2: addappid with a 64-hex-char key stores 32 decoded bytes.
//   T6: Provider URL {gid} substitution produces correct strings.
//   T6: parsePlainUint / parseSteamRunJson parse and validate correctly.
//   T6: fetch_manifest_code_ex callback selection (branch 1) is invoked first
//       when the ref is set; returns the correct code.
//   T6: fetch_manifest_code callback (branch 2) is used when _ex is not set.
//   T6: Lua stack is balanced across pcall / result-read.
//   T6: http_get accepts an optional headers table arg (smoke stub).
//   T6: http_post is now a real binding (validates 2 required args in smoke).

#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

// ── Minimal in-process state tables (mirrors LuaLoader's declarations) ────

namespace SmokeLoader {

// Depot decryption keys: depotId → 32 bytes parsed from 64 hex chars.
static std::unordered_map<uint32_t, std::vector<uint8_t>> g_depotKeys;
// Manifest GID+size overrides.
struct ManifestOverride { uint64_t gid; uint64_t size; };
static std::unordered_map<uint32_t, ManifestOverride> g_manifestOverrides;
// Owned app/depot IDs (plain set — union target).
static std::unordered_set<uint32_t> g_ownedAppIds;
// App access tokens.
static std::unordered_map<uint32_t, uint64_t> g_appTokens;

static lua_State* g_lua = nullptr;
static std::unordered_map<std::string, lua_CFunction> g_func_registry;
static bool g_had_error = false;

// Registry refs for captured Lua callbacks (mirrors LuaLoader internal state).
static int g_fetchCodeRef   = LUA_NOREF;
static int g_fetchCodeExRef = LUA_NOREF;

// ── Hex helpers ───────────────────────────────────────────────────────────

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parseHex64(const char* hex, std::vector<uint8_t>& out) {
    if (std::strlen(hex) != 64) return false;
    std::vector<uint8_t> result;
    result.reserve(32);
    for (int i = 0; i < 64; i += 2) {
        int hi = hexNibble(hex[i]);
        int lo = hexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        result.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    out = std::move(result);
    return true;
}

static bool parseU64Decimal(const char* str, uint64_t& out) {
    if (!str || *str == '\0') return false;
    errno = 0;
    char* end = nullptr;
    unsigned long long v = strtoull(str, &end, 10);
    if (errno != 0 || end == str || *end != '\0') return false;
    out = static_cast<uint64_t>(v);
    return true;
}

// ── T6: Pure-logic helpers inlined from ManifestProvider + LuaLoader ─────

// Build a URL by replacing "{gid}" in tmpl with the decimal value of gid.
// (Mirrors ManifestProvider::buildUrl — inlined here to keep smoke self-contained.)
static std::string buildUrl(const char* tmpl, uint64_t gid) {
    std::string url;
    url.reserve(std::strlen(tmpl) + 24);
    const char* p = tmpl;
    while (*p) {
        if (p[0] == '{' && p[1] == 'g' && p[2] == 'i' && p[3] == 'd' && p[4] == '}') {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(gid));
            url += buf;
            p += 5;
        } else {
            url += *p++;
        }
    }
    return url;
}

// Parse plain decimal uint64 from body (wudrm format) — inlined from ManifestProvider.
static bool parsePlainUint(std::string_view body, uint64_t& out) {
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

// Parse {"content":"<decimal>"} JSON fragment — inlined from ManifestProvider.
static bool parseSteamRunJson(std::string_view body, uint64_t& out) {
    size_t key = body.find("\"content\"");
    if (key == std::string_view::npos) return false;
    size_t q1 = body.find('"', key + 9);
    if (q1 == std::string_view::npos) return false;
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string_view::npos) return false;
    return parsePlainUint(body.substr(q1 + 1, q2 - q1 - 1), out);
}

// Read the top Lua stack value as a non-zero uint64 — mirrors LuaLoader::readU64FromTop.
static bool readU64FromTop(lua_State* L, uint64_t& out) {
    if (lua_isnil(L, -1)) return false;
    if (lua_isinteger(L, -1)) {
        lua_Integer v = lua_tointeger(L, -1);
        if (v <= 0) return false;
        out = static_cast<uint64_t>(v);
        return true;
    }
    if (lua_isnumber(L, -1)) {
        double d = lua_tonumber(L, -1);
        if (d <= 0) return false;
        out = static_cast<uint64_t>(d);
        return (out != 0);
    }
    if (lua_isstring(L, -1)) {
        const char* s = lua_tostring(L, -1);
        errno = 0;
        char* end = nullptr;
        unsigned long long v = strtoull(s, &end, 10);
        if (errno != 0 || end == s || *end != '\0' || v == 0) return false;
        out = static_cast<uint64_t>(v);
        return true;
    }
    return false;
}

// ── Real binding implementations (mirrors LuaLoader.cpp T2 logic) ────────

static int impl_addappid(lua_State* L) {
    int argc = lua_gettop(L);
    if (argc == 0 || !lua_isinteger(L, 1)) {
        lua_pushstring(L, "addappid: arg1 must be integer");
        return lua_error(L);
    }
    lua_Integer raw = lua_tointeger(L, 1);
    if (raw < 0 || raw > static_cast<lua_Integer>(UINT32_MAX)) {
        lua_pushstring(L, "addappid: id out of range");
        return lua_error(L);
    }
    uint32_t id = static_cast<uint32_t>(raw);
    g_ownedAppIds.insert(id);

    if (argc >= 3 && lua_isstring(L, 3)) {
        const char* hexStr = lua_tostring(L, 3);
        std::vector<uint8_t> keyBytes;
        if (parseHex64(hexStr, keyBytes)) {
            g_depotKeys[id] = std::move(keyBytes);
        }
    }
    return 0;
}

static int impl_addtoken(lua_State* L) {
    int argc = lua_gettop(L);
    if (argc < 1 || !lua_isinteger(L, 1)) {
        lua_pushstring(L, "addtoken: arg1 must be integer");
        return lua_error(L);
    }
    lua_Integer raw = lua_tointeger(L, 1);
    if (raw < 0 || raw > static_cast<lua_Integer>(UINT32_MAX)) {
        lua_pushstring(L, "addtoken: out of range");
        return lua_error(L);
    }
    uint32_t appId = static_cast<uint32_t>(raw);
    if (argc < 2) return 0;
    if (!lua_isstring(L, 2)) {
        lua_pushstring(L, "addtoken: arg2 must be string");
        return lua_error(L);
    }
    uint64_t token = 0;
    if (!parseU64Decimal(lua_tostring(L, 2), token)) {
        lua_pushstring(L, "addtoken: invalid decimal string");
        return lua_error(L);
    }
    g_appTokens[appId] = token;
    return 0;
}

static int impl_setmanifestid(lua_State* L) {
    int argc = lua_gettop(L);
    if (argc < 2 || !lua_isinteger(L, 1) || !lua_isstring(L, 2)) {
        lua_pushstring(L, "setmanifestid: need integer depotId and string gid");
        return lua_error(L);
    }
    lua_Integer raw = lua_tointeger(L, 1);
    if (raw < 0 || raw > static_cast<lua_Integer>(UINT32_MAX)) {
        lua_pushstring(L, "setmanifestid: depotId out of range");
        return lua_error(L);
    }
    uint32_t depotId = static_cast<uint32_t>(raw);
    uint64_t gid = 0;
    if (!parseU64Decimal(lua_tostring(L, 2), gid)) {
        lua_pushstring(L, "setmanifestid: invalid gid string");
        return lua_error(L);
    }
    uint64_t size = 0;
    if (argc >= 3) {
        // Most-specific first: lua_isstring is true for numbers in Lua 5.4,
        // so integer/number branches must precede the string branch.
        if (lua_isinteger(L, 3)) {
            lua_Integer rs = lua_tointeger(L, 3);
            size = rs >= 0 ? static_cast<uint64_t>(rs) : 0;
        } else if (lua_isnumber(L, 3)) {
            double d = lua_tonumber(L, 3);
            size = d >= 0 ? static_cast<uint64_t>(d) : 0;
        } else if (lua_isstring(L, 3)) {
            parseU64Decimal(lua_tostring(L, 3), size);
        }
    }
    g_manifestOverrides[depotId] = ManifestOverride{gid, size};
    return 0;
}

// T6: real ref-capturing implementation (mirrors LuaLoader).
static int impl_fetch_manifest_code(lua_State* L) {
    if (lua_gettop(L) < 1 || !lua_isfunction(L, 1)) {
        lua_pushstring(L, "fetch_manifest_code: arg1 must be function");
        return lua_error(L);
    }
    if (g_fetchCodeRef != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, g_fetchCodeRef);
    lua_pushvalue(L, 1);
    g_fetchCodeRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

// T6: real ref-capturing implementation (mirrors LuaLoader).
static int impl_fetch_manifest_code_ex(lua_State* L) {
    if (lua_gettop(L) < 1 || !lua_isfunction(L, 1)) {
        lua_pushstring(L, "fetch_manifest_code_ex: arg1 must be function");
        return lua_error(L);
    }
    if (g_fetchCodeExRef != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, g_fetchCodeExRef);
    lua_pushvalue(L, 1);
    g_fetchCodeExRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

// T6: http_get(url [, headers_table]) → (body, status)
// Smoke: parse headers table arg to verify no crash, then return stub response.
static int impl_http_get(lua_State* L) {
    if (lua_gettop(L) < 1 || !lua_isstring(L, 1)) {
        lua_pushstring(L, "http_get: arg1 must be string");
        return lua_error(L);
    }
    // Consume optional headers table (arg 2) without crashing.
    // No real network call in smoke.
    lua_pushliteral(L, "");
    lua_pushinteger(L, 200); // HTTP 200 (changed from 0 to reflect real http_get contract)
    return 2;
}

// T6: http_post(url, body [, headers_table]) → (response|nil, status)
// Smoke: validate required args, return stub (nil, 200).
static int impl_http_post(lua_State* L) {
    int argc = lua_gettop(L);
    if (argc < 1 || !lua_isstring(L, 1)) {
        lua_pushstring(L, "http_post: arg1 must be URL string");
        return lua_error(L);
    }
    if (argc < 2 || !lua_isstring(L, 2)) {
        lua_pushstring(L, "http_post: arg2 must be body string");
        return lua_error(L);
    }
    // Smoke: don't make real network calls; return a stubbed success response.
    lua_pushliteral(L, "");
    lua_pushinteger(L, 200);
    return 2;
}

// Remaining stubs (T7/T8).
static int s_stub(lua_State*) { return 0; }

// ── Case-insensitive VM helpers ───────────────────────────────────────────

static int case_insensitive_index(lua_State* L) {
    const char* name = lua_tostring(L, 2);
    if (!name) { lua_pushnil(L); return 1; }
    std::string lower;
    for (const char* p = name; *p; ++p)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    auto it = g_func_registry.find(lower);
    if (it != g_func_registry.end()) { lua_pushcfunction(L, it->second); return 1; }
    lua_pushnil(L);
    return 1;
}

static void register_func(lua_State* L, const char* name, lua_CFunction fn) {
    g_func_registry[name] = fn;
    lua_pushcfunction(L, fn);
    lua_setglobal(L, name);
}

// ── Init and script runner ────────────────────────────────────────────────

static void init(const std::string& scanDir) {
    g_lua = luaL_newstate();
    if (!g_lua) { fprintf(stderr, "[smoke] luaL_newstate failed\n"); return; }
    luaL_openlibs(g_lua);

    // Install case-insensitive __index on _G's metatable (mirrors LuaLoader::init).
    lua_getglobal(g_lua, "_G");
    if (!lua_getmetatable(g_lua, -1)) lua_newtable(g_lua);
    lua_pushcfunction(g_lua, case_insensitive_index);
    lua_setfield(g_lua, -2, "__index");
    lua_setmetatable(g_lua, -2);
    lua_pop(g_lua, 1); // Pop _G.

    // MANUAL MIRROR of the registration list in LuaLoader::init() — must be kept
    // in sync by hand whenever bindings are added or renamed there.
    register_func(g_lua, "addappid",                    impl_addappid);
    register_func(g_lua, "addtoken",                    impl_addtoken);
    register_func(g_lua, "setmanifestid",               impl_setmanifestid);
    register_func(g_lua, "fetch_manifest_code",         impl_fetch_manifest_code);
    register_func(g_lua, "fetch_manifest_code_ex",      impl_fetch_manifest_code_ex);
    register_func(g_lua, "http_get",                    impl_http_get);
    register_func(g_lua, "http_post",                   impl_http_post);
    register_func(g_lua, "setappticket",                s_stub);
    register_func(g_lua, "seteticket",                  s_stub);
    register_func(g_lua, "setstat",                     s_stub);
    register_func(g_lua, "downloadapp",                 s_stub);
    register_func(g_lua, "addnonowneddepot",            s_stub);
    register_func(g_lua, "setnotifyondownloadcomplete", s_stub);
    register_func(g_lua, "setstpropertyforaccount",     s_stub);

    // Scan scanDir and execute every .lua file found.
    std::error_code ec;
    if (!std::filesystem::is_directory(scanDir, ec)) {
        fprintf(stderr, "[smoke] dir not found: %s\n", scanDir.c_str());
        g_had_error = true;
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(scanDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".lua") continue;

        const std::string path = entry.path().string();
        printf("[smoke] executing: %s\n", path.c_str());
        lua_settop(g_lua, 0);

        int rc = luaL_loadfile(g_lua, path.c_str());
        if (rc != LUA_OK) {
            fprintf(stderr, "[smoke] compile error in %s: %s\n",
                    path.c_str(), lua_tostring(g_lua, -1));
            lua_pop(g_lua, 1);
            g_had_error = true;
            continue;
        }
        if (lua_pcall(g_lua, 0, 0, 0) != LUA_OK) {
            fprintf(stderr, "[smoke] runtime error in %s: %s\n",
                    path.c_str(), lua_tostring(g_lua, -1));
            lua_pop(g_lua, 1);
            g_had_error = true;
        }
    }
}

// ── T6: fetchManifestCode logic (inlined mirror of LuaLoader::fetchManifestCode) ──
// Used by T6 C-level assertions; no network calls are made.
static bool fetchManifestCode(uint32_t appId, uint32_t depotId,
                               uint64_t gid, uint64_t& outCode)
{
    if (!g_lua) return false;
    const int topBefore = lua_gettop(g_lua);

    // Branch 1: fetch_manifest_code_ex(appId, depotId, gid)
    if (g_fetchCodeExRef != LUA_NOREF) {
        lua_rawgeti(g_lua, LUA_REGISTRYINDEX, g_fetchCodeExRef);
        lua_pushinteger(g_lua, static_cast<lua_Integer>(appId));
        lua_pushinteger(g_lua, static_cast<lua_Integer>(depotId));
        lua_pushinteger(g_lua, static_cast<lua_Integer>(gid));
        if (lua_pcall(g_lua, 3, 1, 0) != LUA_OK) {
            lua_settop(g_lua, topBefore);
        } else {
            uint64_t code = 0;
            bool ok = readU64FromTop(g_lua, code);
            lua_settop(g_lua, topBefore);
            if (ok) { outCode = code; return true; }
        }
    }

    // Branch 2: fetch_manifest_code(gid)
    if (g_fetchCodeRef != LUA_NOREF) {
        lua_rawgeti(g_lua, LUA_REGISTRYINDEX, g_fetchCodeRef);
        lua_pushinteger(g_lua, static_cast<lua_Integer>(gid));
        if (lua_pcall(g_lua, 1, 1, 0) != LUA_OK) {
            lua_settop(g_lua, topBefore);
        } else {
            uint64_t code = 0;
            bool ok = readU64FromTop(g_lua, code);
            lua_settop(g_lua, topBefore);
            if (ok) { outCode = code; return true; }
        }
    }

    // Branch 3: provider — smoke does not make real network calls; signal failure.
    return false;
}

} // namespace SmokeLoader

// ── Main ──────────────────────────────────────────────────────────────────

int main() {
    const char* tmp = std::getenv("TMPDIR");
    const std::string luaDir = std::string(tmp ? tmp : "/tmp") + "/sls_smoke_lua";

    printf("[smoke] LuaLoader T1+T2+T6 smoke test\n");
    printf("[smoke] script dir: %s\n", luaDir.c_str());

    // ── T6 pure-logic checks (no Lua VM, no network) ───────────────────────
    printf("[smoke] --- T6 pure-logic checks ---\n");

    // URL template substitution.
    {
        const char* wudrmTmpl    = "http://gmrc.wudrm.com/manifest/{gid}";
        const char* steamrunTmpl = "https://manifest.steam.run/api/manifest/{gid}";
        uint64_t gid = 1234567890123456789ULL;

        std::string wu  = SmokeLoader::buildUrl(wudrmTmpl, gid);
        std::string sr  = SmokeLoader::buildUrl(steamrunTmpl, gid);

        // Manually build expected strings.
        char expected[256];
        std::snprintf(expected, sizeof(expected),
                      "http://gmrc.wudrm.com/manifest/%llu",
                      static_cast<unsigned long long>(gid));
        if (wu != expected) {
            fprintf(stderr, "[smoke] FAIL T6: wudrm URL wrong: '%s' (expected '%s')\n",
                    wu.c_str(), expected);
            SmokeLoader::g_had_error = true;
        } else {
            printf("[smoke] T6 PASS: wudrm URL: %s\n", wu.c_str());
        }

        std::snprintf(expected, sizeof(expected),
                      "https://manifest.steam.run/api/manifest/%llu",
                      static_cast<unsigned long long>(gid));
        if (sr != expected) {
            fprintf(stderr, "[smoke] FAIL T6: steamrun URL wrong: '%s' (expected '%s')\n",
                    sr.c_str(), expected);
            SmokeLoader::g_had_error = true;
        } else {
            printf("[smoke] T6 PASS: steamrun URL: %s\n", sr.c_str());
        }
    }

    // parsePlainUint: valid + edge cases.
    {
        uint64_t v = 0;
        // Normal decimal.
        if (!SmokeLoader::parsePlainUint("9999999999999999999", v) || v != 9999999999999999999ULL) {
            fprintf(stderr, "[smoke] FAIL T6: parsePlainUint large value\n");
            SmokeLoader::g_had_error = true;
        }
        // Trailing whitespace (wudrm bodies often have a newline).
        if (!SmokeLoader::parsePlainUint("123456789\n", v) || v != 123456789ULL) {
            fprintf(stderr, "[smoke] FAIL T6: parsePlainUint trailing newline\n");
            SmokeLoader::g_had_error = true;
        }
        // Zero — should parse but caller must reject (parsePlainUint itself allows 0).
        if (!SmokeLoader::parsePlainUint("0", v) || v != 0) {
            fprintf(stderr, "[smoke] FAIL T6: parsePlainUint zero\n");
            SmokeLoader::g_had_error = true;
        }
        // Empty → false.
        if (SmokeLoader::parsePlainUint("", v)) {
            fprintf(stderr, "[smoke] FAIL T6: parsePlainUint empty should be false\n");
            SmokeLoader::g_had_error = true;
        }
        // Non-digit → false.
        if (SmokeLoader::parsePlainUint("abc", v)) {
            fprintf(stderr, "[smoke] FAIL T6: parsePlainUint 'abc' should be false\n");
            SmokeLoader::g_had_error = true;
        }
        printf("[smoke] T6 PASS: parsePlainUint edge cases\n");
    }

    // parseSteamRunJson: valid JSON and missing-key cases.
    {
        uint64_t v = 0;
        const std::string_view goodJson =
            R"({"status":"ok","content":"7654321098765432109"})";
        if (!SmokeLoader::parseSteamRunJson(goodJson, v) || v != 7654321098765432109ULL) {
            fprintf(stderr, "[smoke] FAIL T6: parseSteamRunJson good JSON\n");
            SmokeLoader::g_had_error = true;
        }
        const std::string_view noContent = R"({"status":"ok"})";
        if (SmokeLoader::parseSteamRunJson(noContent, v)) {
            fprintf(stderr, "[smoke] FAIL T6: parseSteamRunJson no-content-key should be false\n");
            SmokeLoader::g_had_error = true;
        }
        const std::string_view garbage = "not json at all";
        if (SmokeLoader::parseSteamRunJson(garbage, v)) {
            fprintf(stderr, "[smoke] FAIL T6: parseSteamRunJson garbage should be false\n");
            SmokeLoader::g_had_error = true;
        }
        printf("[smoke] T6 PASS: parseSteamRunJson edge cases\n");
    }

    // ── T1+T2 Lua script test ──────────────────────────────────────────────
    printf("[smoke] --- T1+T2+T6 Lua script checks ---\n");
    std::filesystem::create_directories(luaDir);
    {
        std::ofstream f(luaDir + "/test.lua");
        if (!f) { fprintf(stderr, "[smoke] cannot write test.lua\n"); return 1; }

        // T1: mixed-case binding reachability + T2 table population.
        f << "-- test.lua: T1 case-insensitive reachability + T2+T6 bindings\n";

        // addappid: basic (inserts into ownedAppIds)
        f << "AddAppId(12345)\n";
        // addappid with DLC flag and 64-hex key (all lowercase a-f hex digits)
        f << "ADDAPPID(99999, 1, \"" << std::string(64, 'a') << "\")\n";
        // addappid with mixed-case name
        f << "AddAppID(11111)\n";

        // addtoken: decimal string
        f << "ADDTOKEN(12345, \"9876543210\")\n";
        f << "Addtoken(11111, \"0\")\n";

        // setmanifestid: 2-arg form
        f << "SetManifestId(12345, \"1111111111111111\")\n";
        // setmanifestid: 3-arg form with integer size
        f << "SETMANIFESTID(99999, \"2222222222222222\", 512)\n";
        // setmanifestid: mixed case
        f << "setManifestID(11111, \"3333333333333333\")\n";

        // T6: fetch_manifest_code_ex registers a callback that returns a known code
        // when called with (appId=1, depotId=2, gid=999).
        // Use 9000000000000000001 — fits in Lua 5.4 integer (must be < 2^63).
        f << "fetch_manifest_code_ex(function(app, depot, gid)\n";
        f << "  -- Return a well-known non-zero code for gid 999.\n";
        f << "  if gid == 999 then return 9000000000000000001 end\n";
        f << "  return nil\n";
        f << "end)\n";

        // T6: also register a fetch_manifest_code (branch 2) that returns a
        // different sentinel — should NOT be reached while _ex is set.
        f << "fetch_manifest_code(function(gid)\n";
        f << "  return 11111111111111111\n"; // sentinel: should never be seen when _ex is set
        f << "end)\n";

        // T6: http_get with optional headers table (must not crash).
        f << "local body, st = HTTP_GET(\"http://example.com\", {[\"X-Test\"]=\"val\"})\n";
        f << "assert(type(st) == \"number\", \"http_get status must be number\")\n";

        // T6: http_post with body and optional headers table.
        f << "local r, st2 = Http_Post(\"http://x\", \"body\", {[\"Content-Type\"]=\"text/plain\"})\n";
        f << "assert(type(st2) == \"number\", \"http_post status must be number\")\n";

        // Remaining stubs — just verify they don't crash.
        f << "SetAppTicket(1, \"deadbeef\")\n";
        f << "SETETICKET(1, \"deadbeef\")\n";
        f << "SetStat(1, \"76561198000000000\")\n";
        f << "DownloadApp(1)\n";
        f << "AddNonOwnedDepot(1)\n";
        f << "SetNotifyOnDownloadComplete(true)\n";
        f << "SetStPropertyForAccount(1, \"k\", \"v\")\n";

        f << "print(\"[smoke] all bindings resolved OK\")\n";
    }

    SmokeLoader::init(luaDir);

    // ── T2 table-population assertions ──────────────────────────────────
    if (!SmokeLoader::g_had_error) {
        // ownedAppIds should contain 12345, 99999, 11111
        auto& owned = SmokeLoader::g_ownedAppIds;
        if (!owned.count(12345)) {
            fprintf(stderr, "[smoke] FAIL: 12345 not in ownedAppIds\n");
            SmokeLoader::g_had_error = true;
        }
        if (!owned.count(99999)) {
            fprintf(stderr, "[smoke] FAIL: 99999 not in ownedAppIds\n");
            SmokeLoader::g_had_error = true;
        }
        if (!owned.count(11111)) {
            fprintf(stderr, "[smoke] FAIL: 11111 not in ownedAppIds\n");
            SmokeLoader::g_had_error = true;
        }

        // depotKeys[99999] should be 32 bytes of 0xaa (64 'a' chars)
        auto& keys = SmokeLoader::g_depotKeys;
        if (keys.find(99999) == keys.end() || keys.at(99999).size() != 32) {
            fprintf(stderr, "[smoke] FAIL: depotKeys[99999] missing or wrong size\n");
            SmokeLoader::g_had_error = true;
        } else {
            for (uint8_t b : keys.at(99999)) {
                if (b != 0xaa) {
                    fprintf(stderr, "[smoke] FAIL: depotKeys[99999] byte mismatch (got 0x%02x)\n", b);
                    SmokeLoader::g_had_error = true;
                    break;
                }
            }
        }

        // appTokens[12345] should be 9876543210
        auto& tokens = SmokeLoader::g_appTokens;
        if (!tokens.count(12345) || tokens.at(12345) != 9876543210ULL) {
            fprintf(stderr, "[smoke] FAIL: appTokens[12345] wrong (got %llu)\n",
                    tokens.count(12345) ? (unsigned long long)tokens.at(12345) : 0ULL);
            SmokeLoader::g_had_error = true;
        }
        if (!tokens.count(11111) || tokens.at(11111) != 0ULL) {
            fprintf(stderr, "[smoke] FAIL: appTokens[11111] wrong\n");
            SmokeLoader::g_had_error = true;
        }

        // manifestOverrides
        auto& mfst = SmokeLoader::g_manifestOverrides;
        if (!mfst.count(12345) || mfst.at(12345).gid != 1111111111111111ULL) {
            fprintf(stderr, "[smoke] FAIL: manifestOverrides[12345].gid wrong\n");
            SmokeLoader::g_had_error = true;
        }
        if (!mfst.count(99999) || mfst.at(99999).gid != 2222222222222222ULL ||
            mfst.at(99999).size != 512) {
            fprintf(stderr, "[smoke] FAIL: manifestOverrides[99999] wrong\n");
            SmokeLoader::g_had_error = true;
        }
        if (!mfst.count(11111) || mfst.at(11111).gid != 3333333333333333ULL) {
            fprintf(stderr, "[smoke] FAIL: manifestOverrides[11111].gid wrong\n");
            SmokeLoader::g_had_error = true;
        }
    }

    // ── T6: callback selection order assertions ──────────────────────────
    printf("[smoke] --- T6 callback selection order ---\n");
    if (!SmokeLoader::g_had_error) {

        // Verify refs were captured by the Lua script.
        if (SmokeLoader::g_fetchCodeExRef == LUA_NOREF) {
            fprintf(stderr, "[smoke] FAIL T6: g_fetchCodeExRef not set after script\n");
            SmokeLoader::g_had_error = true;
        }
        if (SmokeLoader::g_fetchCodeRef == LUA_NOREF) {
            fprintf(stderr, "[smoke] FAIL T6: g_fetchCodeRef not set after script\n");
            SmokeLoader::g_had_error = true;
        }

        // Branch 1: _ex ref set → must call fetch_manifest_code_ex(1, 2, 999)
        // and get 9000000000000000001 (fits in Lua 5.4 signed integer).
        if (!SmokeLoader::g_had_error) {
            uint64_t code = 0;
            bool ok = SmokeLoader::fetchManifestCode(1, 2, 999, code);
            if (!ok) {
                fprintf(stderr, "[smoke] FAIL T6: branch1 fetchManifestCode returned false\n");
                SmokeLoader::g_had_error = true;
            } else if (code != 9000000000000000001ULL) {
                fprintf(stderr, "[smoke] FAIL T6: branch1 wrong code %llu (expected 9000000000000000001)\n",
                        (unsigned long long)code);
                SmokeLoader::g_had_error = true;
            } else {
                printf("[smoke] T6 PASS: branch1 (fetch_manifest_code_ex) code=%llu\n",
                       (unsigned long long)code);
            }
        }

        // Branch 1 with gid≠999: _ex returns nil → fall through to branch 2.
        // Branch 2 (fetch_manifest_code) returns 11111111111111111.
        if (!SmokeLoader::g_had_error) {
            uint64_t code = 0;
            bool ok = SmokeLoader::fetchManifestCode(1, 2, 1, code);
            if (!ok) {
                fprintf(stderr, "[smoke] FAIL T6: branch2 fetchManifestCode returned false\n");
                SmokeLoader::g_had_error = true;
            } else if (code != 11111111111111111ULL) {
                fprintf(stderr, "[smoke] FAIL T6: branch2 wrong code %llu (expected 11111111111111111)\n",
                        (unsigned long long)code);
                SmokeLoader::g_had_error = true;
            } else {
                printf("[smoke] T6 PASS: branch2 (fetch_manifest_code) fallthrough code=%llu\n",
                       (unsigned long long)code);
            }
        }

        // Stack balance: verify Lua stack top is unchanged after the calls.
        if (!SmokeLoader::g_had_error) {
            int top = lua_gettop(SmokeLoader::g_lua);
            if (top != 0) {
                fprintf(stderr, "[smoke] FAIL T6: Lua stack not balanced after fetchManifestCode calls (top=%d)\n", top);
                SmokeLoader::g_had_error = true;
            } else {
                printf("[smoke] T6 PASS: Lua stack balanced (top=0)\n");
            }
        }

        // Branch 3: unset both refs → fetchManifestCode returns false (no network).
        if (!SmokeLoader::g_had_error) {
            int savedEx  = SmokeLoader::g_fetchCodeExRef;
            int savedRef = SmokeLoader::g_fetchCodeRef;
            SmokeLoader::g_fetchCodeExRef = LUA_NOREF;
            SmokeLoader::g_fetchCodeRef   = LUA_NOREF;

            uint64_t code = 0;
            bool ok = SmokeLoader::fetchManifestCode(1, 2, 42, code);
            // No network → provider branch returns false.  That's correct.
            if (ok) {
                fprintf(stderr, "[smoke] FAIL T6: branch3 should return false without network\n");
                SmokeLoader::g_had_error = true;
            } else {
                printf("[smoke] T6 PASS: branch3 (provider, no network) correctly returned false\n");
            }

            SmokeLoader::g_fetchCodeExRef = savedEx;
            SmokeLoader::g_fetchCodeRef   = savedRef;
        }
    }

    // Cleanup temp files.
    std::error_code ec;
    std::filesystem::remove_all(luaDir, ec);

    if (!SmokeLoader::g_had_error) {
        printf("[smoke] PASS\n");
        return 0;
    }
    fprintf(stderr, "[smoke] FAIL\n");
    return 1;
}
