# morsw

**morsw** is a deliberately over-engineered Morse code **encoder and decoder**,
written in C on top of the [`hydrastro/ds`](https://github.com/hydrastro/ds)
intrusive data-structure library, with a command-line tool, a clean
[Dear ImGui](https://github.com/ocornut/imgui) desktop application, real-time
audio via [miniaudio](https://github.com/mackron/miniaudio), optional
[ffmpeg](https://ffmpeg.org/) transcoding, interfaces to **real CW radio gear**,
a portable **Makefile** (Linux / macOS / Windows-MinGW), and a
[Nix](https://nixos.org/) flake.

It does not just turn text into dots and dashes. It synthesizes click-free
sidetone audio, writes real `.wav` files, and decodes Morse **back out of audio
of any pitch**. Crucially it can pull apart **many stations transmitting at once
on different pitches** — exactly the situation in real on-air recordings such as
the Titanic distress traffic — transcribing each one separately. It decodes
**live from a microphone or from the system's own audio output** (the default),
lets you key by hand (straight or iambic), drives a **serial line keyer /
WinKeyer / cwdaemon**, draws the code book as a **binary tree**, and accounts for
every byte it allocates.

> The library and binaries are still named `libmorse` / `morsec` /
> `morse-deluxe-gui` internally; "morsw" is the project name.

---

## Highlights

- **Full codebook** — international letters and digits, plus an extended mode
  with punctuation, accented letters (À–Þ), and named prosigns (`<AR>`, `<SK>`,
  `<SOS>`, …). Decode uses a **trie**; encode uses a **hash table**.
- **Accurate timing** — PARIS-standard dit length, ARRL **Farnsworth** spacing,
  and adjustable dit/dah **weight** that holds the overall period constant.
- **Click-free synthesis** — sine tone shaped by a raised-cosine (Hann) ramp,
  phase-continuous across elements, with optional additive noise.
- **Decode any tone** — the detector estimates the dominant CW frequency from
  the signal itself (Welch-averaged FFT spectrum with parabolic interpolation),
  so it locks onto whatever pitch is present and can follow drift. Works across
  a wide configurable band and on noisy signals.
- **Noise rejection** — idle and noisy channels no longer decode into streams
  of stray dits and dashes. Each station's tone must rise above the noise floor
  *and* its neighbouring frequencies (an SNR squelch) before a mark is accepted,
  and a channel is only reported once it shows real on/off keying, so static,
  hiss, hum, carriers and broadband audio are turned away while genuine CW gets
  through. A **Squelch** control trades noise rejection against weak-signal copy.
- **Audio band-pass** — a cascaded biquad (Butterworth) filter cleans the input
  before decoding, removing sub-audible rumble, mains hum, hiss and out-of-band
  interference; narrow it onto a station to lift a weak signal out of the noise.
- **One tone, or many** — by default the decoder follows only the single
  strongest, confident tone at any instant, which is exactly right when stations
  take turns. Detection uses a robust **median noise floor**, so broadband hiss
  and random spikes are not mistaken for stations — idle frequencies stay quiet
  instead of filling with junk. Flip one switch and a rolling FFT tracks every
  active peak and decodes several genuinely simultaneous stations in parallel.
  Each station gets its own frequency lock, threshold, and speed.
- **Chat-log timeline** — every transmission becomes one timestamped message,
  grouped per station and ordered in time, so the decode reads like a
  conversation; a side panel keeps each frequency's full running transcript.
- **Scrollable SDR waterfall** — a classic spectrogram (frequency across, newest
  at the bottom, colour = intensity) that follows live and scrolls back through
  history, for live audio or a whole file, so you can *see* the stations come
  and go.
- **Decode live audio** — from a chosen **input device**, or from **system
  output** (the default: WASAPI loopback on Windows; an auto-selected monitor /
  "Stereo Mix" input elsewhere).
- **Code book as a tree** — view the dichotomic dot/dash tree (up = dot,
  down = dash) in the GUI or as an ASCII tree from the CLI.
- **Interface with real CW gear** — an iambic (Curtis A/B) paddle keyer, a
  **serial-port line keyer** (RTS/DTR keying + PTT), a **cwdaemon-compatible UDP
  server**, and a **K1EL WinKeyer** host-mode command builder.
- **Counting allocator** — every allocation in the core (including the `ds`
  container nodes) is routed through an instrumented allocator reporting
  live / peak / total bytes and a balanced alloc/free count.
- **Three front-ends** — `libmorse` (C library), `morsec` (CLI), and
  `morse-deluxe-gui` (a tabbed Dear ImGui app).
- **Portable** — core + CLI build everywhere with only `ds` + libm; the GUI runs
  on Linux, macOS, and Windows.

---

## Repository layout

```
morse-deluxe/
├── include/morse/      Public C API (one header per subsystem)
│   ├── types.h table.h timing.h encode.h decode.h
│   ├── synth.h detect.h fft.h    wav.h     cw.h    morse.h (umbrella)
├── src/                Core library implementation (pure C11)
│   ├── morse_alloc.{c,h}   counting allocator (private)
│   ├── fft.c               radix-2 FFT + dominant-tone estimator
│   ├── filter.c            biquad band-pass (Butterworth) audio filter
│   ├── multi.c             multi-station decoder (one detector per peak)
│   ├── cw.c                iambic keyer, serial keyer, cwdaemon, WinKeyer
│   └── table.c encode.c decode.c synth.c detect.c wav.c timing.c types.c morse.c
├── app/
│   ├── cli/main.c      morsec command-line tool
│   └── gui/            Dear ImGui application (tabbed workspace)
│       ├── main.cpp        GLFW + OpenGL3 + ImGui bootstrap
│       ├── app.{hpp,cpp}   tabs + libmorse glue
│       ├── audio.{hpp,cpp} miniaudio playback / keyer / capture / loopback
│       ├── cwio.{hpp,cpp}  background threads for serial + cwdaemon
│       ├── media.{hpp,cpp} ffmpeg import/export helpers
│       └── miniaudio_impl.c the single miniaudio implementation TU
├── tests/              Unit tests: timing, table, encode/decode, synth/detect,
│                       round-trip, fft/tone, cw
├── Makefile            Primary build (Linux / macOS / Windows-MinGW)
├── CMakeLists.txt      Alternative build
├── flake.nix           Nix build / dev shell
└── docs/ARCHITECTURE.md
```

---

## Building

### With Make (primary)

```sh
make            # core library, CLI, and tests
make check      # build + run the unit tests
make gui        # the Dear ImGui desktop application
make everything # all of the above
make deps       # fetch ds / Dear ImGui / miniaudio into third_party/
make install    # install libs, headers, binaries under PREFIX (default /usr/local)
make help       # list targets and options
```

The core library and CLI depend only on `ds` + libm and build on any platform.
If you do not already have a `ds` checkout, run `make deps` (it fetches `ds`,
Dear ImGui, and miniaudio into `third_party/`), or point at an existing one:

```sh
make DS_ROOT=/path/to/ds            # use an existing ds checkout
make gui DS_ROOT=/path/to/ds        # likewise for the GUI
```

Common options (pass on the command line):

| Variable | Default | Meaning |
| --- | --- | --- |
| `DS_ROOT` | `third_party/ds` | location of a `hydrastro/ds` checkout |
| `IMGUI_DIR` | `third_party/imgui` | Dear ImGui source tree (GUI) |
| `MINIAUDIO_DIR` | `third_party/miniaudio` | dir containing `miniaudio.h` (GUI) |
| `GUI_GLFW` | `system` | `system` (pkg-config) or `vendor` GLFW |
| `PREFIX` | `/usr/local` | install prefix |
| `CC` / `CXX` | `cc` / `c++` | compilers |

#### Platform notes (and what X11 is for)

The Makefile auto-detects the platform and links the right libraries:

| Platform | Window/Input | Audio | GUI extra libs |
| --- | --- | --- | --- |
| **Linux** | GLFW → **X11** | miniaudio → ALSA/PulseAudio | `-lGL -lpthread -ldl` + X11 dev pkgs |
| **macOS** | GLFW → Cocoa | miniaudio → CoreAudio | Cocoa/IOKit/CoreAudio frameworks |
| **Windows** (MinGW) | GLFW → **Win32** | miniaudio → WASAPI | `-lopengl32 -lgdi32 -lole32 -lwinmm …` |

**X11 is only the Linux windowing/input backend for GLFW.** It is not used on
Windows (which uses Win32) or macOS (which uses Cocoa). So on Windows you do not
need X11 at all — just build the GUI with MinGW:

```sh
# In an MSYS2 / MinGW-w64 shell, with mingw-w64 GLFW installed (or GUI_GLFW=vendor)
make gui
```

On Linux the GUI needs the usual X11 development packages (e.g. on Debian/Ubuntu
`libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev`) plus
`libgl1-mesa-dev`, and ALSA/PulseAudio headers are not required (miniaudio
`dlopen`s them at runtime). `ffmpeg` is optional and only used for importing or
exporting non-WAV audio.

### With Nix

```sh
nix build                 # core + CLI + tests + GUI
nix run                   # launch the GUI
nix run .#morsec -- encode "CQ CQ DE K1ABC <AR>"
nix develop               # dev shell with make/ffmpeg/clang-tools
```

The flake pins `ds`, Dear ImGui, and miniaudio as inputs and wraps the GUI so
the dlopen'd audio backends and `ffmpeg` resolve at runtime. The lock file is
generated on first build.

> **No audio from a `make`-built binary on NixOS?** miniaudio loads ALSA /
> PulseAudio by `dlopen`-ing them at runtime, so a binary you run *directly*
> needs those libraries on the loader path. `nix run` / `nix build` handle this
> by wrapping the binary. For the `make` workflow, build and run **inside**
> `nix develop` — the dev shell now puts the audio + GL libraries on
> `LD_LIBRARY_PATH` for you. (If you run the binary outside any nix shell, the
> app prints the exact `LD_LIBRARY_PATH` to set.)

### With CMake (alternative)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Options mirror the Makefile: `MORSE_BUILD_CLI`, `MORSE_BUILD_GUI`,
`MORSE_BUILD_TESTS` (all `ON`), and `DS_ROOT` / `IMGUI_DIR` / `MINIAUDIO_DIR`.

---

## CLI usage (`morsec`)

```
morsec <command> [options] [args]

Commands:
  encode TEXT...      Encode text to Morse (reads stdin if no TEXT)
  decode MORSE...     Decode Morse to text  (reads stdin if none)
  wav    TEXT...      Render text to a WAV file (use -o FILE)
  listen FILE.wav     Decode Morse audio from a WAV file (any pitch)
  stations FILE.wav   Decode many simultaneous stations from a WAV
  table               Print the codebook reference chart
  tree [depth]        Print the codebook as a dot/dash tree
  serial TEXT...      Key TEXT out a serial port's RTS/DTR line
  cwdaemon            Run a cwdaemon-compatible UDP keyer server
  winkeyer TEXT...    Emit a K1EL WinKeyer host-mode byte stream
```

Examples:

```sh
morsec encode "SOS"                       # ... --- ...
morsec encode "<AR> 73"                    # .-.-. / --... ...--
echo ".... ." | morsec decode              # HI
morsec wav -w 25 -t 700 -o cq.wav "CQ DE K1ABC"
morsec listen cq.wav                       # decode it back (auto-detects 700 Hz)
morsec stations titanic.wav                # decode the strongest tone over time
morsec stations --multi titanic.wav        # decode simultaneous stations
morsec tree 4                              # print the dot/dash code tree

# Drive real gear
morsec serial -D /dev/ttyUSB0 --keyline rts -w 22 "CQ TEST"
morsec winkeyer -w 22 "CQ TEST" > /dev/ttyUSB0
morsec cwdaemon --port 6789 -D /dev/ttyUSB0   # then send UDP text to :6789
```

Other options include `-V/--variant base|ext`, `-f/--farnsworth`, `--weight`,
`-r/--rate`, `-a/--amplitude`, `--ramp`, `--noise`, `--dit`/`--dah`,
`--diagnostics`, and `--hex` (WinKeyer). Run `morsec --help` for the full list.

---

## GUI (`morse-deluxe-gui`)

A single tabbed workspace (no scattered windows) with a persistent status bar:

- **Encode** — variant, timing/tone controls, text → Morse, render / play /
  export, and a live waveform.
- **Decode** — *From text* (paste dots and dashes) and *From audio*, laid out
  like an SDR receiver: a live spectrum and a **scrollable waterfall** across the
  top, and below them the detected **stations** (each with its pitch, speed and
  full running transcript) beside a colour-coded **chat log** of what was sent in
  order. Input defaults to the **system's audio output**, with device selection,
  an **audio band-pass**, and a **Simultaneous stations** switch (single
  strongest tone vs several at once). Decode a file (`.wav`, or any format via
  ffmpeg) or listen live.
- **Keyer** — straight key or **iambic paddle** (Curtis A/B); hold `SPACE`
  (straight) or `Z`/`X` (paddles), hear click-free sidetone, and watch the
  streaming decode with a live WPM estimate.
- **CW Interface** — serial line keying (device + RTS/DTR), a cwdaemon UDP
  server (start/stop), and a WinKeyer byte-stream preview.
- **Reference** — the full code book as a searchable table or a dot/dash tree.
- **Diagnostics** — live allocation counters and platform capabilities.

---

## Decoding audio of any pitch

Rather than assume a fixed tone, the detector estimates the dominant CW
frequency from the signal itself: it computes a Welch-averaged magnitude
spectrum (a small radix-2 FFT over overlapping Hann windows) and refines the
strongest peak in a configurable band to sub-bin resolution with parabolic
interpolation. That estimate seeds a single-bin **Goertzel** power detector,
which is normalized against an adaptive noise floor and passed through a
Schmitt-trigger to recover clean mark/space timing. In the live path the pitch
estimate can keep tracking, so slow drift is followed automatically. The result
decodes signals at essentially any sensible audio frequency, clean or noisy,
without being told the tone in advance.

---

## Decoding many stations at once

By default the decoder follows only the **single strongest, confident tone** at
any moment. That is the right behaviour when operators take turns — each
transmission, on whatever pitch, is tracked as it becomes dominant, and weak side
peaks (which are mostly spectral leakage) are never decoded into garbage. A
single switch (`--multi` on the CLI, the *Simultaneous stations* checkbox in the
GUI) turns on full parallel decoding for genuinely overlapping signals.

In either mode morsw watches the whole band with a rolling FFT and compares each
candidate peak against a **median noise floor** (far more robust than a mean:
real signals barely move it, so a peak must stand well clear of the surrounding
noise to count). It applies non-maximum suppression so the spectral skirts of a
strong carrier do not spawn phantom stations, requires a new peak to persist
across a couple of analyses before creating a station, and gates each channel so
it only decodes while its own tone is present. The upshot is that idle
frequencies stay silent instead of filling with noise. Every decoded fragment is
stamped with the time it was sent; the GUI groups them into a per-station
**chat log** and an SDR-style scrollable **waterfall**, while the CLI's `morsec
stations FILE.wav` prints the per-station transcripts and a merged timeline.

## Interfacing with CW gear

The `cw` module (`include/morse/cw.h`) is the "talk to real radio things" layer:

- **Iambic keyer** — a Curtis mode A/B paddle state machine (`morse_iambic_*`).
- **Keyed text sender** — turns text into real-time key-down/up events through
  caller-supplied `key()` / `delay()` hooks, so the same code drives a serial
  line, a sidetone, a radio, or a test recorder.
- **Serial line keyer** — keys a transmitter via the RS-232 **RTS/DTR** control
  lines with PTT, cross-platform (POSIX termios / Win32), degrading to a clear
  "unsupported" status where serial is unavailable.
- **cwdaemon server** — a UDP server speaking the widely-used
  [cwdaemon](https://github.com/acerion/cwdaemon) request format (text, speed,
  tone, PTT, abort, reset), compatible with loggers and keyboards.
- **WinKeyer** — builds [K1EL WinKeyer](https://hamcrafters2.com/) host-mode
  command bytes (open, speed, sidetone, buffered text, close).

The pure protocol/keyer parts are deterministic and unit-tested; the serial and
UDP I/O are exercised through the CLI (`serial`, `cwdaemon`, `winkeyer`) and the
GUI's CW Interface tab.

---

## How it works

```
text ──encode──▶ pattern string ("... --- ...")
text ──encode──▶ timed element list ──synth──▶ PCM ──[wav]──▶ file
file/mic/loopback ──[wav/capture]──▶ PCM ──FFT pitch + Goertzel──▶ runs ──stream decode──▶ text
text ──cw──▶ key/PTT events ──▶ serial line / WinKeyer / cwdaemon / sidetone
```

The codebook is a `ds` **trie** (decode) plus a `ds` **hash table** (encode);
the timed element stream is a `ds` **list**; the detector uses a `ds` **circular
buffer** for samples and a `ds` **queue** for runs; output strings are `ds`
**str**. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full tour.

### The counting-allocator pattern

`ds` containers take an allocator pair through their `*_create_alloc`
constructors. `libmorse` passes its own instrumented `morse_xmalloc` /
`morse_xfree`, which carry a small aligned header on every block for exact byte
accounting. The header layout is identical whether or not diagnostics are
enabled, so turning instrumentation on never changes allocation behavior:

```c
morse_diagnostics_enable();
/* ... work ... */
morse_alloc_stats_t s;
if (morse_diagnostics_get(&s))
    printf("live=%zu peak=%zu allocs=%zu frees=%zu\n",
           s.bytes_live, s.bytes_peak, s.allocations, s.frees);
```

The tests assert the counters return to zero after every round-trip.

---

## Tests

Ten suites (`make check`, or `ctest`): timing math, codebook + variant gating,
encode/decode (prosigns, accents, policies, custom glyphs), per-sample
synthesis/detection bounds with clean and noisy signals, full
`text → WAV-on-disk → text` round-trips, the FFT and dominant-tone estimator,
multi-station separation (overlapping stations decoded in parallel, sequential
stations kept clean, broadband noise rejected, timeline ordering), the biquad
band-pass (in-band tones pass, out-of-band rejected), the SNR squelch (pure noise stays silent while clean signals decode), and the
CW keyer/protocol layer — together exercising tens of thousands of
assertions and checking the allocator balances to zero.

---

## License

MIT. See [`LICENSE`](./LICENSE). Dear ImGui, miniaudio, GLFW, and `hydrastro/ds`
are each under their own licenses.
