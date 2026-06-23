/*
 * cw.c - CW gear interfaces. See morse/cw.h.
 *
 * The iambic keyer, keyed-text sender, cwdaemon parser, and WinKeyer command
 * builder are pure and portable. The serial keyer and UDP server are wrapped
 * in platform guards and fall back to a clear "unsupported" status elsewhere.
 */

/* POSIX features (serial ioctls, sockets, nanosleep) before any include. */
#if !defined(_WIN32)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#include "morse/cw.h"

#include "morse/encode.h"

#include "ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* 1. Iambic keyer                                                          */
/* ======================================================================== */

void morse_iambic_init(morse_iambic_t *k, morse_iambic_mode_t mode,
                       double dit_ms) {
  if (!k) {
    return;
  }
  k->mode = mode;
  k->dit_ms = dit_ms > 0.0 ? dit_ms : 60.0;
  k->phase = MORSE_IAMBIC_IDLE;
  k->timer_ms = 0.0;
  k->elem = 0;
  k->last_elem = 1; /* so an opening squeeze starts on a dit */
  k->dit_latch = 0;
  k->dah_latch = 0;
  k->squeezed = 0;
  k->key = 0;
}

static void iambic_start_mark(morse_iambic_t *k, int elem) {
  k->phase = MORSE_IAMBIC_MARK;
  k->elem = elem;
  k->timer_ms = (elem == 1 ? 3.0 : 1.0) * k->dit_ms;
  k->key = 1;
  k->last_elem = elem;
}

/* Decide the next element at a phase boundary; -1 means go idle. */
static int iambic_choose(morse_iambic_t *k, int dit_p, int dah_p) {
  int dit = dit_p || k->dit_latch;
  int dah = dah_p || k->dah_latch;
  int next = -1;
  if (dit && dah) {
    next = (k->last_elem == 0) ? 1 : 0; /* squeeze alternation */
  } else if (dit) {
    next = 0;
  } else if (dah) {
    next = 1;
  } else if (k->mode == MORSE_IAMBIC_B && k->squeezed) {
    next = (k->last_elem == 0) ? 1 : 0; /* mode B trailing element */
    k->squeezed = 0;
  }
  if (next == 0) {
    k->dit_latch = 0;
  } else if (next == 1) {
    k->dah_latch = 0;
  } else {
    k->squeezed = 0;
  }
  return next;
}

int morse_iambic_tick(morse_iambic_t *k, int dit_paddle, int dah_paddle,
                      double dt_ms, int *out_element) {
  int completed = -1;
  double remaining = dt_ms;
  if (!k) {
    if (out_element) {
      *out_element = -1;
    }
    return 0;
  }
  if (dit_paddle) {
    k->dit_latch = 1;
  }
  if (dah_paddle) {
    k->dah_latch = 1;
  }
  if (dit_paddle && dah_paddle) {
    k->squeezed = 1;
  }

  while (remaining > 0.0) {
    if (k->phase == MORSE_IAMBIC_IDLE) {
      int next = iambic_choose(k, dit_paddle, dah_paddle);
      if (next < 0) {
        k->key = 0;
        break;
      }
      iambic_start_mark(k, next);
    }
    if (k->timer_ms > remaining) {
      k->timer_ms -= remaining;
      remaining = 0.0;
    } else {
      remaining -= k->timer_ms;
      k->timer_ms = 0.0;
      if (k->phase == MORSE_IAMBIC_MARK) {
        completed = k->elem;
        k->phase = MORSE_IAMBIC_GAP;
        k->timer_ms = k->dit_ms;
        k->key = 0;
      } else { /* GAP */
        int next = iambic_choose(k, dit_paddle, dah_paddle);
        if (next < 0) {
          k->phase = MORSE_IAMBIC_IDLE;
          k->key = 0;
        } else {
          iambic_start_mark(k, next);
        }
      }
    }
  }
  if (out_element) {
    *out_element = completed;
  }
  return k->key;
}

/* ======================================================================== */
/* 2. Keyed text sender                                                     */
/* ======================================================================== */

