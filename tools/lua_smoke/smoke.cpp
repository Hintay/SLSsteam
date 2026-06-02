// Smoke test for LuaLoader T1+T2.
// Self-contained: no SLSsteam internal headers, no libmem, no yaml-cpp.
// Inlines the same Lua VM setup logic and real T2 binding implementations
// as LuaLoader.cpp to verify:
//   1. The Lua VM can be created.
//   2. The case-insensitive __index metamethod resolves mixed-case names.
//   3. All 14 bindings are reachable without "attempt to call a nil value".
//   4. addappid / addtoken / setmanifestid populate the in-process tables.
//   5. addappid with a 64-hex-char key stores 32 decoded bytes.

#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
        if (lua_isinteger(L, 3)) {
            lua_Integer rs = lua_tointeger(L, 3);
            size = rs >= 0 ? static_cast<uint64_t>(rs) : 0;
        } else if (lua_isstring(L, 3)) {
            parseU64Decimal(lua_tostring(L, 3), size);
        }
    }
    g_manifestOverrides[depotId] = ManifestOverride{gid, size};
    return 0;
}

// fetch_manifest_code and _ex: just accept a function arg (capture not tested here).
static int impl_fetch_manifest_code(lua_State* L) {
    if (lua_gettop(L) < 1 || !lua_isfunction(L, 1)) {
        lua_pushstring(L, "fetch_manifest_code: arg1 must be function");
        return lua_error(L);
    }
    // In smoke we don't keep the ref — just verify the call works.
    return 0;
}
static int impl_fetch_manifest_code_ex(lua_State* L) {
    if (lua_gettop(L) < 1 || !lua_isfunction(L, 1)) {
        lua_pushstring(L, "fetch_manifest_code_ex: arg1 must be function");
        return lua_error(L);
    }
    return 0;
}

// http_get: in smoke, skip real network — return stub values.
static int impl_http_get(lua_State* L) {
    if (lua_gettop(L) < 1 || !lua_isstring(L, 1)) {
        lua_pushstring(L, "http_get: arg1 must be string");
        return lua_error(L);
    }
    // Smoke test: return an empty body + 0 status without hitting the network.
    lua_pushliteral(L, "");
    lua_pushinteger(L, 0);
    return 2;
}

// http_post: returns nil + message (T6 stub).
static int impl_http_post(lua_State* L) {
    lua_pushnil(L);
    lua_pushstring(L, "http_post: not yet implemented (T6)");
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

} // namespace SmokeLoader

// ── Main ──────────────────────────────────────────────────────────────────

int main() {
    const char* tmp = std::getenv("TMPDIR");
    const std::string luaDir = std::string(tmp ? tmp : "/tmp") + "/sls_smoke_lua";

    printf("[smoke] LuaLoader T1+T2 smoke test\n");
    printf("[smoke] script dir: %s\n", luaDir.c_str());

    // Write the T2 test script.
    std::filesystem::create_directories(luaDir);
    {
        std::ofstream f(luaDir + "/test.lua");
        if (!f) { fprintf(stderr, "[smoke] cannot write test.lua\n"); return 1; }

        // ── T1: mixed-case binding reachability ─────────────────────────
        f << "-- test.lua: T1 case-insensitive reachability + T2 table population\n";

        // ── T2: real implementations with varied capitalisation ─────────

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

        // fetch_manifest_code: pass a function
        f << "fetch_manifest_code(function(a,b) return a+b end)\n";
        // fetch_manifest_code_ex: mixed case
        f << "Fetch_Manifest_Code_Ex(function() end)\n";

        // http_get: returns (body, status) — smoke impl returns (\"\", 0)
        f << "local body, st = HTTP_GET(\"http://example.com\")\n";
        f << "assert(st == 0, \"http_get status should be 0 in smoke\")\n";

        // http_post: returns nil + message
        f << "local r, msg = Http_Post(\"http://x\", \"body\")\n";
        f << "assert(r == nil, \"http_post should return nil in smoke\")\n";

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
