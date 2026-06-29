/*
 * morsec - command line front-end for the morse-deluxe core library.
 *
 * Subcommands:
 *   encode  TEXT...      UTF-8 text  -> dot/dash Morse
 *   decode  MORSE...     dot/dash Morse -> text
 *   wav     TEXT...      text -> 16-bit PCM WAV (-o file)
 *   listen  FILE.wav     WAV (any PCM/float) -> decoded text
 *   table                print the codebook as a reference chart
 *
 * With no positional argument, `encode`/`decode` read the payload from stdin,
 * which makes the tool pipe-friendly:  echo "SOS" | morsec encode
 */
#include "morse/morse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- shared options ------------------------------------------------------ */

typedef struct cli_opts {
  morse_variant_t variant;
  double wpm;
  double char_wpm; /* 0 = no Farnsworth */
  double weight;
  double tone_hz;
  unsigned int rate;
  double amplitude;
  double ramp_ms;
  double noise; /* 0 = none */
  char dit;
  char dah;
  const char *output;
  int diagnostics;
  /* CW interfacing (serial / cwdaemon / winkeyer) */
  const char *device;             /* serial device, e.g. /dev/ttyUSB0 or COM3 */
  morse_serial_keyline_t keyline; /* which control line keys the rig          */
  unsigned short port;            /* cwdaemon UDP port                        */
  int hex;                        /* winkeyer: print hex instead of raw bytes */
  int multi;                      /* stations: decode simultaneous tones      */
  int filter;                     /* stations: band-pass the audio first       */
} cli_opts;

static void opts_init(cli_opts *o) {
  o->variant = MORSE_VARIANT_INTERNATIONAL_EXT;
  o->wpm = 20.0;
  o->char_wpm = 0.0;
  o->weight = 0.5;
  o->tone_hz = 0.0; /* 0 = unset: synth uses 600 Hz, listen auto-detects */
  o->rate = 44100u;
  o->amplitude = 0.7;
  o->ramp_ms = 5.0;
  o->noise = 0.0;
  o->dit = '.';
  o->dah = '-';
  o->output = "out.wav";
  o->diagnostics = 0;
  o->device = NULL;
  o->keyline = MORSE_SERIAL_KEY_DTR;
  o->port = 6789u; /* cwdaemon default */
  o->hex = 0;
  o->multi = 0; /* default: follow the single strongest tone */
  o->filter = 0;
}

static void usage(FILE *f, const char *prog) {
  fprintf(f,
          "morse-deluxe CLI (morsec) %s\n\n"
          "Usage: %s <command> [options] [args]\n\n"
          "Commands:\n"
          "  encode TEXT...      Encode text to Morse (reads stdin if no TEXT)\n"
          "  decode MORSE...     Decode Morse to text  (reads stdin if none)\n"
          "  wav    TEXT...      Render text to a WAV file (use -o FILE)\n"
          "  listen FILE.wav     Decode Morse audio from a WAV file\n"
          "  stations FILE.wav   Decode stations from a WAV (strongest tone;\n"
          "                      --multi for simultaneous, --filter to band-pass)\n"
          "  table               Print the codebook reference chart\n"
          "  tree [depth]        Print the codebook as a dot/dash tree\n"
          "  serial TEXT...      Key TEXT out a serial port's RTS/DTR line\n"
          "  cwdaemon            Run a cwdaemon-compatible UDP keyer server\n"
          "  winkeyer TEXT...    Emit a K1EL WinKeyer host-mode byte stream\n\n"
          "Options:\n"
          "  -V, --variant base|ext  Codebook (default ext: punct+accents+prosigns)\n"
          "  -w, --wpm N             Words per minute (default 20)\n"
          "  -f, --farnsworth N      Character speed for Farnsworth spacing\n"
          "      --weight W          Dit/dah weight 0.25..0.75 (default 0.5)\n"
          "  -t, --tone HZ           Sidetone frequency for wav (default 600)\n"
          "  -r, --rate HZ           Sample rate for wav (default 44100)\n"
          "  -a, --amplitude A       Peak amplitude 0..1 (default 0.7)\n"
          "      --ramp MS           Envelope rise/fall ms (default 5)\n"
          "      --noise A           Mix white noise at amplitude A (wav)\n"
          "      --dit C             Dot character (default '.')\n"
          "      --dah C             Dash character (default '-')\n"
          "  -o, --output FILE       Output path for wav (default out.wav)\n"
          "      --diagnostics       Print allocation stats at exit\n"
          "  -D, --device DEV        Serial device for serial/cwdaemon/winkeyer\n"
          "      --keyline rts|dtr   Which line keys the rig (default dtr)\n"
          "      --port N            cwdaemon UDP port (default 6789)\n"
          "      --hex               winkeyer: print hex instead of raw bytes\n"
          "  -h, --help              This help\n"
          "      --version           Print version and exit\n\n"
          "Examples:\n"
          "  %s encode \"SOS\"\n"
          "  %s encode \"<AR> 73\" \n"
          "  echo \".... ..\" | %s decode\n"
          "  %s wav -w 25 -t 700 -o cq.wav \"CQ CQ DE K1ABC\"\n"
          "  %s listen cq.wav\n"
          "  %s serial -D /dev/ttyUSB0 --keyline rts -w 22 \"CQ TEST\"\n"
          "  %s winkeyer -w 22 \"CQ TEST\" > /dev/ttyUSB0\n"
          "  %s cwdaemon --port 6789 -D /dev/ttyUSB0\n",
          morse_version_string(), prog, prog, prog, prog, prog, prog, prog,
          prog, prog);
}

