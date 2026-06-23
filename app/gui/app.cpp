// app.cpp - Implementation of the morse-deluxe GUI application.
#include "app.hpp"

#include "media.hpp"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace {

// Sink shared by the live keyer decoder and the capture detector: append the
// decoded UTF-8 to the std::string handed in as `user`.
void appendSink(const char *utf8, void *user) {
  if (utf8 != nullptr && user != nullptr) {
    static_cast<std::string *>(user)->append(utf8);
  }
}

std::string takeStr(ds_str_t *s) {
  std::string out = (s != nullptr) ? FUNC_str_cstr(s) : "";
  if (s != nullptr) {
    ds_str_destroy(s);
  }
  return out;
}

const char *kTempWavIn = "/tmp/morse_deluxe_import.wav";
const char *kTempWavOut = "/tmp/morse_deluxe_export.wav";

void sectionLabel(const char *text) {
  ImGui::Spacing();
  ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "%s", text);
  ImGui::Separator();
}

} // namespace

App::App() {
  text_in_.assign(8192, 0);
  code_in_.assign(8192, 0);
  path_buf_.assign(1024, 0);
  export_buf_.assign(1024, 0);
  cw_device_.assign(256, 0);
  wk_text_.assign(1024, 0);
  std::snprintf(text_in_.data(), text_in_.size(), "%s", "CQ CQ DE K1ABC <AR>");
  std::snprintf(code_in_.data(), code_in_.size(), "%s", "... --- ...");
  std::snprintf(export_buf_.data(), export_buf_.size(), "%s", "message.ogg");
  std::snprintf(wk_text_.data(), wk_text_.size(), "%s", "CQ TEST DE K1ABC");
#if defined(_WIN32)
  std::snprintf(cw_device_.data(), cw_device_.size(), "%s", "COM3");
#else
  std::snprintf(cw_device_.data(), cw_device_.size(), "%s", "/dev/ttyUSB0");
#endif
}

App::~App() {
  if (keyer_dec_ != nullptr) {
    morse_stream_decoder_destroy(keyer_dec_);
  }
  if (multi_ != nullptr) {
    morse_multi_destroy(multi_);
  }
  if (table_ != nullptr) {
    morse_table_destroy(table_);
  }
}

void App::init() {
  morse_diagnostics_enable();
  diagnostics_ = true;
  audio_.init(static_cast<unsigned int>(rate_));
  audio_.refreshDevices();
  rebuildTables();
  morse_iambic_init(&iambic_, MORSE_IAMBIC_B, 1200.0 / wpm_);
  ffmpeg_ok_ = media::ffmpegAvailable();

  // Default to listening to the system's own audio output, as requested.
  if (audio_.loopbackSupported()) {
    use_loopback_ = true; // Windows/WASAPI true loopback
  } else {
    // Elsewhere, auto-select a monitor / "Stereo Mix" capture device if present.
    const auto &caps = audio_.captureDevices();
    for (int i = 0; i < (int)caps.size(); ++i) {
      std::string n = caps[i].name;
      for (char &c : n) {
        c = (char)std::tolower((unsigned char)c);
      }
      if (n.find("monitor") != std::string::npos ||
          n.find("stereo mix") != std::string::npos ||
          n.find("what u hear") != std::string::npos ||
          n.find("loopback") != std::string::npos) {
        capture_idx_ = i;
        break;
      }
    }
  }

  doEncode();
  doRender();
  applyStyle();
}

void App::applyStyle() {
  ImGuiStyle &s = ImGui::GetStyle();
  s.WindowRounding = 6.0f;
  s.FrameRounding = 5.0f;
  s.GrabRounding = 5.0f;
  s.TabRounding = 5.0f;
  s.ChildRounding = 6.0f;
  s.PopupRounding = 5.0f;
  s.ScrollbarRounding = 6.0f;
  s.FrameBorderSize = 1.0f;
  s.WindowPadding = ImVec2(12, 12);
  s.FramePadding = ImVec2(8, 5);
  s.ItemSpacing = ImVec2(9, 7);
  s.ItemInnerSpacing = ImVec2(7, 5);
  s.GrabMinSize = 11.0f;

  ImVec4 *c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
  c[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.13f, 0.16f, 0.55f);
  c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.19f, 0.23f, 1.00f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.23f, 0.26f, 0.31f, 1.00f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.31f, 0.37f, 1.00f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.16f, 0.21f, 1.00f);
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.36f, 0.55f, 1.00f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.47f, 0.70f, 1.00f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.33f, 0.57f, 0.84f, 1.00f);
  c[ImGuiCol_Header] = ImVec4(0.20f, 0.36f, 0.55f, 0.70f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.27f, 0.47f, 0.70f, 0.80f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.33f, 0.57f, 0.84f, 1.00f);
  c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.78f, 1.00f, 1.00f);
  c[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.66f, 0.96f, 1.00f);
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.50f, 0.76f, 1.00f, 1.00f);
  c[ImGuiCol_Tab] = ImVec4(0.16f, 0.19f, 0.24f, 1.00f);
  c[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.50f, 0.74f, 1.00f);
  c[ImGuiCol_TabActive] = ImVec4(0.22f, 0.40f, 0.61f, 1.00f);
  c[ImGuiCol_PlotLines] = ImVec4(0.55f, 0.85f, 1.00f, 1.00f);
  c[ImGuiCol_PlotHistogram] = ImVec4(0.45f, 0.80f, 0.55f, 1.00f);
  c[ImGuiCol_Separator] = ImVec4(0.26f, 0.30f, 0.36f, 1.00f);
}

void App::rebuildTables() {
  if (table_ != nullptr) {
    morse_table_destroy(table_);
  }
  table_ = morse_table_create(variant());
}

void App::fillDetectOpts(morse_detect_opts_t *o) const {
  morse_detect_opts_default(o);
  o->tone_hz = manual_tone_;
  o->tone_min_hz = tone_min_;
  o->tone_max_hz = tone_max_;
  o->track_tone = track_tone_ ? 1 : 0;
}

// ---- encode / decode -----------------------------------------------------

void App::doEncode() {
  if (table_ == nullptr) {
    return;
  }
  morse_encode_opts_t o;
  morse_encode_opts_default(&o);
  ds_str_t *out = ds_str_create();
  morse_status_t st = morse_encode_string(table_, text_in_.data(), &o, out);
  if (st == MORSE_OK) {
    code_out_ = takeStr(out);
  } else {
    code_out_ = std::string("[") + morse_status_str(st) + "]";
    ds_str_destroy(out);
  }
}