morse_status_t morse_cw_key_text(const morse_table_t *table, const char *text,
                                 const morse_durations_t *durations,
                                 morse_key_fn key, morse_delay_fn delay,
                                 void *user) {
  ds_list_t *els;
  ds_list_node_t *it;
  double total = 0.0;
  morse_status_t st;

  if (!table || !text || !durations || !key || !delay) {
    return MORSE_ERR_NULL;
  }
  els = ds_list_create();
  if (!els) {
    return MORSE_ERR_ALLOC;
  }
  st = morse_encode_elements(table, text, durations, MORSE_UNKNOWN_SKIP, els,
                             &total);
  if (st != MORSE_OK) {
    morse_elements_free(els);
    return st;
  }

  for (it = els->head; it != els->nil; it = it->next) {
    const morse_element_t *e = (const morse_element_t *)it;
    if (morse_symbol_is_mark(e->symbol)) {
      key(1, user);
      delay(e->duration_ms, user);
      key(0, user);
    } else {
      delay(e->duration_ms, user);
    }
  }
  morse_elements_free(els);
  return MORSE_OK;
}

/* ======================================================================== */
/* 3. cwdaemon UDP request protocol                                         */
/* ======================================================================== */

static int parse_int(const char *p, size_t len) {
  int sign = 1, val = 0;
  size_t i = 0;
  while (i < len && (p[i] == ' ' || p[i] == '\t')) {
    i++;
  }
  if (i < len && (p[i] == '+' || p[i] == '-')) {
    sign = (p[i] == '-') ? -1 : 1;
    i++;
  }
  for (; i < len && p[i] >= '0' && p[i] <= '9'; i++) {
    val = val * 10 + (p[i] - '0');
  }
  return sign * val;
}

morse_status_t morse_cwd_parse(const char *buf, size_t len,
                               morse_cwd_msg_t *out) {
  if (!buf || !out) {
    return MORSE_ERR_NULL;
  }
  out->ival = 0;
  out->text = NULL;
  out->text_len = 0;

  if (len >= 1 && (unsigned char)buf[0] == 0x1B /* ESC */) {
    char cmd = (len >= 2) ? buf[1] : '\0';
    const char *arg = buf + 2;
    size_t arglen = (len >= 2) ? len - 2 : 0;
    switch (cmd) {
    case '0':
      out->kind = MORSE_CWD_RESET;
      break;
    case '2':
      out->kind = MORSE_CWD_SPEED;
      out->ival = parse_int(arg, arglen);
      break;
    case '3':
      out->kind = MORSE_CWD_TONE;
      out->ival = parse_int(arg, arglen);
      break;
    case '4':
      out->kind = MORSE_CWD_ABORT;
      break;
    case '5':
      out->kind = MORSE_CWD_EXIT;
      break;
    case 'a':
      out->kind = MORSE_CWD_PTT;
      out->ival = parse_int(arg, arglen) ? 1 : 0;
      break;
    default:
      out->kind = MORSE_CWD_UNKNOWN;
      break;
    }
    return MORSE_OK;
  }

  /* Literal text: strip a single trailing CR/LF for convenience. */
  out->kind = MORSE_CWD_TEXT;
  out->text = buf;
  out->text_len = len;
  while (out->text_len > 0 && (buf[out->text_len - 1] == '\n' ||
                              buf[out->text_len - 1] == '\r')) {
    out->text_len--;
  }
  return MORSE_OK;
}

/* ======================================================================== */
/* 4. WinKeyer (K1EL) host-mode command builder                             */
/* ======================================================================== */

size_t morse_winkeyer_open(unsigned char *buf, size_t cap) {
  if (!buf || cap < 2) {
    return 0;
  }
  buf[0] = 0x00; /* admin */
  buf[1] = 0x02; /* host open */
  return 2;
}

size_t morse_winkeyer_set_speed(unsigned char *buf, size_t cap, int wpm) {
  if (!buf || cap < 2) {
    return 0;
  }
  if (wpm < 5) {
    wpm = 5;
  }
  if (wpm > 99) {
    wpm = 99;
  }
  buf[0] = 0x02; /* set WPM speed */
  buf[1] = (unsigned char)wpm;
  return 2;
}

