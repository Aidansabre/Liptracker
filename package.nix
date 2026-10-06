{
  lib,
  stdenv,
  stb,
  libuvc,
  libusb1,
}:
stdenv.mkDerivation {
  pname = "vft-stream";
  version = "0.3-focus3";

  src = lib.cleanSource ./.;

  buildInputs = [
    libuvc
    libusb1
  ];

  # libuvc's .pc doubles its prefix, so flags are spelled out instead of
  # asking pkg-config.
  buildPhase = ''
    runHook preBuild
    $CC -O2 -Wall -Wextra -o vft-stream vft-stream.c tracker.c image.c \
      -I${stb}/include/stb -I${lib.getDev libuvc}/include -I${lib.getDev libusb1}/include/libusb-1.0 \
      -L${lib.getLib libuvc}/lib -L${lib.getLib libusb1}/lib \
      -luvc -lusb-1.0 -lm -lpthread
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 vft-stream -t $out/bin
    runHook postInstall
  '';

  meta = {
    description = "Activate a Vive Facial Tracker or Focus 3 tracker over libuvc and serve MJPEG for Baballonia";
    # A C translation of Baballonia code, so it inherits Babble's
    # non-commercial copyleft license (see LICENSE and the source header).
    # Not free software by the FSF/OSI definitions, hence free = false.
    license = {
      fullName = "Babble Software Distribution License 1.0";
      url = "https://github.com/Project-Babble/Baballonia/blob/main/LICENSE";
      free = false;
      redistributable = true;
    };
    platforms = lib.platforms.linux;
    mainProgram = "vft-stream";
  };
}