void App::doDecodeText() {
  if (table_ == nullptr) {
    return;
  }
  morse_decode_opts_t o;
  morse_decode_opts_default(&o);
  ds_str_t *out = ds_str_create();
  morse_status_t st = morse_decode_string(table_, code_in_.data(), &o, out);
  if (st == MORSE_OK) {
    text_out_ = takeStr(out);
  } else {
    text_out_ = std::string("[") + morse_status_str(st) + "]";
    ds_str_destroy(out);
  }
}

void App::doRender() {
  if (table_ == nullptr) {
    return;
  }
  morse_timing_t tm;
  morse_durations_t dur;
  morse_synth_opts_t so;

  morse_timing_default(&tm);
  tm.wpm = wpm_;
  tm.char_wpm = char_wpm_;
  tm.weight = weight_;
  if (morse_timing_resolve(&tm, &dur) != MORSE_OK) {
    return;
  }

  morse_synth_opts_default(&so);
  so.sample_rate = static_cast<unsigned int>(rate_);
  so.tone_hz = tone_;
  so.amplitude = amplitude_;
  so.ramp_ms = ramp_;
  so.add_noise = noise_ > 0.0f ? 1 : 0;
  so.noise_amplitude = noise_;

  list_t *els = ds_list_create();
  total_ms_ = 0.0;
  if (morse_encode_elements(table_, text_in_.data(), &dur, MORSE_UNKNOWN_SKIP,
                            els, &total_ms_) != MORSE_OK) {
    morse_elements_free(els);
    return;
  }

  mark_regions_.clear();
  for (list_node_t *it = els->head; it != els->nil; it = it->next) {
    const morse_element_t *e = reinterpret_cast<const morse_element_t *>(it);
    if (morse_symbol_is_mark(e->symbol)) {
      std::size_t a = static_cast<std::size_t>(e->start_ms / 1000.0 * rate_);
      std::size_t b = static_cast<std::size_t>(
          (e->start_ms + e->duration_ms) / 1000.0 * rate_);
      mark_regions_.emplace_back(a, b);
    }
  }

  morse_pcm_t pcm;
  morse_pcm_init(&pcm);
  morse_status_t st = morse_synth_render(els, &so, &pcm);
  morse_elements_free(els);
  if (st != MORSE_OK) {
    morse_pcm_free(&pcm);
    return;
  }

  pcm_.assign(pcm.samples, pcm.samples + pcm.count);
  morse_pcm_free(&pcm);

  const std::size_t target = 800;
  waveform_.clear();
  if (!pcm_.empty()) {
    std::size_t step = std::max<std::size_t>(1, pcm_.size() / target);
    for (std::size_t i = 0; i < pcm_.size(); i += step) {
      float peak = 0.0f;
      for (std::size_t j = i; j < i + step && j < pcm_.size(); ++j) {
        float a = std::fabs(pcm_[j]);
        if (a > peak) {
          peak = pcm_[j] < 0 ? -a : a;
        }
      }
      waveform_.push_back(peak);
    }
  }
  audio_.setMarkRegions(mark_regions_);
}

void App::doPlay() {
  if (pcm_.empty()) {
    doRender();
  }
  audio_.setMarkRegions(mark_regions_);
  audio_.play(pcm_);
}

// ---- offline listen + media ----------------------------------------------

void App::listenFile(const std::string &path) {
  if (table_ == nullptr) {
    return;
  }
  morse_pcm_t pcm;
  morse_pcm_init(&pcm);
  if (morse_wav_read(path.c_str(), &pcm) != MORSE_OK) {
    status_ = "could not read " + path;
    morse_pcm_free(&pcm);
    return;
  }

  morse_detect_opts_t det;
  fillDetectOpts(&det);
  morse_envelope_t env;
  morse_envelope_init(&env);
  ds_str_t *out = ds_str_create();
  morse_status_t st = morse_detect_pcm(&pcm, table_, &det, out, &env);
  listen_out_ = takeStr(out);

  envelope_.assign(env.power, env.power + env.count);
  detected_tone_ = env.tone_hz;
  morse_envelope_free(&env);

  pcm_.assign(pcm.samples, pcm.samples + pcm.count);
  rate_ = static_cast<int>(pcm.sample_rate);
  morse_pcm_free(&pcm);

  mark_regions_.clear();
  audio_.setMarkRegions(mark_regions_);
  const std::size_t target = 800;
  waveform_.clear();
  if (!pcm_.empty()) {
    std::size_t step = std::max<std::size_t>(1, pcm_.size() / target);
    for (std::size_t i = 0; i < pcm_.size(); i += step) {
      waveform_.push_back(pcm_[i]);
    }
  }
  spectrum_src_.assign(pcm_.begin(),
                       pcm_.begin() + std::min<std::size_t>(pcm_.size(), 16384));
  refreshSpectrum();
  status_ = (st == MORSE_OK) ? "decoded " + path : "decode error";
}

void App::importMedia(const std::string &path) {
  if (!ffmpeg_ok_) {
    status_ = "ffmpeg not available";
    return;
  }
  if (!media::importToWav(path, kTempWavIn,
                          static_cast<unsigned int>(rate_))) {
    status_ = "ffmpeg import failed";
    return;
  }
  decodeFileMulti(kTempWavIn);
}

void App::exportMedia(const std::string &path) {
  if (pcm_.empty()) {
    doRender();
  }
  morse_pcm_t pcm;
  morse_pcm_init(&pcm);
  pcm.samples = pcm_.data();
  pcm.count = pcm_.size();
  pcm.capacity = pcm_.size();
  pcm.sample_rate = static_cast<unsigned int>(rate_);
  morse_status_t st = morse_wav_write(kTempWavOut, &pcm);
  if (st != MORSE_OK) {
    status_ = "could not write temp wav";
    return;
  }
  const bool wantWav =
      path.size() >= 4 && (path.compare(path.size() - 4, 4, ".wav") == 0 ||
                           path.compare(path.size() - 4, 4, ".WAV") == 0);
  if (wantWav || !ffmpeg_ok_) {
    if (std::rename(kTempWavOut, path.c_str()) != 0) {
      if (ffmpeg_ok_ && media::exportFromWav(kTempWavOut, path)) {
        status_ = "exported " + path;
      } else {
        status_ = "export failed";
      }
      return;
    }
    status_ = "exported " + path;
    return;
  }
  status_ = media::exportFromWav(kTempWavOut, path) ? "exported " + path
                                                    : "ffmpeg export failed";
}

