{
  rev,
  lib,
  stdenv,
  pkgs,
  buildDotnetModule,
  dotnetCorePackages,
}: let
  # ticket-grabber: the .NET companion CLI, built as its own derivation and
  # installed into $out below (see nix-modules/ticket-grabber.nix + deps.json).
  ticketGrabber = import ./ticket-grabber.nix {
    inherit rev buildDotnetModule dotnetCorePackages;
  };

  # The Makefile fetches Lua at build time, but the Nix sandbox has no network.
  # Pre-fetch the identical tarball as a fixed-output derivation. Keep the
  # version + hash in sync with LUA_VER / LUA_SHA256 in the Makefile.
  luaVersion = "5.4.8";
  luaSrc = pkgs.fetchurl {
    url = "https://www.lua.org/ftp/lua-${luaVersion}.tar.gz";
    sha256 = "4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae";
  };

  # The Makefile fetches the protobuf source at build time; the Nix sandbox has no
  # network. Pre-fetch the identical tarball. Keep version + hash in sync with
  # PROTOBUF_VER / PROTOBUF_SHA256 in the Makefile.
  protobufVersion = "3.15.8";
  protobufSrc = pkgs.fetchurl {
    url = "https://github.com/protocolbuffers/protobuf/releases/download/v${protobufVersion}/protobuf-cpp-${protobufVersion}.tar.gz";
    sha256 = "9b57647b898e45253c98fae35146f6a5e9e788817d29019f9280270c951a0038";
  };
in
  pkgs.pkgsi686Linux.stdenv.mkDerivation {
    pname = "SLSsteam";
    version = "${rev}";
    src = ../.;

    nativeBuildInputs = with pkgs; [
      pkg-config
      makeWrapper
      cmake
    ];

    # cmake is needed only as a build tool for the protobuf sub-build inside the
    # Makefile. Prevent stdenv from treating this as a CMake project (there is no
    # CMakeLists.txt at the root).
    dontUseCmakeConfigure = true;

    buildInputs = with pkgs.pkgsi686Linux; [
      openssl
      which
      curl
    ];

    buildPhase = ''
      make clean

      # Pre-stage the Lua sources the Makefile would otherwise download, plus
      # the matching stamp, so `make` skips its (network-less) fetch step.
      mkdir -p third_party/lua
      tar xzf ${luaSrc} -C third_party/lua --strip-components=2 lua-${luaVersion}/src
      rm -f third_party/lua/lua.c third_party/lua/luac.c
      touch third_party/lua/.fetched-${luaVersion}

      # Pre-stage the protobuf source + stamp so make's (network-less) fetch is skipped.
      mkdir -p third_party/protobuf
      tar xzf ${protobufSrc} -C third_party/protobuf --strip-components=1 protobuf-${protobufVersion}
      touch third_party/protobuf/.fetched-${protobufVersion}

      # Build the .so targets explicitly: bare `make` would build the smoke-test
      # binary (now the first Makefile target). ticket-grabber is built as its
      # own derivation and copied into $out in installPhase, so it does not need
      # to be staged into the build tree here.
      make bin/SLSsteam.so bin/library-inject.so
    '';

    installPhase = ''
      mkdir -p $out/
      cp bin/SLSsteam.so $out/
      cp bin/library-inject.so $out/
      cp ${ticketGrabber}/bin/ticket-grabber $out/ticket-grabber

      # Set rpath for the dynamically-linked runtime deps (curl + openssl).
      # Lua is statically linked (third_party/lua), so it needs no rpath entry.
      patchelf --set-rpath ${
        lib.makeLibraryPath [
          pkgs.pkgsi686Linux.curl
          pkgs.pkgsi686Linux.openssl
        ]
      } $out/SLSsteam.so
    '';

    meta = {
      description = "Steamclient Modification for Linux";
      homepage = "https://github.com/AceSLS/SLSsteam";
      license = lib.licenses.agpl3Only;
      platforms = lib.platforms.linux;
    };
  }
