#Thanks to https://stackoverflow.com/questions/52034997/how-to-make-makefile-recompile-when-a-header-file-is-changed for the -MMD & -MP flags
#Without them headers wouldn't trigger recompilation

#Force g++ cause clang crashes on some hooks
CXX := g++
CC := gcc

libs := $(wildcard lib/*.a)
srcs := $(shell find src/ -type f -iname "*.cpp")
objs := $(srcs:src/%.cpp=obj/%.o)
deps := $(objs:%.o=%.d)

# Lua 5.4 is fetched at build time (checksum-verified, NOT committed to git) and
# built into a static archive linked into the .so. This removes any external
# 32-bit lua runtime dependency on every target (gcc .7z / Arch makepkg / Nix),
# matching upstream which only depends on openssl + curl.
# NOTE: the Nix sandbox has no network, so nix-modules/default.nix pre-stages
# these same sources via fetchurl and touches $(LUA_STAMP) so make skips the
# download. Keep LUA_VER / LUA_SHA256 in sync with that file.
LUA_VER    := 5.4.8
LUA_SHA256 := 4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae
LUA_DIR    := third_party/lua
LUA_STAMP  := $(LUA_DIR)/.fetched-$(LUA_VER)

# protobuf is fetched + built at build time (host protoc + 32-bit libprotobuf-lite),
# mirroring the Lua handling above. NOT committed. Keep PROTOBUF_VER / PROTOBUF_SHA256
# in sync with nix-modules/default.nix (its sandbox has no network -> fetchurl pre-stage).
PROTOBUF_VER    := 3.15.8
PROTOBUF_SHA256 := 9b57647b898e45253c98fae35146f6a5e9e788817d29019f9280270c951a0038
PROTOBUF_DIR    := third_party/protobuf
PROTOBUF_STAMP  := $(PROTOBUF_DIR)/.fetched-$(PROTOBUF_VER)
PROTOC          := tools/protoc
PROTOBUF_LITE_A := lib/libprotobuf-lite.a

# The lua 5.4 library sources (everything except the lua.c/luac.c standalone
# mains). Hard-coded rather than $(wildcard) because the tree does not exist at
# parse time on a fresh checkout.
lua_names  := lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo \
              ldump lfunc lgc linit liolib llex lmathlib lmem loadlib lobject \
              lopcodes loslib lparser lstate lstring lstrlib ltable ltablib ltm \
              lundump lutf8lib lvm lzio
lua_a      := obj/liblua5.4.a
lua_objs   := $(lua_names:%=obj/luavendor/%.o)

CXXFLAGS := -O3 -flto=auto -fPIC -m32 -std=c++20 -Wall -Wextra -Wpedantic -Wno-error=format-security -D_GLIBCXX_USE_CXX11_ABI=0
CXXFLAGS += -Ithird_party/lua

LDFLAGS := -shared -Wl,--no-undefined
LDFLAGS += $(shell pkg-config --libs "openssl")
LDFLAGS += $(shell pkg-config --libs "libcurl")
# Vendored lua is linked via $(lua_a); -ldl satisfies loadlib's dlopen (a no-op
# stub on modern glibc, so it adds no external package dependency).
LDFLAGS += -ldl

#DATE := $(shell date "+%Y%m%d%H%M%S")
DATE := $(shell cat res/version.txt)

ifeq ($(shell echo $$NATIVE),1)
	CXXFLAGS += -march=native
endif

#Speed up compilation if additional dependencies are found
ifeq ($(shell type ccache &> /dev/null && echo "found"),found)
	export PATH := /usr/lib/ccache/bin:$(PATH)
endif
ifeq ($(shell type mold &> /dev/null && echo "found"),found)
	LDFLAGS += -fuse-ld=mold
endif

# Smoke-test binary: validates Lua VM init + case-insensitive binding lookup.
# Self-contained: does not link SLSsteam internals (no libmem, no yaml-cpp, etc.).
# Run on the Deck: make lua_smoke && ./bin/lua_smoke
SMOKE_CXXFLAGS := -m32 -std=c++20 -O0 -g \
                  $(shell pkg-config --cflags "lua5.4")
SMOKE_LDFLAGS := $(shell pkg-config --libs "lua5.4")

bin/lua_smoke: tools/lua_smoke/smoke.cpp
	@mkdir -p bin
	$(CXX) $(SMOKE_CXXFLAGS) $< -o $@ $(SMOKE_LDFLAGS)

lua_smoke: bin/lua_smoke

bin/pkg_smoke: tools/pkg_smoke/smoke.cpp
	@mkdir -p bin
	g++ -std=c++17 -o bin/pkg_smoke tools/pkg_smoke/smoke.cpp

pkg_smoke: bin/pkg_smoke

bin/netpacket_smoke: tools/netpacket_smoke/smoke.cpp src/sdk/RawNetPacket.hpp src/sdk/RawNetPacket.cpp src/sdk/CNetPacket.hpp
	@mkdir -p bin
	g++ -std=c++20 -m32 -Og -g -o bin/netpacket_smoke tools/netpacket_smoke/smoke.cpp src/sdk/RawNetPacket.cpp

netpacket_smoke: bin/netpacket_smoke

audit-libs: bin/SLSsteam.so bin/library-inject.so tools/ticket-grabber/bin/Release/net9.0/linux-x64/publish/ticket-grabber

# Fetch + verify + unpack Lua sources on first build. Network is needed only
# here; the Nix build pre-stages the tree + this stamp (its sandbox has no net).
$(LUA_STAMP):
	@mkdir -p $(LUA_DIR)
	curl -fsSL "https://www.lua.org/ftp/lua-$(LUA_VER).tar.gz" -o "$(LUA_DIR)/lua.tar.gz"
	printf '%s  %s\n' "$(LUA_SHA256)" "$(LUA_DIR)/lua.tar.gz" | sha256sum -c -
	tar xzf "$(LUA_DIR)/lua.tar.gz" -C "$(LUA_DIR)" --strip-components=2 "lua-$(LUA_VER)/src"
	rm -f "$(LUA_DIR)/lua.tar.gz" "$(LUA_DIR)/lua.c" "$(LUA_DIR)/luac.c"
	touch "$@"

# Fetch + verify + unpack the full protobuf source tree (need cmake/ + src/, so no
# aggressive strip). Network is needed only here; the Nix build pre-stages this tree
# + stamp (its sandbox has no net).
$(PROTOBUF_STAMP):
	@mkdir -p $(PROTOBUF_DIR)
	curl -fsSL "https://github.com/protocolbuffers/protobuf/releases/download/v$(PROTOBUF_VER)/protobuf-cpp-$(PROTOBUF_VER).tar.gz" -o "$(PROTOBUF_DIR)/protobuf.tar.gz"
	printf '%s  %s\n' "$(PROTOBUF_SHA256)" "$(PROTOBUF_DIR)/protobuf.tar.gz" | sha256sum -c -
	tar xzf "$(PROTOBUF_DIR)/protobuf.tar.gz" -C "$(PROTOBUF_DIR)" --strip-components=1 "protobuf-$(PROTOBUF_VER)"
	rm -f "$(PROTOBUF_DIR)/protobuf.tar.gz"
	touch "$@"

# Build the host protoc from the fetched source via the tarball's bundled CMake
# (as OST does). protoc is a HOST build tool — no -m32. Cached: rebuilt only if the
# stamp changes. tests OFF; static; no shared libs.
$(PROTOC): $(PROTOBUF_STAMP)
	@mkdir -p tools $(PROTOBUF_DIR)/build-host
	cmake -S "$(PROTOBUF_DIR)/cmake" -B "$(PROTOBUF_DIR)/build-host" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		-Dprotobuf_BUILD_TESTS=OFF \
		-Dprotobuf_BUILD_SHARED_LIBS=OFF
	cmake --build "$(PROTOBUF_DIR)/build-host" --target protoc -j
	cp "$(PROTOBUF_DIR)/build-host/protoc" "$(PROTOC)"

# The unpacked .c files are produced by the fetch step (order-only: the stamp's
# mtime must not force a rebuild of every object).
$(LUA_DIR)/%.c: | $(LUA_STAMP) ;

# Compile each lua source as C (gcc) into obj/luavendor/ — a separate tree from
# obj/lua/ (which holds the project's own src/lua/*.cpp) so the pattern rules
# never collide. LUA_USE_LINUX enables the POSIX/dlopen feature set; readline is
# only used by the excluded standalone interpreter.
obj/luavendor/%.o: $(LUA_DIR)/%.c | $(LUA_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -m32 -fPIC -O2 -DLUA_USE_LINUX -I$(LUA_DIR) -c $< -o $@

$(lua_a): $(lua_objs)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

# Project sources compile with -I$(LUA_DIR) and some #include <lua.h>, so every
# object must wait for the lua fetch/extract. Order-only (|): the stamp's mtime
# must not force a full rebuild of the tree.
$(objs): | $(LUA_STAMP)

bin/SLSsteam.so: $(objs) $(lua_a) $(libs)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o bin/SLSsteam.so $(LDFLAGS)

bin/library-inject.so: tools/library-inject/main.cpp tools/library-inject/build.sh
	sh tools/library-inject/build.sh
	@mkdir -p bin
	cp tools/library-inject/library-inject.so bin/library-inject.so

tools/ticket-grabber/bin/Release/net9.0/linux-x64/publish/ticket-grabber:
	sh tools/ticket-grabber/build.sh

-include $(deps)
obj/update.o: src/update.cpp res/version.txt
	$(shell sh ./embed-version.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

-include $(deps)
obj/config.o: src/config.cpp res/config.yaml
	$(shell sh ./embed-config.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

-include $(deps)
obj/%.o : src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

clean:
	rm -rvf "obj/" "bin/" "zips/" "tools/ticket-grabber/bin"

install:
	sh setup.sh

zips: rebuild
	@mkdir -p zips
	7z a -mx9 -m9=lzma2 \
		"zips/SLSsteam $(DATE).7z" \
		"bin/SLSsteam.so" \
		"bin/library-inject.so" \
		"setup.sh" \
		"docs/LICENSE" \
		"res/config.yaml" \
		"tools/SLScheevo" \
		"tools/ticket-grabber/bin/Release/net9.0/linux-x64/publish/ticket-grabber"

	#Compatibility for Github issues
	7z a -mx9 -m9=lzma \
		"zips/SLSsteam $(DATE).zip" \
		"bin/SLSsteam.so" \
		"bin/library-inject.so" \
		"setup.sh" \
		"docs/LICENSE" \
		"res/config.yaml" \
		"tools/SLScheevo" \
		"tools/ticket-grabber/bin/Release/net9.0/linux-x64/publish/ticket-grabber"

zips-config:
	7z a -mx9 -m9=lzma "zips/SLSsteam - SLSConfig $(DATE).zip" "$(HOME)/.config/SLSsteam/config.yaml"
	#Compatibility for Github issues
	7z a -mx9 -m9=lzma2 "zips/SLSsteam - SLSConfig $(DATE).7z" "$(HOME)/.config/SLSsteam/config.yaml"

build: audit-libs
rebuild: clean build
all: clean build zips

.PHONY: all build clean rebuild zips lua_smoke pkg_smoke netpacket_smoke
.NOTPARALLEL: clean rebuild zips