/* Join argv[start..argc) into one space-separated, heap-allocated string. */
static char *join_args(int argc, char **argv, int start) {
  size_t total = 1; /* NUL */
  int i;
  char *s;
  if (start >= argc) {
    return NULL;
  }
  for (i = start; i < argc; i++) {
    total += strlen(argv[i]) + 1;
  }
  s = (char *)malloc(total);
  if (s == NULL) {
    return NULL;
  }
  s[0] = '\0';
  for (i = start; i < argc; i++) {
    strcat(s, argv[i]);
    if (i + 1 < argc) {
      strcat(s, " ");
    }
  }
  return s;
}

/* Read all of stdin into a heap buffer (trimming a single trailing newline). */
static char *read_stdin(void) {
  size_t cap = 4096, len = 0;
  char *buf = (char *)malloc(cap);
  int c;
  if (buf == NULL) {
    return NULL;
  }
  while ((c = getchar()) != EOF) {
    if (len + 1 >= cap) {
      char *grown;
      cap *= 2;
      grown = (char *)realloc(buf, cap);
      if (grown == NULL) {
        free(buf);
        return NULL;
      }
      buf = grown;
    }
    buf[len++] = (char)c;
  }
  if (len > 0 && buf[len - 1] == '\n') {
    len--;
  }
  buf[len] = '\0';
  return buf;
}

static int parse_variant(const char *s, morse_variant_t *out) {
  if (strcmp(s, "base") == 0 || strcmp(s, "international") == 0) {
    *out = MORSE_VARIANT_INTERNATIONAL;
    return 0;
  }
  if (strcmp(s, "ext") == 0 || strcmp(s, "extended") == 0) {
    *out = MORSE_VARIANT_INTERNATIONAL_EXT;
    return 0;
  }
  return -1;
}

/* Returns index of first non-option argument, or -1 on a parse error. Fills
 * `o`. `need_value` style long/short options consume the next argv. */
