{
  description = "morse-deluxe - an over-engineered Morse code encoder/decoder (DSA + Dear ImGui + miniaudio)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    dsa = {
      url = "github:hydrastro/dsa";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    # Dear ImGui and miniaudio are header/source drops with no flakes of their
    # own. Pin ImGui to a stable release; miniaudio tracks master.
    imgui-src = {
      url = "github:ocornut/imgui/v1.91.5";
      flake = false;
    };
    miniaudio-src = {
      url = "github:mackron/miniaudio";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, dsa, imgui-src, miniaudio-src }:
    let
      # Match the systems exported by the pinned DSA flake.
      systems = builtins.attrNames dsa.packages;
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);
      pkgsFor = system: nixpkgs.legacyPackages.${system};

      mkMorse = system:
        let
          pkgs = pkgsFor system;
          lib = pkgs.lib;
          isLinux = pkgs.stdenv.isLinux;
          isDarwin = pkgs.stdenv.isDarwin;

          # Audio backends miniaudio dlopen()s at runtime on Linux, plus ffmpeg
          # for the GUI's optional import/export.
          runtimeLibs = lib.optionals isLinux [
            pkgs.alsa-lib
            pkgs.libpulseaudio
            pkgs.libGL
          ];
        in
        pkgs.stdenv.mkDerivation {
          pname = "morse-deluxe";
          version = "1.0.0";
          src = self;

          nativeBuildInputs = [ pkgs.pkg-config ]
            ++ lib.optional isLinux pkgs.makeWrapper;

          buildInputs = [ pkgs.glfw dsa.packages.${system}.default ]
            ++ lib.optionals isLinux [
              pkgs.libGL
              # X11 is GLFW's *Linux* windowing backend only; Windows uses Win32
              # and macOS uses Cocoa, so these are Linux-only inputs.
              pkgs.libx11
              pkgs.libxrandr
              pkgs.libxi
              pkgs.libxcursor
              pkgs.libxinerama
              pkgs.libxext
              pkgs.alsa-lib
              pkgs.libpulseaudio
            ]
            ++ lib.optionals isDarwin (with pkgs.darwin.apple_sdk.frameworks; [
              Cocoa
              OpenGL
              IOKit
              CoreVideo
              CoreAudio
              CoreFoundation
              AudioToolbox
              AudioUnit
            ]);

          # Build everything (core + CLI + tests + GUI) with Make, pointing at
          # the pinned dependency sources.
          buildPhase = ''
            runHook preBuild
            make everything \
              IMGUI_DIR=${imgui-src} \
              MINIAUDIO_DIR=${miniaudio-src} \
              GUI_GLFW=system \
              -j''${NIX_BUILD_CORES:-1}
            runHook postBuild
          '';

          doCheck = true;
          checkPhase = ''
            runHook preCheck
            make check
            runHook postCheck
          '';

          installPhase = ''
            runHook preInstall
            make install PREFIX=$out
            runHook postInstall
          '';

          # Make ffmpeg and the dlopen'd audio backends reachable from the GUI.
          postInstall = lib.optionalString isLinux ''
            wrapProgram $out/bin/morse-deluxe-gui \
              --prefix PATH : ${lib.makeBinPath [ pkgs.ffmpeg ]} \
              --prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath runtimeLibs}
          '';

          meta = with lib; {
            description = "Feature-rich, over-engineered Morse code encoder/decoder";
            longDescription = ''
              A C core built on the DSA intrusive data structures
              (trie + hash table codebook, click-free tone synthesis, Goertzel
              detection, native WAV I/O, a counting allocator) with a command
              line tool and a Dear ImGui desktop application (live keyer,
              oscilloscope, tone meter, microphone decode, ffmpeg import/export).
            '';
            license = licenses.mit;
            platforms = systems;
            mainProgram = "morse-deluxe-gui";
          };
        };
    in
    {
      packages = forAllSystems (system: rec {
        morse-deluxe = mkMorse system;
        default = morse-deluxe;
      });

      apps = forAllSystems (system: {
        # `nix run` launches the GUI; `nix run .#morsec -- ...` runs the CLI.
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/morse-deluxe-gui";
        };
        morsec = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/morsec";
        };
      });

      devShells = forAllSystems (system:
        let
          pkgs = pkgsFor system;
          lib = pkgs.lib;
          # Libraries the make-built GUI loads at runtime: miniaudio dlopen()s
          # the audio backends, and OpenGL is needed too. Putting them on
          # LD_LIBRARY_PATH lets a binary built with plain `make` (not the
          # wrapped `nix build` output) actually find them inside the shell.
          runLibs = lib.optionals pkgs.stdenv.isLinux [
            pkgs.alsa-lib
            pkgs.libpulseaudio
            pkgs.libGL
          ];
        in {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.default ];
            packages = [
              pkgs.gnumake
              pkgs.pkg-config
              pkgs.ffmpeg
              pkgs.gdb
              pkgs.clang-tools # clang-format / clang-tidy
            ];
            shellHook = ''
              ${lib.optionalString pkgs.stdenv.isLinux ''
                export LD_LIBRARY_PATH="${lib.makeLibraryPath runLibs}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
                # Link ALSA + PulseAudio directly so `make gui` produces a binary
                # whose audio works without LD_LIBRARY_PATH (rpath is baked in).
                export AUDIO_DIRECT=1
              ''}
              echo "morse-deluxe dev shell"
              echo "Build with Make, pointing at the pinned dependency sources:"
              echo "  make everything \\"
              echo "    IMGUI_DIR=${imgui-src} \\"
              echo "    MINIAUDIO_DIR=${miniaudio-src}"
              echo "  make check"
              ${lib.optionalString pkgs.stdenv.isLinux ''
                echo "(AUDIO_DIRECT=1: audio backends linked directly; GL via LD_LIBRARY_PATH)"
              ''}
            '';
          };
        });

      formatter = forAllSystems (system: (pkgsFor system).nixpkgs-fmt);
    };
}
