// audio.hpp - Real-time audio for the GUI, backed by miniaudio.
//
// The engine handles four things the UI needs:
//   * one-shot playback of a rendered PCM buffer (the "play this message" button)
//   * a live keyer sidetone gated by an atomic key-down flag (straight key)
//   * capture from a *selectable* input - microphone, a monitor/"what-u-hear"
//     source, or (on Windows/WASAPI) true output loopback - buffered for the UI
//     thread to feed the live decoder
//   * device enumeration so the UI can offer those sources
//
// The miniaudio data callback runs on a real-time audio thread, so everything
// it touches is either an atomic or guarded by a short-held mutex, and it never
// calls into libmorse. Decoding of captured audio happens on the UI thread,
// which drains the capture buffer each frame.
//
// miniaudio's concrete types (ma_context, ma_device_id, ...) are kept out of
// this header behind a PImpl so including it stays cheap.
#ifndef MORSE_GUI_AUDIO_HPP
#define MORSE_GUI_AUDIO_HPP

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct ma_device; // opaque; real type lives in the .cpp

// A selectable audio endpoint.
struct AudioDevice {
  std::string name;
  bool is_default = false;
};

class AudioEngine {
public:
  AudioEngine();
  ~AudioEngine();

  bool init(unsigned int sample_rate = 44100);
  void shutdown();
  bool ready() const { return ready_; }
  unsigned int sampleRate() const { return sample_rate_; }

  // ---- device enumeration ------------------------------------------------
  // Refresh the cached device lists from the system. Safe to call any time.
  bool refreshDevices();
  const std::vector<AudioDevice> &captureDevices() const { return capture_devs_; }
  const std::vector<AudioDevice> &playbackDevices() const {
    return playback_devs_;
  }
  // True only where output loopback is natively supported (Windows/WASAPI).
  bool loopbackSupported() const;

  // ---- one-shot playback -------------------------------------------------
  void play(const std::vector<float> &samples);
  void stopPlayback();
  bool isPlaying() const { return play_active_.load(); }
  float playProgress() const;
  bool markActive() const { return mark_active_.load(); }

  // ---- live keyer sidetone ----------------------------------------------
  void setKeyer(bool down, double tone_hz, double amplitude);
  void keyerEnable(bool on);

  // ---- capture -----------------------------------------------------------
  // Start capturing. `capture_index` selects an entry from captureDevices()
  // (-1 = system default). When `loopback` is true, `playback_index` selects an
  // output from playbackDevices() to capture (-1 = default output); this only
  // works where loopbackSupported() is true. For monitor-based system audio on
  // Linux/macOS, leave loopback false and pick the monitor entry from the
  // capture list instead.
  bool startCapture(int capture_index = -1, bool loopback = false,
                    int playback_index = -1);
  void stopCapture();
  bool isCapturing() const { return capturing_; }
  std::size_t drainCapture(std::vector<float> &out);

  // Marks where tone regions are in the playback buffer so markActive() can be
  // computed without re-running detection. Each pair is [start,end) in frames.
  void setMarkRegions(std::vector<std::pair<std::size_t, std::size_t>> regions);

private:
  static void playbackCb(ma_device *dev, void *out, const void *in,
                         unsigned int frames);
  static void captureCb(ma_device *dev, void *out, const void *in,
                        unsigned int frames);

  struct Impl;          // holds ma_context + device-id tables
  Impl *impl_ = nullptr;

  ma_device *play_dev_ = nullptr;
  ma_device *cap_dev_ = nullptr;
  bool ready_ = false;
  bool capturing_ = false;
  bool keyer_on_ = false;
  unsigned int sample_rate_ = 44100;

  std::vector<AudioDevice> capture_devs_;
  std::vector<AudioDevice> playback_devs_;

  // playback buffer (guarded; only swapped when not playing)
  std::mutex play_mtx_;
  std::vector<float> play_buf_;
  std::atomic<std::size_t> play_cursor_{0};
  std::atomic<bool> play_active_{false};
  std::atomic<bool> mark_active_{false};
  std::vector<std::pair<std::size_t, std::size_t>> mark_regions_;
  std::mutex mark_mtx_;

  // keyer
  std::atomic<bool> key_down_{false};
  std::atomic<double> key_tone_{600.0};
  std::atomic<double> key_amp_{0.6};
  double key_phase_ = 0.0; // callback-owned
  double key_env_ = 0.0;   // smoothed gate 0..1 for click-free keying

  // capture
  std::mutex cap_mtx_;
  std::vector<float> cap_buf_;
};

#endif // MORSE_GUI_AUDIO_HPP
