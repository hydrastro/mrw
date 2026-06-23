/*
 * morse/cw.h - Interfaces to real CW (Continuous Wave) gear and protocols.
 *
 * This is the "talk to actual radio things" layer. It has four parts, three of
 * which are pure, deterministic, and unit-tested, and one of which is guarded
 * OS I/O:
 *
 *   1. Iambic keyer        - a Curtis mode A/B paddle keyer state machine.
 *   2. Keyed text sender   - turn text into real-time key-up/key-down events
 *                            through caller-supplied key() and delay() hooks
 *                            (so it drives a serial line, a sidetone, a radio,
 *                            or a test recorder with equal ease).
 *   3. cwdaemon protocol   - parse the UDP request format used by cwdaemon and
 *                            its many clients (keyboards, loggers, fldigi).
 *   4. WinKeyer protocol   - build K1EL WinKeyer host-mode command bytes.
 *
 * Plus a small cross-platform serial keyer (RTS/DTR line keying + PTT) and an
 * optional cwdaemon-compatible UDP server, both of which degrade to a clear
 * "unsupported on this platform" status rather than failing to compile.
 */
#ifndef MORSE_CW_H
#define MORSE_CW_H

#include "morse/table.h"
#include "morse/timing.h"
#include "morse/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* 1. Iambic keyer                                                          */
/* ======================================================================== */

typedef enum {
  MORSE_IAMBIC_A = 0, /* dit/dah memory ends cleanly when paddles released   */
  MORSE_IAMBIC_B = 1  /* one extra alternate element after a squeeze release */
} morse_iambic_mode_t;

typedef enum {
  MORSE_IAMBIC_IDLE = 0,
  MORSE_IAMBIC_MARK = 1, /* key down for the current element                 */
  MORSE_IAMBIC_GAP = 2   /* the one-dit inter-element space                  */
} morse_iambic_phase_t;

/*
 * Transparent so callers/tests can inspect it, but treat the fields as opaque
 * outside of init/tick. One unit = `dit_ms`; a dah is three units; every
 * element is followed by one unit of key-up.
 */
typedef struct morse_iambic {
  morse_iambic_mode_t mode;
  double dit_ms;
  morse_iambic_phase_t phase;
  double timer_ms;   /* time left in the current phase                       */
  int elem;          /* element in progress: 0 = dit, 1 = dah                */
  int last_elem;     /* last element actually sent (for squeeze alternation) */
  int dit_latch;     /* paddle memory                                        */
  int dah_latch;
  int squeezed;      /* both paddles were down during the current element    */
  int key;           /* current key output, 0/1                             */
} morse_iambic_t;

void morse_iambic_init(morse_iambic_t *k, morse_iambic_mode_t mode,
                       double dit_ms);

/*
 * Advance the keyer by `dt_ms` with the current paddle inputs and return the
 * key output (0/1). If an element *completes* on this tick, `*out_element` (if
 * non-NULL) is set to 0 for a dit or 1 for a dah; otherwise it is set to -1.
 * Use small steps (a millisecond or two) for accurate timing.
 */
int morse_iambic_tick(morse_iambic_t *k, int dit_paddle, int dah_paddle,
                      double dt_ms, int *out_element);

/* ======================================================================== */
/* 2. Keyed text sender                                                     */
/* ======================================================================== */

/* Called to set the key/transmitter line. `on` is 1 for key-down, 0 for up. */
typedef void (*morse_key_fn)(int on, void *user);
/* Called to wait. Implementations sleep (real) or just record (test). */
typedef void (*morse_delay_fn)(double ms, void *user);

/*
 * Encode `text` with `table` at the supplied element `durations`, then play it
 * out as a sequence of key()/delay() calls: key(1)/delay(mark)/key(0) for each
 * dit or dah, delay() for each gap. Returns MORSE_OK or an encode error. With a
 * recording key/delay this is fully deterministic and testable; with a real
 * serial keyer and a sleeping delay it transmits.
 */
morse_status_t morse_cw_key_text(const morse_table_t *table, const char *text,
                                 const morse_durations_t *durations,
                                 morse_key_fn key, morse_delay_fn delay,
                                 void *user);

/* ======================================================================== */
/* 3. cwdaemon UDP request protocol                                         */
/* ======================================================================== */

typedef enum {
  MORSE_CWD_TEXT = 0,   /* plain text to send as CW                          */
  MORSE_CWD_SPEED,      /* set keying speed, ival = WPM                       */
  MORSE_CWD_TONE,       /* set sidetone, ival = Hz (0 disables tone)         */
  MORSE_CWD_ABORT,      /* abort the message in progress                     */
  MORSE_CWD_PTT,        /* set PTT, ival = 0/1                               */
  MORSE_CWD_RESET,      /* reset to defaults                                 */
  MORSE_CWD_EXIT,       /* stop the server                                   */
  MORSE_CWD_UNKNOWN     /* an escape we do not implement                      */
} morse_cwd_kind_t;