static int parse_options(int argc, char **argv, int start, cli_opts *o) {
  int i = start;
  while (i < argc) {
    const char *a = argv[i];
    if (a[0] != '-' || strcmp(a, "-") == 0) {
      break; /* positional begins */
    }
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
#define NEEDVAL(name)                                                          \
  if (++i >= argc) {                                                           \
    fprintf(stderr, "%s: missing value\n", name);                             \
    return -1;                                                                 \
  }
    if (!strcmp(a, "-V") || !strcmp(a, "--variant")) {
      NEEDVAL(a);
      if (parse_variant(argv[i], &o->variant) != 0) {
        fprintf(stderr, "unknown variant '%s' (use base|ext)\n", argv[i]);
        return -1;
      }
    } else if (!strcmp(a, "-w") || !strcmp(a, "--wpm")) {
      NEEDVAL(a);
      o->wpm = atof(argv[i]);
    } else if (!strcmp(a, "-f") || !strcmp(a, "--farnsworth")) {
      NEEDVAL(a);
      o->char_wpm = atof(argv[i]);
    } else if (!strcmp(a, "--weight")) {
      NEEDVAL(a);
      o->weight = atof(argv[i]);
    } else if (!strcmp(a, "-t") || !strcmp(a, "--tone")) {
      NEEDVAL(a);
      o->tone_hz = atof(argv[i]);
    } else if (!strcmp(a, "-r") || !strcmp(a, "--rate")) {
      NEEDVAL(a);
      o->rate = (unsigned int)atoi(argv[i]);
    } else if (!strcmp(a, "-a") || !strcmp(a, "--amplitude")) {
      NEEDVAL(a);
      o->amplitude = atof(argv[i]);
    } else if (!strcmp(a, "--ramp")) {
      NEEDVAL(a);
      o->ramp_ms = atof(argv[i]);
    } else if (!strcmp(a, "--noise")) {
      NEEDVAL(a);
      o->noise = atof(argv[i]);
    } else if (!strcmp(a, "--dit")) {
      NEEDVAL(a);
      o->dit = argv[i][0];
    } else if (!strcmp(a, "--dah")) {
      NEEDVAL(a);
      o->dah = argv[i][0];
    } else if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
      NEEDVAL(a);
      o->output = argv[i];
    } else if (!strcmp(a, "--diagnostics")) {
      o->diagnostics = 1;
    } else if (!strcmp(a, "--device") || !strcmp(a, "-D")) {
      NEEDVAL(a);
      o->device = argv[i];
    } else if (!strcmp(a, "--keyline")) {
      NEEDVAL(a);
      if (!strcmp(argv[i], "rts")) {
        o->keyline = MORSE_SERIAL_KEY_RTS;
      } else if (!strcmp(argv[i], "dtr")) {
        o->keyline = MORSE_SERIAL_KEY_DTR;
      } else {
        fprintf(stderr, "keyline must be rts|dtr\n");
        return -1;
      }
    } else if (!strcmp(a, "--port")) {
      NEEDVAL(a);
      o->port = (unsigned short)atoi(argv[i]);
    } else if (!strcmp(a, "--hex")) {
      o->hex = 1;
    } else if (!strcmp(a, "--multi") || !strcmp(a, "--simultaneous")) {
      o->multi = 1;
    } else if (!strcmp(a, "--filter")) {
      o->filter = 1;
    } else {
      fprintf(stderr, "unknown option '%s'\n", a);
      return -1;
    }
#undef NEEDVAL
    i++;
  }
  return i;
}

static void fill_timing(const cli_opts *o, morse_timing_t *tm) {
  morse_timing_default(tm);
  tm->wpm = o->wpm;
  tm->char_wpm = o->char_wpm;
  tm->weight = o->weight;
}

static void fill_synth(const cli_opts *o, morse_synth_opts_t *so) {
  morse_synth_opts_default(so);
  so->sample_rate = o->rate;
  so->tone_hz = o->tone_hz > 0.0 ? o->tone_hz : 600.0;
  so->amplitude = o->amplitude;
  so->ramp_ms = o->ramp_ms;
  so->add_noise = o->noise > 0.0 ? 1 : 0;
  so->noise_amplitude = o->noise;
}

/* ---- commands ------------------------------------------------------------ */

static int cmd_encode(int argc, char **argv, int pos, const cli_opts *o) {
  char *text = (pos < argc) ? join_args(argc, argv, pos) : read_stdin();
  morse_table_t *t;
  morse_encode_opts_t eo;
  ds_str_t *out;
  morse_status_t st;
  if (text == NULL) {
    fprintf(stderr, "encode: no input\n");
    return 2;
  }
  t = morse_table_create(o->variant);
  morse_encode_opts_default(&eo);
  eo.dit = o->dit;
  eo.dah = o->dah;
  out = ds_str_create();
  st = morse_encode_string(t, text, &eo, out);
  if (st == MORSE_OK) {
    printf("%s\n", FUNC_str_cstr(out));
  } else {
    fprintf(stderr, "encode failed: %s\n", morse_status_str(st));
  }
  ds_str_destroy(out);
  morse_table_destroy(t);
  free(text);
  return st == MORSE_OK ? 0 : 1;
}

