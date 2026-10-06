# `nix-build` builds the static aarch64 binary for the Steam Frame.
# `nix-build --arg cross false` builds for the machine you are on instead.
{
  pkgs ? import <nixpkgs> { },
  cross ? true,
}:
let
  # pkgsStatic links libuvc, libusb and musl into the one binary, so it runs
  # on SteamOS with nothing installed.
  target = if cross then pkgs.pkgsCross.aarch64-multiplatform.pkgsStatic else pkgs;
in
target.callPackage ./package.nix { }
