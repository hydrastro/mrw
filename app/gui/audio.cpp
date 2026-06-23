// audio.cpp - miniaudio-backed implementation of AudioEngine.
#include "audio.hpp"

#include "miniaudio.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// PImpl: keep miniaudio's concrete types out of the header.
struct AudioEngine::Impl {
  ma_context context;
  bool have_context = false;
  std::vector<ma_device_id> capture_ids;
  std::vector<ma_device_id> playback_ids;
};

AudioEngine::AudioEngine() { impl_ = new Impl(); }

AudioEngine::~AudioEngine() {
  shutdown();
  delete impl_;
  impl_ = nullptr;
}

bool AudioEngine::init(unsigned int sample_rate) {
  if (ready_) {
    return true;
  }
  sample_rate_ = sample_rate;

  if (ma_context_init(nullptr, 0, nullptr, &impl_->context) != MA_SUCCESS) {
    std::fprintf(stderr,
                 "audio: could not initialise any miniaudio backend.\n"
                 "       On NixOS, a binary run directly (not via `nix run`)\n"
                 "       needs the audio libraries on the loader path, e.g.\n"
                 "         export LD_LIBRARY_PATH=$(nix eval --raw "
                 "nixpkgs#alsa-lib.out)/lib:$(nix eval --raw "
                 "nixpkgs#libpulseaudio.out)/lib:$LD_LIBRARY_PATH\n"
                 "       or just run `nix run` / `nix develop`.\n");
    return false;
  }
  impl_->have_context = true;
  refreshDevices();

  play_dev_ = new ma_device();
  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 1;
  cfg.sampleRate = sample_rate_;
  cfg.dataCallback = &AudioEngine::playbackCb;
  cfg.pUserData = this;
  if (ma_device_init(&impl_->context, &cfg, play_dev_) != MA_SUCCESS) {
    delete play_dev_;
    play_dev_ = nullptr;
    return false;
  }
  if (ma_device_start(play_dev_) != MA_SUCCESS) {
    ma_device_uninit(play_dev_);
    delete play_dev_;
    play_dev_ = nullptr;
    return false;
  }
  ready_ = true;
  return true;
}

void AudioEngine::shutdown() {
  stopCapture();
  if (play_dev_ != nullptr) {
    ma_device_uninit(play_dev_);
    delete play_dev_;
    play_dev_ = nullptr;
  }
  if (impl_ != nullptr && impl_->have_context) {
    ma_context_uninit(&impl_->context);
    impl_->have_context = false;
  }
  ready_ = false;
}

// ---- device enumeration --------------------------------------------------

bool AudioEngine::refreshDevices() {
  if (impl_ == nullptr || !impl_->have_context) {
    return false;
  }
  ma_device_info *playback = nullptr;
  ma_uint32 playback_n = 0;
  ma_device_info *capture = nullptr;
  ma_uint32 capture_n = 0;
  if (ma_context_get_devices(&impl_->context, &playback, &playback_n, &capture,
                             &capture_n) != MA_SUCCESS) {
    return false;
  }

  capture_devs_.clear();
  impl_->capture_ids.clear();
  for (ma_uint32 i = 0; i < capture_n; ++i) {
    AudioDevice d;
    d.name = capture[i].name;
    d.is_default = capture[i].isDefault != 0;
    capture_devs_.push_back(d);
    impl_->capture_ids.push_back(capture[i].id);
  }

  playback_devs_.clear();
  impl_->playback_ids.clear();
  for (ma_uint32 i = 0; i < playback_n; ++i) {
    AudioDevice d;
    d.name = playback[i].name;
    d.is_default = playback[i].isDefault != 0;
    playback_devs_.push_back(d);
    impl_->playback_ids.push_back(playback[i].id);
  }
  return true;
}

bool AudioEngine::loopbackSupported() const {
#if defined(_WIN32)
  return true; // WASAPI loopback
#else
  return false;
#endif
}

// ---- one-shot playback ---------------------------------------------------

void AudioEngine::play(const std::vector<float> &samples) {
  {
    std::lock_guard<std::mutex> lk(play_mtx_);
    play_buf_ = samples;
  }
  play_cursor_.store(0);
  play_active_.store(true);
}

void AudioEngine::stopPlayback() {
  play_active_.store(false);
  play_cursor_.store(0);
  mark_active_.store(false);
}

float AudioEngine::playProgress() const {
  std::size_t n = play_buf_.size();
  if (n == 0) {
    return 0.0f;
  }
  std::size_t c = play_cursor_.load();
  if (c >= n) {
    return 1.0f;
  }
  return static_cast<float>(c) / static_cast<float>(n);
}

void AudioEngine::setMarkRegions(
    std::vector<std::pair<std::size_t, std::size_t>> regions) {
  std::lock_guard<std::mutex> lk(mark_mtx_);
  mark_regions_ = std::move(regions);
}

// ---- keyer ---------------------------------------------------------------