// ---- spectrum ------------------------------------------------------------

void App::refreshSpectrum() {
  if (spectrum_src_.size() < 256) {
    return;
  }
  double strength = 0.0;
  detected_tone_ = morse_dominant_tone(
      spectrum_src_.data(), spectrum_src_.size(),
      static_cast<unsigned int>(rate_), tone_min_, tone_max_, 0, &strength);
  tone_strength_ = strength;

  std::size_t n = morse_floor_pow2(spectrum_src_.size());
  if (n > 4096) {
    n = 4096;
  }
  if (n < 256) {
    return;
  }
  const float *x = spectrum_src_.data() + (spectrum_src_.size() - n);
  std::vector<double> mag(n / 2, 0.0);
  if (morse_real_spectrum(x, n, mag.data()) != 0) {
    return;
  }
  double binHz = static_cast<double>(rate_) / static_cast<double>(n);
  std::size_t maxBin = std::min<std::size_t>(
      mag.size(), static_cast<std::size_t>(spectrum_max_hz_ / binHz) + 1);
  double peak = 1e-9;
  for (std::size_t i = 0; i < maxBin; ++i) {
    if (mag[i] > peak) {
      peak = mag[i];
    }
  }
  spectrum_.resize(maxBin);
  for (std::size_t i = 0; i < maxBin; ++i) {
    spectrum_[i] = static_cast<float>(mag[i] / peak);
  }
}

// ---- capture (microphone / system audio) ---------------------------------

void App::startCapture() {
  stopKeyer();
  if (multi_ != nullptr) {
    morse_multi_destroy(multi_);
    multi_ = nullptr;
  }
  stations_.clear();
  morse_multi_opts_t mo;
  morse_multi_opts_default(&mo);
  mo.tone_min_hz = tone_min_;
  mo.tone_max_hz = tone_max_;
  multi_ = morse_multi_create(table_, static_cast<unsigned int>(rate_), &mo,
                              nullptr, nullptr);
  bool ok = multi_ != nullptr &&
            audio_.startCapture(capture_idx_, use_loopback_, playback_idx_);
  if (ok) {
    capturing_ = true;
    envelope_.clear();
    spectrum_src_.clear();
    status_ = use_loopback_ ? "listening to system audio (loopback)"
                            : "listening on selected input";
  } else {
    capturing_ = false;
    status_ = "could not open capture device";
  }
}

void App::stopCapture() {
  if (capturing_) {
    audio_.stopCapture();
  }
  capturing_ = false;
}

// ---- live keyer (straight + iambic) --------------------------------------

void App::startKeyer() {
  stopCapture();
  if (keyer_dec_ != nullptr) {
    morse_stream_decoder_destroy(keyer_dec_);
  }
  keyer_text_.clear();
  double dit = 1200.0 / (wpm_ > 0 ? wpm_ : 20.0);
  keyer_dec_ =
      morse_stream_decoder_create(table_, dit, &appendSink, &keyer_text_);
  morse_iambic_init(&iambic_,
                    iambic_mode_idx_ == 0 ? MORSE_IAMBIC_A : MORSE_IAMBIC_B,
                    dit);
  keyer_on_ = true;
  prev_key_ = false;
  keyer_seen_mark_ = false;
  keyer_accum_ms_ = 0.0;
  last_edge_ = ImGui::GetTime();
  audio_.keyerEnable(true);
}

void App::stopKeyer() {
  if (keyer_on_ && keyer_dec_ != nullptr) {
    morse_stream_decoder_finish(keyer_dec_);
  }
  keyer_on_ = false;
  audio_.keyerEnable(false);
}

void App::updateKeyer() {
  if (!keyer_on_ || keyer_dec_ == nullptr) {
    return;
  }
  ImGuiIO &io = ImGui::GetIO();
  double now = ImGui::GetTime();
  bool key = false;

  if (use_iambic_) {
    bool dit_p = dit_btn_ || ImGui::IsKeyDown(ImGuiKey_Z) ||
                 ImGui::IsKeyDown(ImGuiKey_LeftArrow);
    bool dah_p = dah_btn_ || ImGui::IsKeyDown(ImGuiKey_X) ||
                 ImGui::IsKeyDown(ImGuiKey_RightArrow);
    keyer_accum_ms_ += io.DeltaTime * 1000.0;
    int keyout = iambic_.key;
    const double step = 1.0;
    int guard = 0;
    while (keyer_accum_ms_ >= step && guard++ < 1000) {
      keyout = morse_iambic_tick(&iambic_, dit_p ? 1 : 0, dah_p ? 1 : 0, step,
                                 nullptr);
      keyer_accum_ms_ -= step;
    }
    key = keyout != 0;
  } else {
    key = straight_btn_ || ImGui::IsKeyDown(ImGuiKey_Space);
  }

  if (key != prev_key_) {
    double ms = (now - last_edge_) * 1000.0;
    if (prev_key_) {
      morse_stream_decoder_push(keyer_dec_, 1, ms);
      keyer_seen_mark_ = true;
    } else if (keyer_seen_mark_) {
      morse_stream_decoder_push(keyer_dec_, 0, ms);
    }
    last_edge_ = now;
    prev_key_ = key;
    audio_.setKeyer(key, tone_, amplitude_);
  } else if (!key && keyer_seen_mark_ && (now - last_edge_) > 1.2) {
    morse_stream_decoder_push(keyer_dec_, 0, (now - last_edge_) * 1000.0);
    keyer_seen_mark_ = false;
    last_edge_ = now;
  }
}

// ---- WinKeyer preview ----------------------------------------------------

void App::buildWinkeyerPreview() {
  unsigned char buf[1100];
  std::size_t n = 0;
  n += morse_winkeyer_open(buf + n, sizeof(buf) - n);
  n += morse_winkeyer_set_speed(buf + n, sizeof(buf) - n,
                                static_cast<int>(wpm_));
  n += morse_winkeyer_set_sidetone(buf + n, sizeof(buf) - n,
                                   static_cast<int>(tone_));
  n += morse_winkeyer_text(buf + n, sizeof(buf) - n, wk_text_.data());
  n += morse_winkeyer_close(buf + n, sizeof(buf) - n);

  char line[3400];
  std::size_t p = 0;
  for (std::size_t i = 0; i < n && p + 4 < sizeof(line); ++i) {
    p += static_cast<std::size_t>(
        std::snprintf(line + p, sizeof(line) - p, "%02X ", buf[i]));
  }
  wk_preview_.assign(line, p);
}