size_t morse_winkeyer_set_sidetone(unsigned char *buf, size_t cap, int hz) {
  int n;
  if (!buf || cap < 2) {
    return 0;
  }
  /* WinKeyer sidetone index n maps to 4000/n Hz, n in 1..10. Pick nearest. */
  if (hz < 400) {
    hz = 400;
  }
  if (hz > 4000) {
    hz = 4000;
  }
  n = (int)((4000.0 / (double)hz) + 0.5);
  if (n < 1) {
    n = 1;
  }
  if (n > 10) {
    n = 10;
  }
  buf[0] = 0x01; /* sidetone control */
  buf[1] = (unsigned char)n;
  return 2;
}

size_t morse_winkeyer_close(unsigned char *buf, size_t cap) {
  if (!buf || cap < 2) {
    return 0;
  }
  buf[0] = 0x00; /* admin */
  buf[1] = 0x03; /* host close */
  return 2;
}

size_t morse_winkeyer_text(unsigned char *buf, size_t cap, const char *text) {
  size_t n;
  if (!buf || !text) {
    return 0;
  }
  n = strlen(text);
  if (n > cap) {
    return 0;
  }
  memcpy(buf, text, n);
  return n;
}

/* ======================================================================== */
/* Real-time delay used by the blocking I/O helpers                         */
/* ======================================================================== */

#if defined(_WIN32)
#  include <windows.h>
static void rt_delay(double ms, void *user) {
  (void)user;
  if (ms < 0.0) {
    ms = 0.0;
  }
  Sleep((DWORD)(ms + 0.5));
}
#else
#  include <time.h>
static void rt_delay(double ms, void *user) {
  struct timespec ts;
  (void)user;
  if (ms < 0.0) {
    ms = 0.0;
  }
  ts.tv_sec = (time_t)(ms / 1000.0);
  ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1.0e6);
  nanosleep(&ts, NULL);
}
#endif

/* ======================================================================== */
/* Serial line keyer (guarded)                                              */
/* ======================================================================== */

#if defined(__unix__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <sys/ioctl.h>
#  include <termios.h>
#  include <unistd.h>
#  define MORSE_HAVE_SERIAL 1

struct morse_serial {
  int fd;
  int key_bit; /* TIOCM_DTR or TIOCM_RTS */
  int ptt_bit;
};

int morse_serial_supported(void) { return 1; }

morse_serial_t *morse_serial_open(const char *device,
                                  morse_serial_keyline_t keyline) {
  morse_serial_t *s;
  int fd;
  if (!device) {
    return NULL;
  }
  fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    return NULL;
  }
  s = (morse_serial_t *)malloc(sizeof(*s));
  if (!s) {
    close(fd);
    return NULL;
  }
  s->fd = fd;
  if (keyline == MORSE_SERIAL_KEY_RTS) {
    s->key_bit = TIOCM_RTS;
    s->ptt_bit = TIOCM_DTR;
  } else {
    s->key_bit = TIOCM_DTR;
    s->ptt_bit = TIOCM_RTS;
  }
  /* de-assert both lines on open */
  {
    int bits = s->key_bit | s->ptt_bit;
    ioctl(fd, TIOCMBIC, &bits);
  }
  return s;
}

void morse_serial_close(morse_serial_t *s) {
  if (!s) {
    return;
  }
  {
    int bits = s->key_bit | s->ptt_bit;
    ioctl(s->fd, TIOCMBIC, &bits);
  }
  close(s->fd);
  free(s);
}

static morse_status_t serial_line(morse_serial_t *s, int bit, int on) {
  if (!s) {
    return MORSE_ERR_NULL;
  }
  if (ioctl(s->fd, on ? TIOCMBIS : TIOCMBIC, &bit) != 0) {
    return MORSE_ERR_IO;
  }
  return MORSE_OK;
}

morse_status_t morse_serial_key(morse_serial_t *s, int on) {
  return serial_line(s, s ? s->key_bit : 0, on);
}
morse_status_t morse_serial_ptt(morse_serial_t *s, int on) {
  return serial_line(s, s ? s->ptt_bit : 0, on);
}

#elif defined(_WIN32)
#  include <windows.h>
#  define MORSE_HAVE_SERIAL 1

struct morse_serial {
  HANDLE h;
  int key_set, key_clr, ptt_set, ptt_clr;
};

int morse_serial_supported(void) { return 1; }

