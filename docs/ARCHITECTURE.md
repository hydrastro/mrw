# Architecture

This document goes a level deeper than the README on how the core library is
put together and why. The whole core is pure C11 and depends only on
`hydrastro/ds` and the C math library.

## Layering

```
            ┌─────────────────────────────────────────────┐
   public   │ morse.h  (umbrella: version, one-shot WAV    │
   API      │           helpers, diagnostics)              │
            ├───────┬───────┬───────┬───────┬───────┬──────┤
            │types  │table  │timing │encode │decode │ ...  │
            │       │       │       │synth  │detect │ wav  │
            └───────┴───┬───┴───────┴───┬───┴───────┴──────┘
                        │               │
            ┌───────────▼───┐   ┌───────▼──────────┐
   private  │ morse_alloc   │   │ hydrastro/ds      │
            │ (counting     │──▶│ trie, hash table, │
            │  allocator)   │   │ list, queue,      │
            └───────────────┘   │ circular buffer,  │
                                │ str               │
                                └───────────────────┘
```

Every public type and function is prefixed `morse_` and every public header is
wrapped in `extern "C"` so the C++ GUI can include them directly. The counting
allocator and the codebook tables live in `src/` and are not installed.

## Status and error handling

All fallible functions return a `morse_status_t`. `MORSE_OK` is `0`; errors are
negative (`MORSE_ERR_NULL`, `MORSE_ERR_ALLOC`, `MORSE_ERR_RANGE`,
`MORSE_ERR_UNKNOWN_SYMBOL`, `MORSE_ERR_BAD_PATTERN`, `MORSE_ERR_IO`,
`MORSE_ERR_FORMAT`, `MORSE_ERR_STATE`, `MORSE_ERR_UNSUPPORTED`).
`morse_status_str()` maps any code to a short string.

## The codebook (`table`)

Two `ds` containers back the codebook, built once per `morse_table_t`:

- **Decode**: a **trie** whose edges are `.` and `-`. Walking the pattern lands
  on the entry for that symbol. Prosigns are deliberately *excluded* from the
  trie — they are ambiguous as receive symbols and exist only for sending.
- **Encode**: a **hash table** keyed by Unicode codepoint, value is the entry
  (with its pattern string). Lookup is O(1) amortized.

The static entry arrays are split by class — 26 letters, 10 digits, 18
punctuation marks, 11 accented letters (codepoints `U+00C0`–`U+00DE`), and 11
prosigns. The base variant exposes only letters + digits (36 entries); the
extended variant adds the rest (76 entries). Prosigns carry codepoint `0` and
are addressed by name (`morse_table_lookup_prosign`, case-insensitive).

## Timing

`morse_timing_resolve()` converts a `morse_timing_t { wpm, char_wpm, weight }`
into a `morse_durations_t` of five element lengths:

- The **dit** is `1200 / wpm` milliseconds (the PARIS standard: the word
  "PARIS " is exactly 50 dit units).
- **Farnsworth**: when `char_wpm > wpm`, characters are sent fast (`char_wpm`)
  but the inter-character and inter-word gaps are stretched so the *overall*
  rate matches `wpm`, using the ARRL 3:7 ratio for character:word spacing.
- **Weight** redistributes time between the mark and the intra-character gap
  while holding the dot period constant, so a heavier weight sounds "fatter"
  without changing speed. `wpm`/`char_wpm` are clamped to `[1,120]` and weight
  to `[0.25, 0.75]`.

## Encoding

`morse_encode_string()` produces a printable pattern with configurable dot/dash
glyphs and element/letter/word separators. `morse_encode_elements()` instead
appends a sequence of **timed elements** to a caller-owned `ds` list. Each
`morse_element_t` embeds a `list_node_t` as its first member (the intrusive
pattern) and records its symbol, `start_ms`, and `duration_ms`. Unknown input
characters are handled by policy: skip, replace, or fail.

## Synthesis

`morse_synth_render()` walks the element list and writes float samples. Marks
become a sine wave at `tone_hz`; gaps are silence. Each mark is multiplied by a
**raised-cosine (Hann) envelope** over `ramp_ms` at onset and release to avoid
the audible click of a hard gate, and the oscillator phase is carried across
elements so there are no discontinuities. Optional white noise (a fast xorshift
generator) can be mixed in at a chosen amplitude for realistic practice audio.

## Detection

Recovering Morse from audio is the most involved stage:

1. **Goertzel filter** — for each block of `block_size` samples, a single-bin
   Goertzel computes the power at the tone frequency far more cheaply than a
   full FFT. If `tone_hz` is `0`, the detector first scans candidate bins over a
   plausible range (≈300–1000 Hz) and locks onto the strongest.
2. **Normalization** — block power is normalized against an adaptive noise floor
   tracked with an exponential moving average, yielding a 0..1 envelope that is
   robust to overall level.
3. **Hysteresis** — a Schmitt-trigger-style pair of thresholds (an upper trip
   and a lower trip at a fraction of it) converts the envelope into clean
   mark/space transitions without chattering on noisy edges.
4. **Runs → symbols** — consecutive same-state blocks become timed runs, which
   feed the streaming decoder.

`morse_detect_pcm()` does this offline over a whole buffer and can also hand back
the full envelope for plotting. `morse_detector_t` is the streaming front-end
for live microphone input, backed by a `ds` **circular buffer** for samples and
a `ds` **queue** for pending runs, emitting decoded text through a sink callback
and exposing the latest tone power for a live meter.

## Adaptive streaming decode

The streaming decoder never needs to be told the speed. It keeps an exponential
estimate of the dit length from the marks it has seen and classifies each run
relative to that estimate (dot vs. dash; intra / character / word gap). As the
sender speeds up or slows down, the estimate follows, so the same decoder
handles a steady machine and a wobbly human fist. `morse_stream_decoder_wpm()`
and `morse_stream_decoder_dit_ms()` expose the current estimate.

## WAV I/O

`wav.c` is a small, dependency-free RIFF/WAVE implementation. The writer emits
canonical mono 16-bit PCM. The reader is liberal: it accepts 8/16/24/32-bit
integer and 32-bit float sample formats and any channel count, down-mixing to
mono float for the detector. This is enough to interoperate with audio produced
by other tools (or transcoded in by ffmpeg in the GUI) without pulling in a
heavyweight codec dependency.

## Memory accounting

See the README section on the counting allocator. The key design point is that
the per-block size header is present in *both* instrumented and
non-instrumented builds, so enabling diagnostics is purely additive and never
changes allocation layout or behavior. Each `ds` container is created with the
matching allocator/deallocator pair so its nodes are counted and freed
consistently. The round-trip tests assert the counters return to zero.
