// cwio_cpp.cpp - Implementation of the GUI's CW interface glue.
#include "cwio.hpp"

#include "morse/table.h"

#include <cstring>

// Minimal portable loopback UDP send, used to stop the cwdaemon server.
#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

void udpSendLoopback(unsigned short port, const void *data, std::size_t len) {
#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return;
  }
  SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s != INVALID_SOCKET) {
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, static_cast<const char *>(data), static_cast<int>(len), 0,
           reinterpret_cast<sockaddr *>(&a), sizeof(a));
    closesocket(s);
  }
  WSACleanup();
#else
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s >= 0) {
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, data, len, 0, reinterpret_cast<sockaddr *>(&a), sizeof(a));
    close(s);
  }
#endif
}

} // namespace

CwIo::~CwIo() {
  stopCwd();
  if (serial_thread_.joinable()) {
    serial_thread_.join();
  }
}

void CwIo::setStatus(const std::string &s) {
  std::lock_guard<std::mutex> lk(status_mtx_);
  status_ = s;
}

std::string CwIo::status() {
  std::lock_guard<std::mutex> lk(status_mtx_);
  return status_;
}

// ---- serial -------------------------------------------------------------

bool CwIo::serialSupported() const { return morse_serial_supported() != 0; }

void CwIo::sendSerial(const std::string &device, morse_serial_keyline_t keyline,
                      morse_variant_t variant, const std::string &text,
                      const morse_durations_t &dur) {
  if (serial_busy_.load()) {
    return;
  }
  if (serial_thread_.joinable()) {
    serial_thread_.join();
  }
  serial_busy_.store(true);
  setStatus("keying \"" + text + "\" on " + device);
  morse_durations_t d = dur;
  std::string dev = device;
  std::string txt = text;
  serial_thread_ = std::thread([this, dev, keyline, variant, txt, d]() {
    morse_table_t *t = morse_table_create(variant);
    morse_status_t st =
        morse_serial_send_text(dev.c_str(), keyline, t, txt.c_str(), &d);
    if (t != nullptr) {
      morse_table_destroy(t);
    }
    setStatus(st == MORSE_OK ? std::string("serial: sent")
                             : std::string("serial: ") + morse_status_str(st));
    serial_busy_.store(false);
  });
}

// ---- cwdaemon server ----------------------------------------------------

bool CwIo::cwdSupported() const { return morse_cwd_server_supported() != 0; }

bool CwIo::startCwd(const morse_cwd_config_t &cfg) {
  if (cwd_running_.load()) {
    return false;
  }
  if (cwd_thread_.joinable()) {
    cwd_thread_.join();
  }
  cwd_port_ = cfg.port;
  cwd_running_.store(true);
  setStatus("cwdaemon server listening on UDP " + std::to_string(cfg.port));
  morse_cwd_config_t copy = cfg;
  cwd_thread_ = std::thread([this, copy]() {
    morse_status_t st = morse_cwd_serve(&copy);
    setStatus(st == MORSE_OK ? std::string("cwdaemon: stopped")
                             : std::string("cwdaemon: ") + morse_status_str(st));
    cwd_running_.store(false);
  });
  return true;
}

void CwIo::stopCwd() {
  if (cwd_running_.load()) {
    // cwdaemon EXIT escape: ESC '5'
    const unsigned char exit_msg[2] = {0x1B, '5'};
    udpSendLoopback(cwd_port_, exit_msg, sizeof(exit_msg));
  }
  if (cwd_thread_.joinable()) {
    cwd_thread_.join();
  }
}