morse_serial_t *morse_serial_open(const char *device,
                                  morse_serial_keyline_t keyline) {
  morse_serial_t *s;
  HANDLE h;
  if (!device) {
    return NULL;
  }
  h = CreateFileA(device, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                  0, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    return NULL;
  }
  s = (morse_serial_t *)malloc(sizeof(*s));
  if (!s) {
    CloseHandle(h);
    return NULL;
  }
  s->h = h;
  if (keyline == MORSE_SERIAL_KEY_RTS) {
    s->key_set = SETRTS;
    s->key_clr = CLRRTS;
    s->ptt_set = SETDTR;
    s->ptt_clr = CLRDTR;
  } else {
    s->key_set = SETDTR;
    s->key_clr = CLRDTR;
    s->ptt_set = SETRTS;
    s->ptt_clr = CLRRTS;
  }
  EscapeCommFunction(h, s->key_clr);
  EscapeCommFunction(h, s->ptt_clr);
  return s;
}

void morse_serial_close(morse_serial_t *s) {
  if (!s) {
    return;
  }
  EscapeCommFunction(s->h, s->key_clr);
  EscapeCommFunction(s->h, s->ptt_clr);
  CloseHandle(s->h);
  free(s);
}

morse_status_t morse_serial_key(morse_serial_t *s, int on) {
  if (!s) {
    return MORSE_ERR_NULL;
  }
  return EscapeCommFunction(s->h, on ? s->key_set : s->key_clr) ? MORSE_OK
                                                                : MORSE_ERR_IO;
}
morse_status_t morse_serial_ptt(morse_serial_t *s, int on) {
  if (!s) {
    return MORSE_ERR_NULL;
  }
  return EscapeCommFunction(s->h, on ? s->ptt_set : s->ptt_clr) ? MORSE_OK
                                                                : MORSE_ERR_IO;
}

#else /* no serial support */

struct morse_serial {
  int unused;
};
int morse_serial_supported(void) { return 0; }
morse_serial_t *morse_serial_open(const char *device,
                                  morse_serial_keyline_t keyline) {
  (void)device;
  (void)keyline;
  return NULL;
}
void morse_serial_close(morse_serial_t *s) { (void)s; }
morse_status_t morse_serial_key(morse_serial_t *s, int on) {
  (void)s;
  (void)on;
  return MORSE_ERR_UNSUPPORTED;
}
morse_status_t morse_serial_ptt(morse_serial_t *s, int on) {
  (void)s;
  (void)on;
  return MORSE_ERR_UNSUPPORTED;
}
#endif

/* Shared key callback bridging the sender to a serial handle. */
static void serial_key_cb(int on, void *user) {
  morse_serial_key((morse_serial_t *)user, on);
}

morse_status_t morse_serial_send_text(const char *device,
                                      morse_serial_keyline_t keyline,
                                      const morse_table_t *table,
                                      const char *text,
                                      const morse_durations_t *durations) {
#if defined(MORSE_HAVE_SERIAL)
  morse_serial_t *s;
  morse_status_t st;
  if (!device || !table || !text || !durations) {
    return MORSE_ERR_NULL;
  }
  s = morse_serial_open(device, keyline);
  if (!s) {
    return MORSE_ERR_IO;
  }
  morse_serial_ptt(s, 1);
  rt_delay(20.0, NULL); /* let PTT settle */
  st = morse_cw_key_text(table, text, durations, serial_key_cb, rt_delay, s);
  rt_delay(10.0, NULL);
  morse_serial_ptt(s, 0);
  morse_serial_close(s);
  return st;
#else
  (void)device;
  (void)keyline;
  (void)table;
  (void)text;
  (void)durations;
  return MORSE_ERR_UNSUPPORTED;
#endif
}

/* ======================================================================== */
/* cwdaemon-compatible UDP server (guarded)                                 */
/* ======================================================================== */

void morse_cwd_config_default(morse_cwd_config_t *cfg) {
  if (!cfg) {
    return;
  }
  cfg->port = 6789;
  cfg->variant = MORSE_VARIANT_INTERNATIONAL_EXT;
  cfg->wpm = 24.0;
  cfg->tone_hz = 700.0;
  cfg->serial_device = NULL;
  cfg->keyline = MORSE_SERIAL_KEY_DTR;
  cfg->verbose = 1;
}

#if defined(__unix__) || defined(__APPLE__)
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define MORSE_HAVE_UDP 1
#elif defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define MORSE_HAVE_UDP 1
#endif