// ---- per-frame work (no ImGui calls here) --------------------------------

void App::onFrame() {
  if (capturing_ && multi_ != nullptr) {
    cap_scratch_.clear();
    if (audio_.drainCapture(cap_scratch_) > 0) {
      morse_multi_process(multi_, cap_scratch_.data(), cap_scratch_.size());
      // crude live power for the meter: peak magnitude of this chunk
      float power = 0.0f;
      for (float s : cap_scratch_) {
        float a = std::fabs(s);
        if (a > power) {
          power = a;
        }
      }
      live_power_ = power;
      envelope_.push_back(power);
      if (envelope_.size() > 800) {
        envelope_.erase(envelope_.begin(),
                        envelope_.begin() + (envelope_.size() - 800));
      }
      spectrum_src_.insert(spectrum_src_.end(), cap_scratch_.begin(),
                           cap_scratch_.end());
      if (spectrum_src_.size() > 16384) {
        spectrum_src_.erase(spectrum_src_.begin(),
                            spectrum_src_.begin() +
                                (spectrum_src_.size() - 16384));
      }
      refreshSpectrum();
      refreshStations();
    }
  }
}

void App::refreshStations() {
  if (multi_ == nullptr) {
    return;
  }
  stations_.clear();
  size_t n = morse_multi_channel_count(multi_);
  for (size_t i = 0; i < n; ++i) {
    morse_multi_channel_info_t info;
    const char *txt = morse_multi_channel_text(multi_, i);
    if (morse_multi_get_channel(multi_, i, &info)) {
      StationView v;
      v.id = info.id;
      v.hz = info.tone_hz;
      v.wpm = info.wpm;
      v.text = txt != nullptr ? txt : "";
      stations_.push_back(v);
    }
  }
  pullTimeline(multi_);
}

void App::decodeFileMulti(const std::string &path) {
  morse_pcm_t pcm;
  morse_pcm_init(&pcm);
  if (morse_wav_read(path.c_str(), &pcm) != MORSE_OK) {
    status_ = "could not read " + path;
    morse_pcm_free(&pcm);
    return;
  }
  rate_ = static_cast<int>(pcm.sample_rate);

  morse_multi_opts_t mo;
  morse_multi_opts_default(&mo);
  mo.tone_min_hz = tone_min_;
  mo.tone_max_hz = tone_max_;
  morse_multi_detector_t *md =
      morse_multi_create(table_, pcm.sample_rate, &mo, nullptr, nullptr);
  if (md != nullptr) {
    for (size_t i = 0; i < pcm.count; i += 4096) {
      size_t step = pcm.count - i < 4096 ? pcm.count - i : 4096;
      morse_multi_process(md, pcm.samples + i, step);
    }
    morse_multi_finish(md);
    stations_.clear();
    size_t n = morse_multi_channel_count(md);
    for (size_t i = 0; i < n; ++i) {
      morse_multi_channel_info_t info;
      const char *txt = morse_multi_channel_text(md, i);
      if (morse_multi_get_channel(md, i, &info)) {
        StationView v;
        v.id = info.id;
        v.hz = info.tone_hz;
        v.wpm = info.wpm;
        v.text = txt != nullptr ? txt : "";
        stations_.push_back(v);
      }
    }
    pullTimeline(md);
    morse_multi_destroy(md);
  }

  // waveform + spectrum from the file for the scope/spectrum panels
  pcm_.assign(pcm.samples, pcm.samples + pcm.count);
  spectrum_src_.assign(pcm_.begin(),
                       pcm_.begin() + std::min<size_t>(pcm_.size(), 16384));
  refreshSpectrum();
  const size_t target = 800;
  waveform_.clear();
  if (!pcm_.empty()) {
    size_t step = std::max<size_t>(1, pcm_.size() / target);
    for (size_t i = 0; i < pcm_.size(); i += step) {
      waveform_.push_back(pcm_[i]);
    }
  }
  mark_regions_.clear();
  audio_.setMarkRegions(mark_regions_);
  morse_pcm_free(&pcm);
  status_ = std::to_string(stations_.size()) + " station(s) from " + path;
}

// ---- drawing -------------------------------------------------------------