void AudioEngine::setKeyer(bool down, double tone_hz, double amplitude) {
  key_down_.store(down);
  key_tone_.store(tone_hz);
  key_amp_.store(amplitude);
}

void AudioEngine::keyerEnable(bool on) {
  keyer_on_ = on;
  if (on) {
    stopPlayback();
  } else {
    key_down_.store(false);
  }
}

// ---- capture -------------------------------------------------------------

bool AudioEngine::startCapture(int capture_index, bool loopback,
                               int playback_index) {
  if (capturing_) {
    stopCapture();
  }
  if (impl_ == nullptr || !impl_->have_context) {
    return false;
  }

  cap_dev_ = new ma_device();
  ma_device_config cfg;

  if (loopback && loopbackSupported()) {
    cfg = ma_device_config_init(ma_device_type_loopback);
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    // For loopback, the "capture" device id names the *output* to record.
    if (playback_index >= 0 &&
        static_cast<std::size_t>(playback_index) < impl_->playback_ids.size()) {
      cfg.capture.pDeviceID = &impl_->playback_ids[playback_index];
    }
  } else {
    cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    if (capture_index >= 0 &&
        static_cast<std::size_t>(capture_index) < impl_->capture_ids.size()) {
      cfg.capture.pDeviceID = &impl_->capture_ids[capture_index];
    }
  }
  cfg.sampleRate = sample_rate_;
  cfg.dataCallback = &AudioEngine::captureCb;
  cfg.pUserData = this;

  if (ma_device_init(&impl_->context, &cfg, cap_dev_) != MA_SUCCESS) {
    delete cap_dev_;
    cap_dev_ = nullptr;
    return false;
  }
  if (ma_device_start(cap_dev_) != MA_SUCCESS) {
    ma_device_uninit(cap_dev_);
    delete cap_dev_;
    cap_dev_ = nullptr;
    return false;
  }
  capturing_ = true;
  return true;
}

void AudioEngine::stopCapture() {
  if (cap_dev_ != nullptr) {
    ma_device_uninit(cap_dev_);
    delete cap_dev_;
    cap_dev_ = nullptr;
  }
  capturing_ = false;
}

std::size_t AudioEngine::drainCapture(std::vector<float> &out) {
  std::lock_guard<std::mutex> lk(cap_mtx_);
  out.insert(out.end(), cap_buf_.begin(), cap_buf_.end());
  std::size_t n = cap_buf_.size();
  cap_buf_.clear();
  return n;
}

// ---- callbacks (audio thread) --------------------------------------------

void AudioEngine::playbackCb(ma_device *dev, void *out, const void *in,
                             unsigned int frames) {
  (void)in;
  AudioEngine *self = static_cast<AudioEngine *>(dev->pUserData);
  float *o = static_cast<float *>(out);

  if (self->keyer_on_) {
    const double sr = static_cast<double>(self->sample_rate_);
    const double f = self->key_tone_.load();
    const double amp = self->key_amp_.load();
    const bool down = self->key_down_.load();
    const double target = down ? 1.0 : 0.0;
    const double coeff = std::exp(-1.0 / (0.005 * sr)); // ~5 ms
    double phase = self->key_phase_;
    double env = self->key_env_;
    const double inc = 2.0 * 3.14159265358979323846 * f / sr;
    for (unsigned int i = 0; i < frames; ++i) {
      env = target + (env - target) * coeff;
      o[i] = static_cast<float>(std::sin(phase) * env * amp);
      phase += inc;
      if (phase > 2.0 * 3.14159265358979323846) {
        phase -= 2.0 * 3.14159265358979323846;
      }
    }
    self->key_phase_ = phase;
    self->key_env_ = env;
    return;
  }

  if (self->play_active_.load()) {
    std::size_t cursor = self->play_cursor_.load();
    const std::vector<float> &buf = self->play_buf_;
    std::size_t n = buf.size();
    bool in_mark = false;
    for (unsigned int i = 0; i < frames; ++i) {
      if (cursor < n) {
        o[i] = buf[cursor];
        ++cursor;
      } else {
        o[i] = 0.0f;
      }
    }
    self->play_cursor_.store(cursor);
    if (cursor >= n) {
      self->play_active_.store(false);
      self->mark_active_.store(false);
    } else {
      std::lock_guard<std::mutex> lk(self->mark_mtx_);
      for (const auto &r : self->mark_regions_) {
        if (cursor >= r.first && cursor < r.second) {
          in_mark = true;
          break;
        }
      }
      self->mark_active_.store(in_mark);
    }
    return;
  }

  std::memset(o, 0, sizeof(float) * frames);
}

void AudioEngine::captureCb(ma_device *dev, void *out, const void *in,
                            unsigned int frames) {
  (void)out;
  AudioEngine *self = static_cast<AudioEngine *>(dev->pUserData);
  const float *src = static_cast<const float *>(in);
  if (src == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lk(self->cap_mtx_);
  self->cap_buf_.insert(self->cap_buf_.end(), src, src + frames);
}
