# i686 (32-bit) build toolchain for SLSsteam on SteamOS via Nix — no base-image changes
{ pkgs ? import <nixpkgs> {} }:
let p = pkgs.pkgsi686Linux;
in p.mkShell {
  nativeBuildInputs = [ p.gcc p.gnumake p.pkg-config ];
  buildInputs = [ p.openssl p.curl p.lua5_4 ];
}