void App::draw() {
  drawMenuBar();

  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
      ImGuiWindowFlags_NoSavedSettings;
  ImGui::Begin("##workspace", nullptr, flags);

  float footer = ImGui::GetFrameHeightWithSpacing() + 4.0f;
  ImGui::BeginChild("##body", ImVec2(0, -footer));
  if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None)) {
    if (ImGui::BeginTabItem("Encode")) {
      drawEncodeTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Decode")) {
      drawDecodeTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Keyer")) {
      drawKeyerTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("CW Interface")) {
      drawCwTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Reference")) {
      drawReferenceTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Diagnostics")) {
      drawDiagnosticsTab();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::EndChild();

  drawStatusBar();
  ImGui::End();

  updateKeyer();
}

void App::drawMenuBar() {
  if (ImGui::BeginMainMenuBar()) {
    ImGui::TextColored(ImVec4(0.45f, 0.78f, 1.0f, 1.0f), "morsw");
    ImGui::Separator();
    if (ImGui::BeginMenu("Audio")) {
      if (ImGui::MenuItem("Rescan devices")) {
        audio_.refreshDevices();
      }
      ImGui::MenuItem("Allocation diagnostics", nullptr, &diagnostics_);
      if (diagnostics_) {
        morse_diagnostics_enable();
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
      ImGui::TextDisabled("Tabs:");
      ImGui::BulletText("Encode  - text to Morse + audio");
      ImGui::BulletText("Decode  - Morse text/audio to text (any tone)");
      ImGui::BulletText("Keyer   - straight / iambic paddle key");
      ImGui::BulletText("CW      - serial keying, cwdaemon, WinKeyer");
      ImGui::Separator();
      ImGui::Text("libmorse v%s", morse_version_string());
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
}

void App::drawStatusBar() {
  ImGui::Separator();
  ImGui::Text("Audio: %s @ %u Hz", audio_.ready() ? "ready" : "off", rate_);
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  ImGui::Text("ffmpeg: %s", ffmpeg_ok_ ? "yes" : "no");
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (capturing_) {
    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "CAPTURING");
  } else if (keyer_on_) {
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "KEYER ON");
  } else {
    ImGui::TextDisabled("idle");
  }
  if (cw_.cwdRunning()) {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "cwdaemon:%u",
                       cw_.cwdPort());
  }
  if (!status_.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("  -  %s", status_.c_str());
  }
}

// ---- shared fragments ----------------------------------------------------

void App::drawTimingControls(bool include_audio) {
  bool changed = false;
  ImGui::PushItemWidth(220);
  changed |= ImGui::SliderFloat("WPM", &wpm_, 5.0f, 60.0f, "%.0f");
  changed |= ImGui::SliderFloat("Farnsworth char WPM", &char_wpm_, 0.0f, 60.0f,
                                "%.0f (0=off)");
  changed |= ImGui::SliderFloat("Weight", &weight_, 0.25f, 0.75f, "%.2f");
  if (include_audio) {
    changed |= ImGui::SliderFloat("Tone (Hz)", &tone_, 100.0f, 2000.0f, "%.0f");
    changed |=
        ImGui::SliderFloat("Amplitude", &amplitude_, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Ramp (ms)", &ramp_, 0.0f, 20.0f, "%.1f");
    changed |= ImGui::SliderFloat("Noise", &noise_, 0.0f, 0.5f, "%.2f");
  }
  ImGui::PopItemWidth();
  if (changed) {
    doEncode();
    if (include_audio) {
      doRender();
    }
  }
}

void App::drawScope(float height) {
  if (!waveform_.empty()) {
    ImGui::PlotLines("##scope", waveform_.data(),
                     static_cast<int>(waveform_.size()), 0, "waveform", -1.0f,
                     1.0f, ImVec2(-1, height));
  } else {
    ImGui::TextDisabled("Render or load audio to see the waveform.");
  }
}

void App::drawSpectrum(float height) {
  if (!spectrum_.empty()) {
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "dominant: %.0f Hz (%.0f%%)",
                  detected_tone_, tone_strength_ * 100.0);
    ImGui::PlotHistogram("##spectrum", spectrum_.data(),
                         static_cast<int>(spectrum_.size()), 0, overlay, 0.0f,
                         1.0f, ImVec2(-1, height));
    ImGui::TextDisabled("0 - %.0f Hz", spectrum_max_hz_);
  } else {
    ImGui::TextDisabled("Spectrum appears while decoding audio.");
  }
}

bool App::drawDeviceSelector() {
  bool restart = false;
  const auto &caps = audio_.captureDevices();
  std::string preview =
      (capture_idx_ >= 0 && capture_idx_ < (int)caps.size())
          ? caps[capture_idx_].name
          : "System default input";
  ImGui::PushItemWidth(360);
  if (ImGui::BeginCombo("Input", preview.c_str())) {
    if (ImGui::Selectable("System default input", capture_idx_ < 0)) {
      capture_idx_ = -1;
      restart = true;
    }
    for (int i = 0; i < (int)caps.size(); ++i) {
      bool sel = (i == capture_idx_);
      std::string label =
          caps[i].name + (caps[i].is_default ? "  (default)" : "");
      if (ImGui::Selectable(label.c_str(), sel)) {
        capture_idx_ = i;
        restart = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::PopItemWidth();

  if (audio_.loopbackSupported()) {
    if (ImGui::Checkbox("Capture system output (loopback)", &use_loopback_)) {
      restart = true;
    }
    if (use_loopback_) {
      const auto &outs = audio_.playbackDevices();
      std::string op = (playback_idx_ >= 0 && playback_idx_ < (int)outs.size())
                           ? outs[playback_idx_].name
                           : "System default output";
      ImGui::SameLine();
      ImGui::PushItemWidth(280);
      if (ImGui::BeginCombo("Output", op.c_str())) {
        if (ImGui::Selectable("System default output", playback_idx_ < 0)) {
          playback_idx_ = -1;
          restart = true;
        }
        for (int i = 0; i < (int)outs.size(); ++i) {
          if (ImGui::Selectable(outs[i].name.c_str(), i == playback_idx_)) {
            playback_idx_ = i;
            restart = true;
          }
        }
        ImGui::EndCombo();
      }
      ImGui::PopItemWidth();
    }
  } else {
    ImGui::TextDisabled(
        "Tip: to decode system audio, pick a Monitor / Stereo Mix input.");
  }
  return restart;
}

// ---- tabs ----------------------------------------------------------------

void App::drawEncodeTab() {
  ImGui::Columns(2, "enc", false);

  ImGui::BeginChild("enc_left", ImVec2(0, 0));
  sectionLabel("Message");
  if (ImGui::Combo("Variant", &variant_idx_, "Base (A-Z 0-9)\0Extended\0")) {
    rebuildTables();
    doEncode();
    doRender();
  }
  if (ImGui::InputTextMultiline("##text", text_in_.data(), text_in_.size(),
                                ImVec2(-1, 90))) {
    doEncode();
  }
  ImGui::TextDisabled("Prosigns as <AR>, <SK>, <SOS>, <BT> ...");

  sectionLabel("Timing & tone");
  drawTimingControls(true);

  sectionLabel("Actions");
  if (ImGui::Button("Render")) {
    doRender();
  }
  ImGui::SameLine();
  if (ImGui::Button(audio_.isPlaying() ? "Stop" : "Play")) {
    if (audio_.isPlaying()) {
      audio_.stopPlayback();
    } else {
      doRender();
      doPlay();
    }
  }
  ImGui::SameLine();
  ImGui::PushItemWidth(180);
  ImGui::InputText("##exp", export_buf_.data(), export_buf_.size());
  ImGui::PopItemWidth();
  ImGui::SameLine();
  if (ImGui::Button("Export")) {
    doRender();
    exportMedia(export_buf_.data());
  }
  if (audio_.isPlaying()) {
    ImGui::ProgressBar(audio_.playProgress(), ImVec2(-1, 0));
  }
  ImGui::EndChild();

  ImGui::NextColumn();

  ImGui::BeginChild("enc_right", ImVec2(0, 0));
  sectionLabel("Morse");
  ImGui::BeginChild("code", ImVec2(-1, 120), ImGuiChildFlags_Borders);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(code_out_.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndChild();
  if (total_ms_ > 0.0) {
    ImGui::TextDisabled("duration %.2f s   %zu samples @ %d Hz",
                        total_ms_ / 1000.0, pcm_.size(), rate_);
  }
  sectionLabel("Waveform");
  drawScope(150);
  ImGui::EndChild();

  ImGui::Columns(1);
}

void App::drawDecodeTab() {
  if (ImGui::BeginTabBar("dec_modes")) {
    if (ImGui::BeginTabItem("From text")) {
      sectionLabel("Morse in  (., -, space, /)");
      if (ImGui::InputTextMultiline("##codein", code_in_.data(),
                                    code_in_.size(), ImVec2(-1, 90))) {
        doDecodeText();
      }
      if (ImGui::Button("Decode")) {
        doDecodeText();
      }
      sectionLabel("Text out");
      ImGui::BeginChild("txt", ImVec2(-1, 120), ImGuiChildFlags_Borders);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted(text_out_.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("From audio")) {
      sectionLabel("Tone detection (handles any pitch)");
      ImGui::PushItemWidth(200);
      ImGui::SliderFloat("Manual tone (Hz)", &manual_tone_, 0.0f, 2000.0f,
                         manual_tone_ < 1.0f ? "auto" : "%.0f");
      ImGui::SliderFloat("Band min (Hz)", &tone_min_, 50.0f, 1500.0f, "%.0f");
      ImGui::SliderFloat("Band max (Hz)", &tone_max_, 500.0f, 6000.0f, "%.0f");
      ImGui::PopItemWidth();
      ImGui::Checkbox("Track pitch drift", &track_tone_);

      sectionLabel("File");
      ImGui::PushItemWidth(420);
      ImGui::InputText("##path", path_buf_.data(), path_buf_.size());
      ImGui::PopItemWidth();
      ImGui::SameLine();
      if (ImGui::Button("Decode file")) {
        std::string p = path_buf_.data();
        bool isWav =
            p.size() >= 4 && (p.compare(p.size() - 4, 4, ".wav") == 0 ||
                              p.compare(p.size() - 4, 4, ".WAV") == 0);
        if (isWav || !ffmpeg_ok_) {
          decodeFileMulti(p);
        } else {
          importMedia(p);
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Play loaded")) {
        audio_.play(pcm_);
      }

      sectionLabel("Live (microphone / system audio)");
      bool restart = drawDeviceSelector();
      bool cap = capturing_;
      if (ImGui::Checkbox("Listen", &cap)) {
        if (cap) {
          startCapture();
        } else {
          stopCapture();
        }
      } else if (restart && capturing_) {
        startCapture();
      }

      sectionLabel("Spectrum");
      drawSpectrum(110);
      if (capturing_) {
        ImGui::ProgressBar(std::min(1.0f, live_power_), ImVec2(-1, 0),
                           "level");
      }

      sectionLabel("Stations");
      if (stations_.empty()) {
        ImGui::TextDisabled(
            "Each detected station (by pitch) is decoded separately.");
      }
      ImGui::BeginChild("stations", ImVec2(-1, 160), ImGuiChildFlags_Borders);
      for (const auto &s : stations_) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
                           "%.0f Hz  -  %.0f WPM", s.hz, s.wpm);
        ImGui::SameLine();
        ImGui::TextDisabled("(#%d)", s.id);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(s.text.empty() ? "..." : s.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Separator();
      }
      ImGui::EndChild();

      sectionLabel("Timeline (what was sent, in order)");
      drawTimeline(150);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
}

void App::drawKeyerTab() {
  bool want = keyer_on_;
  if (ImGui::Checkbox("Keyer enabled", &want)) {
    if (want) {
      startKeyer();
    } else {
      stopKeyer();
    }
  }
  ImGui::SameLine();
  int modeMirror = use_iambic_ ? 1 : 0;
  ImGui::PushItemWidth(120);
  if (ImGui::Combo("Mode", &modeMirror, "Straight\0Iambic\0")) {
    use_iambic_ = (modeMirror == 1);
    if (keyer_on_) {
      startKeyer();
    }
  }
  ImGui::PopItemWidth();
  if (use_iambic_) {
    ImGui::SameLine();
    ImGui::PushItemWidth(80);
    if (ImGui::Combo("Curtis", &iambic_mode_idx_, "A\0B\0")) {
      if (keyer_on_) {
        startKeyer();
      }
    }
    ImGui::PopItemWidth();
  }

  ImGui::TextDisabled(use_iambic_
                          ? "Paddles: Z / Left = dit, X / Right = dah."
                          : "Straight key: hold SPACE or the button.");

  sectionLabel("Key");
  if (use_iambic_) {
    ImGui::Button("DIT (hold)", ImVec2(150, 56));
    dit_btn_ = ImGui::IsItemActive();
    ImGui::SameLine();
    ImGui::Button("DAH (hold)", ImVec2(150, 56));
    dah_btn_ = ImGui::IsItemActive();
    straight_btn_ = false;
  } else {
    ImGui::Button("KEY (hold)", ImVec2(-1, 56));
    straight_btn_ = ImGui::IsItemActive();
    dit_btn_ = dah_btn_ = false;
  }

  bool on = keyer_on_ && prev_key_;
  ImVec2 p = ImGui::GetCursorScreenPos();
  float r = 22.0f;
  ImVec2 c = ImVec2(p.x + r + 4, p.y + r + 8);
  ImU32 col = on ? IM_COL32(255, 210, 70, 255) : IM_COL32(60, 60, 70, 255);
  ImGui::GetWindowDrawList()->AddCircleFilled(c, r, col, 32);
  ImGui::Dummy(ImVec2(2 * r + 10, 2 * r + 14));
  ImGui::SameLine();
  if (keyer_on_ && keyer_dec_ != nullptr) {
    ImGui::Text("  %.1f WPM  (dit %.0f ms)",
                morse_stream_decoder_wpm(keyer_dec_),
                morse_stream_decoder_dit_ms(keyer_dec_));
  }

  sectionLabel("Decoded");
  if (ImGui::Button("Clear")) {
    keyer_text_.clear();
    if (keyer_on_) {
      startKeyer();
    }
  }
  ImGui::BeginChild("keyout", ImVec2(-1, 120), ImGuiChildFlags_Borders);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(keyer_text_.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndChild();
}

void App::drawCwTab() {
  ImGui::TextWrapped(
      "Drive real CW hardware and protocols. Serial keying and the cwdaemon "
      "server use OS I/O and report when unsupported on this platform.");

  sectionLabel("Serial line keyer (RTS/DTR + PTT)");
  if (!cw_.serialSupported()) {
    ImGui::TextDisabled("Serial keying is not supported in this build/platform.");
  }
  ImGui::PushItemWidth(260);
  ImGui::InputText("Device", cw_device_.data(), cw_device_.size());
  ImGui::PopItemWidth();
  ImGui::SameLine();
  ImGui::PushItemWidth(180);
  ImGui::Combo("Key line", &keyline_idx_,
               "DTR keys / RTS PTT\0RTS keys / DTR PTT\0");
  ImGui::PopItemWidth();
  ImGui::BeginDisabled(!cw_.serialSupported() || cw_.serialBusy());
  if (ImGui::Button("Send via serial")) {
    morse_timing_t tm;
    morse_durations_t dur;
    morse_timing_default(&tm);
    tm.wpm = wpm_;
    tm.char_wpm = char_wpm_;
    tm.weight = weight_;
    morse_timing_resolve(&tm, &dur);
    cw_.sendSerial(cw_device_.data(),
                   keyline_idx_ == 0 ? MORSE_SERIAL_KEY_DTR
                                     : MORSE_SERIAL_KEY_RTS,
                   variant(), text_in_.data(), dur);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled(cw_.serialBusy() ? "sending..." : "uses the Encode text");

  sectionLabel("cwdaemon-compatible UDP server");
  if (!cw_.cwdSupported()) {
    ImGui::TextDisabled("UDP server is not supported in this build/platform.");
  }
  ImGui::PushItemWidth(160);
  ImGui::InputInt("Port", &cwd_port_);
  ImGui::PopItemWidth();
  ImGui::SameLine();
  ImGui::BeginDisabled(!cw_.cwdSupported());
  if (!cw_.cwdRunning()) {
    if (ImGui::Button("Start server")) {
      morse_cwd_config_t cfg;
      morse_cwd_config_default(&cfg);
      cfg.port = static_cast<unsigned short>(cwd_port_);
      cfg.variant = variant();
      cfg.wpm = wpm_;
      cfg.tone_hz = tone_;
      cfg.serial_device =
          (cw_device_[0] != 0 && cw_.serialSupported()) ? cw_device_.data()
                                                        : nullptr;
      cfg.keyline = keyline_idx_ == 0 ? MORSE_SERIAL_KEY_DTR
                                      : MORSE_SERIAL_KEY_RTS;
      cw_.startCwd(cfg);
    }
  } else {
    if (ImGui::Button("Stop server")) {
      cw_.stopCwd();
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled(cw_.cwdRunning()
                          ? "running - send UDP text to this port"
                          : "off");

  sectionLabel("WinKeyer (K1EL) host-mode byte stream");
  ImGui::PushItemWidth(360);
  if (ImGui::InputText("Text", wk_text_.data(), wk_text_.size())) {
    wk_preview_.clear();
  }
  ImGui::PopItemWidth();
  ImGui::SameLine();
  if (ImGui::Button("Build bytes")) {
    buildWinkeyerPreview();
  }
  if (!wk_preview_.empty()) {
    ImGui::BeginChild("wk", ImVec2(-1, 80), ImGuiChildFlags_Borders);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(wk_preview_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
  }

  if (!cw_.status().empty()) {
    ImGui::Separator();
    ImGui::TextDisabled("CW: %s", cw_.status().c_str());
  }
}

void App::drawReferenceTab() {
  int mode = tree_view_ ? 0 : 1;
  if (ImGui::RadioButton("Tree view", &mode, 0)) {
    tree_view_ = true;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Table view", &mode, 1)) {
    tree_view_ = false;
  }
  ImGui::Separator();

  if (tree_view_) {
    drawTree();
    return;
  }

  static char filter[64] = {0};
  ImGui::PushItemWidth(260);
  ImGui::InputText("Filter", filter, sizeof(filter));
  ImGui::PopItemWidth();
  ImGui::SameLine();
  ImGui::TextDisabled("%zu entries",
                      table_ ? morse_table_size(table_) : (size_t)0);

  if (table_ != nullptr &&
      ImGui::BeginTable("ref", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Symbol");
    ImGui::TableSetupColumn("Morse");
    ImGui::TableSetupColumn("Code");
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    std::size_t n = morse_table_size(table_);
    for (std::size_t i = 0; i < n; ++i) {
      const morse_entry_t *e = morse_table_entry_at(table_, i);
      if (e == nullptr) {
        continue;
      }
      char sym[24];
      if (e->codepoint != 0) {
        std::snprintf(sym, sizeof(sym), "%s", e->name);
      } else {
        std::snprintf(sym, sizeof(sym), "<%s>", e->name);
      }
      if (filter[0] != 0 && std::strstr(sym, filter) == nullptr &&
          std::strstr(e->pattern, filter) == nullptr) {
        continue;
      }
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(sym);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", e->pattern);
      ImGui::TableSetColumnIndex(2);
      if (e->codepoint != 0) {
        ImGui::Text("U+%04X", e->codepoint);
      } else {
        ImGui::TextDisabled("prosign");
      }
    }
    ImGui::EndTable();
  }
}

void App::drawDiagnosticsTab() {
  sectionLabel("Allocation counters");
  morse_alloc_stats_t s;
  if (diagnostics_ && morse_diagnostics_get(&s)) {
    if (ImGui::BeginTable("diag", 2, ImGuiTableFlags_SizingFixedFit)) {
      auto row = [](const char *k, size_t v) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(k);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%zu", v);
      };
      row("allocations", s.allocations);
      row("frees", s.frees);
      row("reallocations", s.reallocations);
      row("failed allocations", s.failed_allocations);
      row("live bytes", s.bytes_live);
      row("peak bytes", s.bytes_peak);
      row("total bytes", s.bytes_total);
      size_t outstanding =
          s.allocations >= s.frees ? s.allocations - s.frees : 0;
      row("outstanding blocks", outstanding);
      ImGui::EndTable();
    }
    ImGui::TextDisabled("Covers libmorse and the ds container nodes it routes");
    ImGui::TextDisabled("through its instrumented allocator.");
  } else {
    ImGui::TextDisabled("Enable diagnostics from the Audio menu.");
  }

  sectionLabel("Build");
  ImGui::Text("libmorse %s", morse_version_string());
  ImGui::Text("serial keying: %s",
              cw_.serialSupported() ? "supported" : "unsupported");
  ImGui::Text("UDP server: %s",
              cw_.cwdSupported() ? "supported" : "unsupported");
  ImGui::Text("output loopback: %s",
              audio_.loopbackSupported() ? "native (WASAPI)"
                                         : "via monitor input");
}

// The dichotomic Morse tree rendered with the draw list: from the root, an
// upward branch is a dot and a downward branch is a dash; each node shows the
// character formed by the path taken to reach it.
void App::drawTree() {
  if (table_ == nullptr) {
    return;
  }
  static int maxDepth = 4;
  ImGui::PushItemWidth(160);
  ImGui::SliderInt("Depth", &maxDepth, 1, 5);
  ImGui::PopItemWidth();
  ImGui::SameLine();
  ImGui::TextDisabled("up = dot  -  down = dash");

  const int leaves = 1 << maxDepth;
  ImVec2 avail = ImGui::GetContentRegionAvail();
  // Size the tree to the visible area so it fits without scrolling at typical
  // depths; fall back to scrolling only when it genuinely cannot fit.
  float leafH = avail.y > 0 ? (avail.y - 6.0f) / (float)leaves : 36.0f;
  if (leafH < 20.0f) {
    leafH = 20.0f;
  }
  if (leafH > 60.0f) {
    leafH = 60.0f;
  }
  float xspace = avail.x > 0 ? (avail.x - 70.0f) / (float)(maxDepth + 1) : 110.0f;
  if (xspace < 70.0f) {
    xspace = 70.0f;
  }
  if (xspace > 150.0f) {
    xspace = 150.0f;
  }
  float canvasH = (float)leaves * leafH;
  float canvasW = (float)(maxDepth + 1) * xspace + 60.0f;

  ImGui::BeginChild("treecanvas", ImVec2(-1, -1), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_HorizontalScrollbar);
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImGui::Dummy(ImVec2(canvasW, canvasH)); // reserve scroll area

  std::function<void(const std::string &, int, float, float, float, float)> rec =
      [&](const std::string &pat, int depth, float yTop, float yBot, float px,
          float py) {
        float x = origin.x + 24.0f + (float)depth * xspace;
        float y = (yTop + yBot) * 0.5f;

        if (depth > 0) {
          dl->AddLine(ImVec2(px, py), ImVec2(x, y), IM_COL32(110, 130, 160, 200),
                      2.0f);
          const char *mark = (pat[pat.size() - 1] == '.') ? "." : "-";
          dl->AddText(ImVec2((px + x) * 0.5f - 3.0f, (py + y) * 0.5f - 18.0f),
                      IM_COL32(150, 170, 200, 255), mark);
        }

        char sym[8] = {0};
        const char *label = "";
        if (depth == 0) {
          label = "start";
        } else {
          const morse_entry_t *e = nullptr;
          if (morse_table_decode_pattern(table_, pat.c_str(), &e) == MORSE_OK &&
              e != nullptr) {
            std::snprintf(sym, sizeof(sym), "%s", e->name);
            label = sym;
          }
        }
        float r = 15.0f;
        ImU32 fill = depth == 0
                         ? IM_COL32(70, 80, 100, 255)
                         : (label[0] ? IM_COL32(40, 95, 150, 255)
                                     : IM_COL32(44, 47, 55, 255));
        dl->AddCircleFilled(ImVec2(x, y), r, fill, 24);
        dl->AddCircle(ImVec2(x, y), r, IM_COL32(18, 20, 26, 255), 24, 1.5f);
        if (label[0]) {
          ImVec2 ts = ImGui::CalcTextSize(label);
          dl->AddText(ImVec2(x - ts.x * 0.5f, y - ts.y * 0.5f),
                      IM_COL32(236, 240, 247, 255), label);
        }

        if (depth < maxDepth) {
          float ymid = (yTop + yBot) * 0.5f;
          rec(pat + ".", depth + 1, yTop, ymid, x, y);
          rec(pat + "-", depth + 1, ymid, yBot, x, y);
        }
      };
  rec("", 0, origin.y, origin.y + canvasH, origin.x, origin.y);
  ImGui::EndChild();
}

// A stable-ish colour per station id for the timeline.
static ImVec4 channelColor(int id) {
  static const ImVec4 pal[6] = {
      ImVec4(0.55f, 0.85f, 1.00f, 1.0f), ImVec4(0.60f, 0.90f, 0.60f, 1.0f),
      ImVec4(1.00f, 0.80f, 0.45f, 1.0f), ImVec4(0.90f, 0.62f, 0.92f, 1.0f),
      ImVec4(1.00f, 0.62f, 0.55f, 1.0f), ImVec4(0.60f, 0.85f, 0.85f, 1.0f)};
  return pal[((id % 6) + 6) % 6];
}

void App::pullTimeline(morse_multi_detector_t *md) {
  timeline_.clear();
  if (md == nullptr) {
    return;
  }
  size_t n = morse_multi_event_count(md);
  for (size_t i = 0; i < n; ++i) {
    morse_multi_event_t e;
    if (morse_multi_get_event(md, i, &e)) {
      timeline_.push_back(e);
    }
  }
}

// Chronological transcript across all stations: consecutive fragments from the
// same station (without a long pause) are merged into one timestamped line, so
// you can read who sent what, in order.
void App::drawTimeline(float height) {
  ImGui::BeginChild("timeline", ImVec2(-1, height), ImGuiChildFlags_Borders);
  if (timeline_.empty()) {
    ImGui::TextDisabled("Chronological transcript across all stations.");
  } else {
    size_t i = 0;
    while (i < timeline_.size()) {
      int id = timeline_[i].channel_id;
      double hz = timeline_[i].tone_hz;
      double t0 = timeline_[i].t_seconds;
      double lastT = t0;
      std::string line;
      while (i < timeline_.size() && timeline_[i].channel_id == id &&
             timeline_[i].t_seconds - lastT < 3.0) {
        line += timeline_[i].text;
        lastT = timeline_[i].t_seconds;
        ++i;
      }
      int mm = (int)(t0 / 60.0);
      int ss = (int)t0 % 60;
      ImGui::TextColored(channelColor(id), "[%02d:%02d] %4.0f Hz", mm, ss, hz);
      ImGui::SameLine();
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted(line.c_str());
      ImGui::PopTextWrapPos();
    }
    if (capturing_) {
      ImGui::SetScrollHereY(1.0f); // follow live
    }
  }
  ImGui::EndChild();
}
