{ pkgs, arduino-ctags }:

let
  # Build Arduino's ctags fork natively, including on Apple Silicon.
  arduinoCtags = pkgs.stdenv.mkDerivation {
    pname = "arduino-ctags";
    version = "5.8-arduino11";
    src = arduino-ctags;
    nativeBuildInputs = [ pkgs.autoreconfHook ];
    configureFlags = [ "--enable-tmpdir=/tmp" ];
    # The upstream C sources predate C99.
    env.CFLAGS = "-std=gnu89";
    # Avoid a collision with the macOS SDK's reserved macro.
    postPatch = pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isDarwin ''
      substituteInPlace *.[ch] --replace-quiet '__unused__' 'CTAGS_UNUSED'
    '';
  };
in
{
  arduinoCli = pkgs.arduino-cli;
  inherit arduinoCtags;
}