typedef struct morse_cwd_msg {
  morse_cwd_kind_t kind;
  int ival;            /* numeric argument for SPEED/TONE/PTT                */
  const char *text;    /* for TEXT: points into the caller's buffer         */
  size_t text_len;
} morse_cwd_msg_t;

/*
 * Parse one cwdaemon datagram. The format: a datagram beginning with ESC
 * (0x1B) is a command whose next byte selects the action (e.g. ESC '2' <n> set
 * speed, ESC '3' <n> set tone, ESC '4' abort, ESC 'a' <0/1> PTT, ESC '0'
 * reset); any other datagram is literal text to transmit. `buf` need not be
 * NUL-terminated. Returns MORSE_OK on a recognised message (kind may be
 * UNKNOWN for unimplemented escapes).
 */
morse_status_t morse_cwd_parse(const char *buf, size_t len,
                               morse_cwd_msg_t *out);

/* ======================================================================== */
/* 4. WinKeyer (K1EL) host-mode command builder                             */
/* ======================================================================== */

/* Bytes that open the host interface (admin: host open). */
size_t morse_winkeyer_open(unsigned char *buf, size_t cap);
/* Set keyer speed in WPM (clamped to the WinKeyer 5..99 range). */
size_t morse_winkeyer_set_speed(unsigned char *buf, size_t cap, int wpm);
/* Set sidetone; `hz` is mapped to the nearest WinKeyer tone index. */
size_t morse_winkeyer_set_sidetone(unsigned char *buf, size_t cap, int hz);
/* Close/clear the host interface. */
size_t morse_winkeyer_close(unsigned char *buf, size_t cap);
/*
 * Append `text` to `buf` as the literal bytes WinKeyer transmits. Returns the
 * number of bytes written (0 if it does not fit). Buffered text plus the
 * commands above form a complete host-mode session.
 */
size_t morse_winkeyer_text(unsigned char *buf, size_t cap, const char *text);

/* ======================================================================== */
/* Serial line keyer (guarded OS I/O)                                       */
/* ======================================================================== */

/* Which control line drives which function. */
typedef enum {
  MORSE_SERIAL_KEY_DTR = 0, /* key on DTR, PTT on RTS (common default)       */
  MORSE_SERIAL_KEY_RTS = 1  /* key on RTS, PTT on DTR                        */
} morse_serial_keyline_t;

typedef struct morse_serial morse_serial_t; /* opaque */

/* True if this build can actually drive a serial port on this platform. */
int morse_serial_supported(void);

/*
 * Open a serial device (e.g. "/dev/ttyUSB0" or "COM3") for line keying. The
 * lines are de-asserted on open. Returns NULL on error (including on platforms
 * where serial keying is unsupported - check morse_serial_supported()).
 */
morse_serial_t *morse_serial_open(const char *device,
                                  morse_serial_keyline_t keyline);
void morse_serial_close(morse_serial_t *s);
morse_status_t morse_serial_key(morse_serial_t *s, int on);  /* keying line  */
morse_status_t morse_serial_ptt(morse_serial_t *s, int on);  /* PTT line     */

/*
 * Convenience: open `device`, key out `text` at `durations` in real time
 * (sleeping between elements), then close. Honors PTT (asserted around the
 * transmission). Returns MORSE_ERR_UNSUPPORTED where serial is unavailable.
 */
morse_status_t morse_serial_send_text(const char *device,
                                      morse_serial_keyline_t keyline,
                                      const morse_table_t *table,
                                      const char *text,
                                      const morse_durations_t *durations);

/* ======================================================================== */
/* cwdaemon-compatible UDP server (guarded OS I/O)                          */
/* ======================================================================== */

/* True if this build can host a UDP server on this platform. */
int morse_cwd_server_supported(void);

typedef struct morse_cwd_config {
  unsigned short port;        /* UDP port (cwdaemon default is 6789)         */
  morse_variant_t variant;    /* codebook variant                            */
  double wpm;                 /* initial speed                               */
  double tone_hz;             /* initial sidetone (0 = silent / key only)    */
  const char *serial_device;  /* optional: key a serial line (NULL = none)   */
  morse_serial_keyline_t keyline;
  int verbose;                /* log received messages to stderr             */
} morse_cwd_config_t;

void morse_cwd_config_default(morse_cwd_config_t *cfg);

/*
 * Run a blocking cwdaemon-compatible server until an EXIT message is received
 * or an error occurs. Received text is keyed via the configured serial device
 * if any (otherwise it is logged). Speed/tone/PTT escapes are honored. Returns
 * MORSE_ERR_UNSUPPORTED where UDP sockets are unavailable.
 */
morse_status_t morse_cwd_serve(const morse_cwd_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* MORSE_CW_H */