static int cmd_decode(int argc, char **argv, int pos, const cli_opts *o) {
  char *code = (pos < argc) ? join_args(argc, argv, pos) : read_stdin();
  morse_table_t *t;
  morse_decode_opts_t dopt;
  ds_str_t *out;
  morse_status_t st;
  if (code == NULL) {
    fprintf(stderr, "decode: no input\n");
    return 2;
  }
  t = morse_table_create(o->variant);
  morse_decode_opts_default(&dopt);
  dopt.dit = o->dit;
  dopt.dah = o->dah;
  out = ds_str_create();
  st = morse_decode_string(t, code, &dopt, out);
  if (st == MORSE_OK) {
    printf("%s\n", FUNC_str_cstr(out));
  } else {
    fprintf(stderr, "decode failed: %s\n", morse_status_str(st));
  }
  ds_str_destroy(out);
  morse_table_destroy(t);
  free(code);
  return st == MORSE_OK ? 0 : 1;
}

static int cmd_wav(int argc, char **argv, int pos, const cli_opts *o) {
  char *text = (pos < argc) ? join_args(argc, argv, pos) : read_stdin();
  morse_timing_t tm;
  morse_synth_opts_t so;
  morse_status_t st;
  if (text == NULL) {
    fprintf(stderr, "wav: no input\n");
    return 2;
  }
  fill_timing(o, &tm);
  fill_synth(o, &so);
  st = morse_text_to_wav(text, o->variant, &tm, &so, o->output);
  if (st == MORSE_OK) {
    printf("wrote %s\n", o->output);
  } else {
    fprintf(stderr, "wav failed: %s\n", morse_status_str(st));
  }
  free(text);
  return st == MORSE_OK ? 0 : 1;
}

static int cmd_listen(int argc, char **argv, int pos, const cli_opts *o) {
  morse_detect_opts_t det;
  ds_str_t *out;
  morse_status_t st;
  if (pos >= argc) {
    fprintf(stderr, "listen: expected a WAV file path\n");
    return 2;
  }
  morse_detect_opts_default(&det);
  if (o->tone_hz > 0.0) {
    det.tone_hz = o->tone_hz; /* hint; 0 keeps auto */
  }
  out = ds_str_create();
  st = morse_wav_to_text(argv[pos], o->variant, &det, out);
  if (st == MORSE_OK) {
    printf("%s\n", FUNC_str_cstr(out));
  } else {
    fprintf(stderr, "listen failed: %s\n", morse_status_str(st));
  }
  ds_str_destroy(out);
  return st == MORSE_OK ? 0 : 1;
}

/* Recursively print the dichotomic Morse tree: left branch = dot, right = dash. */
static void tree_node(const morse_table_t *t, const char *pat, int depth,
                      int maxdepth, const char *prefix, int is_last) {
  const morse_entry_t *e = NULL;
  size_t len = strlen(pat);
  char sym[16];
  char nextprefix[256];
  char child[32];

  if (len == 0) {
    printf("(start)\n");
  } else {
    if (morse_table_decode_pattern(t, pat, &e) == MORSE_OK && e != NULL) {
      snprintf(sym, sizeof(sym), "%s", e->name);
    } else {
      snprintf(sym, sizeof(sym), "\xc2\xb7"); /* middle dot for empty node */
    }
    printf("%s%s\xe2\x94\x80%c\xe2\x94\x80 %s\n", prefix,
           is_last ? "\xe2\x94\x94" : "\xe2\x94\x9c",
           pat[len - 1] == '.' ? '.' : '-', sym);
  }
  if (depth >= maxdepth) {
    return;
  }
  if (len == 0) {
    nextprefix[0] = '\0';
  } else {
    snprintf(nextprefix, sizeof(nextprefix), "%s%s", prefix,
             is_last ? "    " : "\xe2\x94\x82   ");
  }
  snprintf(child, sizeof(child), "%s.", pat);
  tree_node(t, child, depth + 1, maxdepth, nextprefix, 0);
  snprintf(child, sizeof(child), "%s-", pat);
  tree_node(t, child, depth + 1, maxdepth, nextprefix, 1);
}

