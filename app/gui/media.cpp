// media.cpp - ffmpeg subprocess helpers.
#include "media.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// Quote a path for the platform shell. cmd.exe (Windows) uses double quotes and
// has no single-quote semantics; /bin/sh uses single quotes with the usual
// '\'' escape for embedded quotes.
std::string shq(const std::string &s) {
#if defined(_WIN32)
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += "\"";
  return out;
#else
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
#endif
}

int run(const std::string &cmd) {
  // Route output to the platform's null device so ffmpeg's banner does not spam
  // the terminal. system() uses cmd.exe on Windows and /bin/sh elsewhere.
#if defined(_WIN32)
  std::string full = cmd + " >NUL 2>&1";
#else
  std::string full = cmd + " >/dev/null 2>&1";
#endif
  return std::system(full.c_str());
}

} // namespace

namespace media {

bool ffmpegAvailable() { return run("ffmpeg -version") == 0; }

bool importToWav(const std::string &input, const std::string &wav_out,
                 unsigned int sample_rate) {
  std::string cmd = "ffmpeg -y -i " + shq(input) + " -ac 1 -ar " +
                    std::to_string(sample_rate) + " -f wav " + shq(wav_out);
  return run(cmd) == 0;
}

bool exportFromWav(const std::string &wav_in, const std::string &output) {
  std::string cmd = "ffmpeg -y -i " + shq(wav_in) + " " + shq(output);
  return run(cmd) == 0;
}

} // namespace media
