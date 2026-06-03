#Thanks to https://stackoverflow.com/questions/52034997/how-to-make-makefile-recompile-when-a-header-file-is-changed for the -MMD & -MP flags
#Without them headers wouldn't trigger recompilation

#Force g++ cause clang crashes on some hooks
CXX := g++

libs := $(wildcard lib/*.a)
srcs := $(shell find src/ -type f -iname "*.cpp")
objs := $(srcs:src/%.cpp=obj/%.o)
deps := $(objs:%.o=%.d)

CXXFLAGS := -O3 -flto=auto -fPIC -m32 -std=c++20 -Wall -Wextra -Wpedantic -Wno-error=format-security -D_GLIBCXX_USE_CXX11_ABI=0
CXXFLAGS += $(shell pkg-config --cflags "lua5.4")

LDFLAGS := -shared -Wl,--no-undefined
LDFLAGS += $(shell pkg-config --libs "openssl")
LDFLAGS += $(shell pkg-config --libs "libcurl")
LDFLAGS += $(shell pkg-config --libs "lua5.4")

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

bin/netpacket_smoke: tools/netpacket_smoke/smoke.cpp src/sdk/CNetPacket.hpp
	@mkdir -p bin
	g++ -std=c++20 -m32 -O0 -g -o bin/netpacket_smoke tools/netpacket_smoke/smoke.cpp

netpacket_smoke: bin/netpacket_smoke

audit-libs: bin/SLSsteam.so bin/library-inject.so tools/ticket-grabber/bin/Release/net9.0/linux-x64/publish/ticket-grabber

bin/SLSsteam.so: $(objs) $(libs)
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
	$(shell ./embed-version.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

-include $(deps)
obj/config.o: src/config.cpp res/config.yaml
	$(shell ./embed-config.sh)
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
