{ pkgs, ... }: {
  # Указываем канал Nixpkgs (стандартное требование IDX)
  channel = "stable-23.11"; # Или "stable-24.05", если вам нужны самые свежие пакеты

  # В IDX все зависимости (buildInputs и nativeBuildInputs) пишутся в единый массив
  packages = [
    pkgs.gcc13
    pkgs.gnumake
    pkgs.cmake
    pkgs.gdb
    pkgs.pkg-config
    pkgs.boost
    pkgs.libGL
    pkgs.libGL.dev
    pkgs.wayland
    pkgs.wayland-protocols
    pkgs.libxkbcommon
    pkgs.libffi
    pkgs.zlib
    pkgs.xorg.xorgproto
    pkgs.xorg.libxcb
    pkgs.xorg.libX11
    pkgs.xorg.libXrandr
    pkgs.xorg.libXinerama
    pkgs.xorg.libXcursor
    pkgs.xorg.libXi
    pkgs.xorg.libXext
    pkgs.xorg.libXrender
    pkgs.xorg.libXfixes
    pkgs.xorg.libxcb.dev
    pkgs.wayland.dev
    pkgs.libxkbcommon.dev
    pkgs.libffi.dev
    pkgs.xorg.libX11.dev
    pkgs.xorg.libXrandr.dev
    pkgs.xorg.libXinerama.dev
    pkgs.xorg.libXcursor.dev
    pkgs.xorg.libXi.dev
    pkgs.xorg.libXext.dev
    pkgs.xorg.libXrender.dev
    pkgs.xorg.libXfixes.dev
    pkgs.xvfb-run
  ];

  # Переменные окружения задаются здесь (IDX сам применит их к терминалу)
  env = {
    LD_LIBRARY_PATH = "${pkgs.lib.makeLibraryPath [
      pkgs.libGL
      pkgs.wayland
      pkgs.xorg.libX11
      pkgs.xorg.libXrandr
      pkgs.xorg.libXinerama
      pkgs.xorg.libXcursor
      pkgs.xorg.libXi
      pkgs.xorg.libXext
      pkgs.xorg.libXrender
      pkgs.xorg.libXfixes
    ]}";

    # Автоматически собираем пути к .pc файлам для всех dev-пакетов
    PKG_CONFIG_PATH = "${pkgs.lib.makeSearchPathOutput "dev" "lib/pkgconfig" [
      pkgs.wayland
      pkgs.libxkbcommon
      pkgs.libffi
      pkgs.xorg.libX11
      pkgs.xorg.libXrandr
      pkgs.xorg.libXinerama
      pkgs.xorg.libXcursor
      pkgs.xorg.libXi
      pkgs.xorg.libXext
      pkgs.xorg.libXrender
      pkgs.xorg.libXfixes
    ]}";

    CMAKE_INCLUDE_PATH = "${pkgs.lib.concatStringsSep ":" [
      "${pkgs.libGL.dev}/include"
      "${pkgs.xorg.xorgproto}/include"
      "${pkgs.xorg.libX11.dev}/include"
      "${pkgs.xorg.libxcb.dev}/include"
      "${pkgs.xorg.libXrandr.dev}/include"
      "${pkgs.xorg.libXinerama.dev}/include"
      "${pkgs.xorg.libXcursor.dev}/include"
      "${pkgs.xorg.libXi.dev}/include"
      "${pkgs.xorg.libXext.dev}/include"
      "${pkgs.xorg.libXrender.dev}/include"
      "${pkgs.xorg.libXfixes.dev}/include"
    ]}";

    CMAKE_LIBRARY_PATH = "${pkgs.lib.concatStringsSep ":" [
      "${pkgs.xorg.libX11}/lib"
      "${pkgs.xorg.libxcb}/lib"
      "${pkgs.xorg.libXrandr}/lib"
      "${pkgs.xorg.libXinerama}/lib"
      "${pkgs.xorg.libXcursor}/lib"
      "${pkgs.xorg.libXi}/lib"
      "${pkgs.xorg.libXext}/lib"
      "${pkgs.xorg.libXrender}/lib"
      "${pkgs.xorg.libXfixes}/lib"
    ]}";

    # --- ПРЯМАЯ НАВОДКА ДЛЯ КОМПИЛЯТОРА GCC ---
    CPATH = "${pkgs.lib.concatStringsSep ":" [
      "${pkgs.libGL.dev}/include"
      "${pkgs.xorg.xorgproto}/include"
      "${pkgs.xorg.libX11.dev}/include"
      "${pkgs.xorg.libxcb.dev}/include"
      "${pkgs.xorg.libXrandr.dev}/include"
      "${pkgs.xorg.libXinerama.dev}/include"
      "${pkgs.xorg.libXcursor.dev}/include"
      "${pkgs.xorg.libXi.dev}/include"
      "${pkgs.xorg.libXext.dev}/include"
      "${pkgs.xorg.libXrender.dev}/include"
      "${pkgs.xorg.libXfixes.dev}/include"
    ]}";
  };

  # Специфичные настройки Project IDX
  idx = {
    workspace = {
      # Эти команды выполняются при (пере)создании окружения
      onCreate = {
        setup-build-folder = "mkdir -p build";
      };
    };
  };
}