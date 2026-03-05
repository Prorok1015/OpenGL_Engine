{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  # Build inputs for the development environment
  buildInputs = [
    pkgs.cmake
    pkgs.gcc13
    pkgs.gdb
    pkgs.pkg-config
    pkgs.assimp
    pkgs.xorg.libX11
    pkgs.xorg.libXrandr
    pkgs.xorg.libXinerama
    pkgs.xorg.libXcursor
    pkgs.libGL
    pkgs.xorg.libXi
    pkgs.wayland
    pkgs.wayland-protocols
    pkgs.libxkbcommon
    pkgs.libffi
    pkgs.zlib
  ];

  # Sets environment variables in the workspace
  shellHook = ''
    export LD_LIBRARY_PATH=${pkgs.libGL}/lib:${pkgs.xorg.libX11}/lib:${pkgs.xorg.libXrandr}/lib:${pkgs.xorg.libXinerama}/lib:${pkgs.xorg.libXcursor}/lib:${pkgs.xorg.libXi}/lib:$LD_LIBRARY_PATH
  '';
}
