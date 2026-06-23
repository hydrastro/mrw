// cwio.hpp - GUI-side glue for the libmorse CW interfaces.
//
// libmorse's CW functions (morse_serial_send_text, morse_cwd_serve) are
// blocking: keying a message sleeps between elements, and the cwdaemon server
// loops until told to exit. The GUI must stay responsive, so this wrapper runs
// each on its own std::thread and exposes simple status/!busy flags. It also
// knows how to stop a running cwdaemon server by sending it the EXIT datagram.
#ifndef MORSE_GUI_CWIO_HPP
#define MORSE_GUI_CWIO_HPP

#include "morse/cw.h"
#include "morse/timing.h"
#include "morse/types.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class CwIo {
public:
  CwIo() = default;
  ~CwIo();

  // ---- serial line keying (background) ----------------------------------
  bool serialSupported() const;
  // Key `text` out `device`'s RTS/DTR line at `dur`, on a worker thread.
  // Ignored if a serial send is already in progress.
  void sendSerial(const std::string &device, morse_serial_keyline_t keyline,
                  morse_variant_t variant, const std::string &text,
                  const morse_durations_t &dur);
  bool serialBusy() const { return serial_busy_.load(); }

  // ---- cwdaemon-compatible UDP server (background) ----------------------
  bool cwdSupported() const;
  bool startCwd(const morse_cwd_config_t &cfg); // copies cfg; false if running
  void stopCwd();                               // sends EXIT, joins
  bool cwdRunning() const { return cwd_running_.load(); }
  unsigned short cwdPort() const { return cwd_port_; }

  std::string status();

private:
  void setStatus(const std::string &s);

  std::thread serial_thread_;
  std::atomic<bool> serial_busy_{false};

  std::thread cwd_thread_;
  std::atomic<bool> cwd_running_{false};
  unsigned short cwd_port_ = 6789;

  std::mutex status_mtx_;
  std::string status_;
};

#endif // MORSE_GUI_CWIO_HPP