static int cmd_tree(int argc, char **argv, int pos, const cli_opts *o) {
  morse_table_t *t = morse_table_create(o->variant);
  int maxdepth = 4; /* letters; pass a number for deeper (digits=5, punct=6) */
  if (pos < argc) {
    int d = atoi(argv[pos]);
    if (d >= 1 && d <= 7) {
      maxdepth = d;
    }
  }
  tree_node(t, "", 0, maxdepth, "", 1);
  printf("\n(left branch = dot, right branch = dash; depth %d)\n", maxdepth);
  morse_table_destroy(t);
  return 0;
}

static int cmd_stations(int argc, char **argv, int pos, const cli_opts *o) {
  morse_pcm_t pcm;
  morse_table_t *t;
  morse_multi_detector_t *md;
  morse_multi_opts_t mo;
  size_t i, nch;
  if (pos >= argc) {
    fprintf(stderr, "stations: expected a WAV file path\n");
    return 2;
  }
  morse_pcm_init(&pcm);
  if (morse_wav_read(argv[pos], &pcm) != MORSE_OK) {
    fprintf(stderr, "stations: could not read %s\n", argv[pos]);
    morse_pcm_free(&pcm);
    return 1;
  }
  t = morse_table_create(o->variant);
  morse_multi_opts_default(&mo);
  mo.max_active = o->multi ? 0 : 1; /* --multi decodes simultaneous stations */
  if (o->filter) {
    morse_filter_t flt;
    morse_filter_init(&flt, pcm.sample_rate);
    morse_filter_bandpass(&flt, mo.tone_min_hz, mo.tone_max_hz, 4);
    morse_filter_process(&flt, pcm.samples, pcm.samples, pcm.count);
  }
  md = morse_multi_create(t, pcm.sample_rate, &mo, NULL, NULL);
  if (md == NULL) {
    morse_table_destroy(t);
    morse_pcm_free(&pcm);
    return 1;
  }
  for (i = 0; i < pcm.count; i += 4096) {
    size_t step = pcm.count - i < 4096 ? pcm.count - i : 4096;
    morse_multi_process(md, pcm.samples + i, step);
  }
  morse_multi_finish(md);
  nch = morse_multi_channel_count(md);
  printf("%zu station(s) detected in %s:\n", nch, argv[pos]);
  for (i = 0; i < nch; ++i) {
    morse_multi_channel_info_t info;
    const char *txt = morse_multi_channel_text(md, i);
    if (morse_multi_get_channel(md, i, &info)) {
      printf("  [%4.0f Hz | %2.0f WPM] %s\n", info.tone_hz, info.wpm,
             txt != NULL ? txt : "");
    }
  }

  /* Chronological transcript across stations: merge consecutive fragments from
   * the same station (without a long pause) into one timestamped line. */
  {
    size_t ne = morse_multi_event_count(md);
    if (ne > 0) {
      size_t e = 0;
      printf("\ntimeline:\n");
      while (e < ne) {
        morse_multi_event_t ev;
        char line[256];
        size_t p = 0;
        int id;
        double hz, t0, lastT;
        if (!morse_multi_get_event(md, e, &ev)) {
          break;
        }
        id = ev.channel_id;
        hz = ev.tone_hz;
        t0 = ev.t_seconds;
        lastT = t0;
        line[0] = '\0';
        while (e < ne && morse_multi_get_event(md, e, &ev) &&
               ev.channel_id == id && ev.t_seconds - lastT < 3.0) {
          size_t k = 0;
          while (ev.text[k] != '\0' && p + 1 < sizeof(line)) {
            line[p++] = ev.text[k++];
          }
          line[p] = '\0';
          lastT = ev.t_seconds;
          ++e;
        }
        printf("  [%02d:%02d] %4.0f Hz  %s\n", (int)(t0 / 60.0), (int)t0 % 60,
               hz, line);
      }
    }
  }
  morse_multi_destroy(md);
  morse_table_destroy(t);
  morse_pcm_free(&pcm);
  return 0;
}