int morse_cwd_server_supported(void) {
#if defined(MORSE_HAVE_UDP)
  return 1;
#else
  return 0;
#endif
}

morse_status_t morse_cwd_serve(const morse_cwd_config_t *cfg) {
#if defined(MORSE_HAVE_UDP)
  morse_cwd_config_t def;
  morse_table_t *table;
  morse_timing_t tm;
  morse_durations_t dur;
  morse_serial_t *serial = NULL;
  char buf[2048];
  int running = 1;
  int sock;
  struct sockaddr_in addr;

  if (!cfg) {
    morse_cwd_config_default(&def);
    cfg = &def;
  }

#  if defined(_WIN32)
  {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      return MORSE_ERR_IO;
    }
  }
#  endif

  sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    return MORSE_ERR_IO;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(cfg->port);
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#  if defined(_WIN32)
    closesocket(sock);
    WSACleanup();
#  else
    close(sock);
#  endif
    return MORSE_ERR_IO;
  }

  table = morse_table_create(cfg->variant);
  if (!table) {
#  if defined(_WIN32)
    closesocket(sock);
    WSACleanup();
#  else
    close(sock);
#  endif
    return MORSE_ERR_ALLOC;
  }
  morse_timing_default(&tm);
  tm.wpm = cfg->wpm;
  morse_timing_resolve(&tm, &dur);

  if (cfg->serial_device && morse_serial_supported()) {
    serial = morse_serial_open(cfg->serial_device, cfg->keyline);
    if (cfg->verbose && !serial) {
      fprintf(stderr, "cwd: could not open serial device %s\n",
              cfg->serial_device);
    }
  }
  if (cfg->verbose) {
    fprintf(stderr, "cwd: listening on UDP %u (%.0f wpm)\n",
            (unsigned)cfg->port, cfg->wpm);
  }

  while (running) {
    morse_cwd_msg_t msg;
#  if defined(_WIN32)
    int n = recvfrom(sock, buf, (int)sizeof(buf) - 1, 0, NULL, NULL);
#  else
    ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
#  endif
    if (n <= 0) {
      continue;
    }
    if (morse_cwd_parse(buf, (size_t)n, &msg) != MORSE_OK) {
      continue;
    }
    switch (msg.kind) {
    case MORSE_CWD_TEXT: {
      char tmp[2048];
      size_t m = msg.text_len < sizeof(tmp) - 1 ? msg.text_len : sizeof(tmp) - 1;
      memcpy(tmp, msg.text, m);
      tmp[m] = '\0';
      if (cfg->verbose) {
        fprintf(stderr, "cwd: send \"%s\"\n", tmp);
      }
      if (serial) {
        morse_serial_ptt(serial, 1);
        rt_delay(20.0, NULL);
        morse_cw_key_text(table, tmp, &dur, serial_key_cb, rt_delay, serial);
        morse_serial_ptt(serial, 0);
      }
      break;
    }
    case MORSE_CWD_SPEED:
      if (msg.ival >= 5 && msg.ival <= 120) {
        tm.wpm = msg.ival;
        morse_timing_resolve(&tm, &dur);
        if (cfg->verbose) {
          fprintf(stderr, "cwd: speed %d wpm\n", msg.ival);
        }
      }
      break;
    case MORSE_CWD_TONE:
      if (cfg->verbose) {
        fprintf(stderr, "cwd: tone %d Hz\n", msg.ival);
      }
      break;
    case MORSE_CWD_PTT:
      if (serial) {
        morse_serial_ptt(serial, msg.ival);
      }
      break;
    case MORSE_CWD_ABORT:
      if (serial) {
        morse_serial_key(serial, 0);
      }
      break;
    case MORSE_CWD_RESET:
      morse_timing_default(&tm);
      tm.wpm = cfg->wpm;
      morse_timing_resolve(&tm, &dur);
      break;
    case MORSE_CWD_EXIT:
      running = 0;
      break;
    default:
      break;
    }
  }

  if (serial) {
    morse_serial_close(serial);
  }
  morse_table_destroy(table);
#  if defined(_WIN32)
  closesocket(sock);
  WSACleanup();
#  else
  close(sock);
#  endif
  return MORSE_OK;
#else
  (void)cfg;
  return MORSE_ERR_UNSUPPORTED;
#endif
}