static const char *class_name(morse_entry_class_t k) {
  switch (k) {
  case MORSE_CLASS_LETTER:
    return "letter";
  case MORSE_CLASS_DIGIT:
    return "digit";
  case MORSE_CLASS_PUNCT:
    return "punct";
  case MORSE_CLASS_ACCENTED:
    return "accent";
  case MORSE_CLASS_PROSIGN:
    return "prosign";
  default:
    return "?";
  }
}

static int cmd_table(const cli_opts *o) {
  morse_table_t *t = morse_table_create(o->variant);
  size_t n = morse_table_size(t), i;
  printf("%-8s %-10s %-14s %s\n", "symbol", "class", "morse", "codepoint");
  printf("-------- ---------- -------------- ---------\n");
  for (i = 0; i < n; i++) {
    const morse_entry_t *e = morse_table_entry_at(t, i);
    if (e == NULL) {
      continue;
    }
    /* `name` is a UTF-8 label for normal chars and the mnemonic for prosigns. */
    if (e->codepoint != 0) {
      printf("%-8s %-10s %-14s U+%04X\n", e->name, class_name(e->klass),
             e->pattern, e->codepoint);
    } else {
      char tok[16];
      snprintf(tok, sizeof(tok), "<%s>", e->name);
      printf("%-8s %-10s %-14s %s\n", tok, class_name(e->klass), e->pattern,
             "-");
    }
  }
  printf("\n%zu entries.\n", n);
  morse_table_destroy(t);
  return 0;
}

/* ---- CW interfacing commands -------------------------------------------- */

/* Key text out a real serial port's RTS/DTR line (CW keying interface). */
static int cmd_serial(int argc, char **argv, int pos, const cli_opts *o) {
  char *text;
  morse_table_t *t;
  morse_timing_t tm;
  morse_durations_t dur;
  morse_status_t st;
  if (!morse_serial_supported()) {
    fprintf(stderr, "serial keying is not available in this build/platform\n");
    return 3;
  }
  if (o->device == NULL) {
    fprintf(stderr,
            "serial: --device required (e.g. /dev/ttyUSB0 or COM3)\n");
    return 2;
  }
  text = (pos < argc) ? join_args(argc, argv, pos) : read_stdin();
  if (text == NULL) {
    fprintf(stderr, "serial: no input\n");
    return 2;
  }
  t = morse_table_create(o->variant);
  fill_timing(o, &tm);
  morse_timing_resolve(&tm, &dur);
  fprintf(stderr, "keying \"%s\" on %s (%s) at %.0f WPM\n", text, o->device,
          o->keyline == MORSE_SERIAL_KEY_RTS ? "RTS" : "DTR", o->wpm);
  st = morse_serial_send_text(o->device, o->keyline, t, text, &dur);
  morse_table_destroy(t);
  free(text);
  if (st != MORSE_OK) {
    fprintf(stderr, "serial: %s\n", morse_status_str(st));
    return 1;
  }
  return 0;
}

/* Run a cwdaemon-compatible UDP server (the de-facto Linux CW keying daemon
 * protocol, spoken by loggers, keyboards and fldigi). */
static int cmd_cwdaemon(int argc, char **argv, int pos, const cli_opts *o) {
  morse_cwd_config_t cfg;
  morse_status_t st;
  (void)argc;
  (void)argv;
  (void)pos;
  if (!morse_cwd_server_supported()) {
    fprintf(stderr, "cwdaemon server is not available in this build/platform\n");
    return 3;
  }
  morse_cwd_config_default(&cfg);
  cfg.port = o->port;
  cfg.variant = o->variant;
  cfg.wpm = o->wpm;
  cfg.tone_hz = o->tone_hz; /* 0 => key only, no sidetone */
  cfg.serial_device = o->device; /* NULL => log received text */
  cfg.keyline = o->keyline;
  cfg.verbose = 1;
  fprintf(stderr, "cwdaemon-compatible server on UDP %u (Ctrl-C to stop)\n",
          (unsigned)cfg.port);
  if (cfg.serial_device != NULL) {
    fprintf(stderr, "keying serial device %s (%s)\n", cfg.serial_device,
            cfg.keyline == MORSE_SERIAL_KEY_RTS ? "RTS" : "DTR");
  }
  st = morse_cwd_serve(&cfg);
  if (st != MORSE_OK) {
    fprintf(stderr, "cwdaemon: %s\n", morse_status_str(st));
    return 1;
  }
  return 0;
}

/* Emit a K1EL WinKeyer host-mode byte stream for TEXT. By default the raw bytes
 * go to stdout (redirect to the keyer's serial port:
 *   morsec winkeyer -w 22 "CQ TEST" > /dev/ttyUSB0
 * ); with --hex they are printed as spaced hex for inspection. */
static int cmd_winkeyer(int argc, char **argv, int pos, const cli_opts *o) {
  char *text = (pos < argc) ? join_args(argc, argv, pos) : read_stdin();
  unsigned char buf[4096];
  size_t n = 0, k;
  if (text == NULL) {
    fprintf(stderr, "winkeyer: no input\n");
    return 2;
  }
  n += morse_winkeyer_open(buf + n, sizeof(buf) - n);
  n += morse_winkeyer_set_speed(buf + n, sizeof(buf) - n, (int)(o->wpm + 0.5));
  if (o->tone_hz > 0.0) {
    n += morse_winkeyer_set_sidetone(buf + n, sizeof(buf) - n, (int)o->tone_hz);
  }
  n += morse_winkeyer_text(buf + n, sizeof(buf) - n, text);
  n += morse_winkeyer_close(buf + n, sizeof(buf) - n);
  free(text);
  if (o->hex) {
    for (k = 0; k < n; k++) {
      printf("%02X%s", buf[k], (k + 1 < n) ? " " : "\n");
    }
  } else {
    fwrite(buf, 1, n, stdout);
  }
  return 0;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv) {
  cli_opts o;
  const char *cmd;
  int pos, rc;

  if (argc < 2) {
    usage(stderr, argv[0]);
    return 2;
  }
  cmd = argv[1];
  if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
    usage(stdout, argv[0]);
    return 0;
  }
  if (!strcmp(cmd, "--version")) {
    printf("morse-deluxe %s\n", morse_version_string());
    return 0;
  }

  opts_init(&o);
  pos = parse_options(argc, argv, 2, &o);
  if (pos < 0) {
    return 2;
  }
  if (o.diagnostics) {
    morse_diagnostics_enable();
  }

  if (!strcmp(cmd, "encode")) {
    rc = cmd_encode(argc, argv, pos, &o);
  } else if (!strcmp(cmd, "decode")) {
    rc = cmd_decode(argc, argv, pos, &o);
  } else if (!strcmp(cmd, "wav")) {
    rc = cmd_wav(argc, argv, pos, &o);
  } else if (!strcmp(cmd, "listen")) {
    rc = cmd_listen(argc, argv, pos, &o);
  } else if (!strcmp(cmd, "table")) {
    rc = cmd_table(&o);
  } else if (!strcmp(cmd, "tree")) {
    rc = cmd_tree(argc, argv, pos, &o);
  } else if (!strcmp(cmd, "stations")) {
    rc = cmd_stations(argc, argv, pos, &o);
  } else if (!strcmp(cmd, "serial")) {
    rc = cmd_serial(argc, argv, pos, &o);
  } else if (!strcmp(cmd, "cwdaemon")) {
    rc = cmd_cwdaemon(argc, argv, pos, &o);
  } else if (!strcmp(cmd, "winkeyer")) {
    rc = cmd_winkeyer(argc, argv, pos, &o);
  } else {
    fprintf(stderr, "unknown command '%s'\n\n", cmd);
    usage(stderr, argv[0]);
    return 2;
  }

  if (o.diagnostics) {
    morse_alloc_stats_t s;
    if (morse_diagnostics_get(&s)) {
      fprintf(stderr,
              "[diagnostics] allocs=%zu frees=%zu reallocs=%zu failed=%zu "
              "live=%zu peak=%zu total=%zu bytes\n",
              s.allocations, s.frees, s.reallocations, s.failed_allocations,
              s.bytes_live, s.bytes_peak, s.bytes_total);
    }
  }
  return rc;
}
