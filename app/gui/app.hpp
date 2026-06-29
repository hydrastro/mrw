// app.hpp - The morse-deluxe GUI application.
//
// A single full-window workspace organised into tabs (Encode, Decode, Keyer,
// CW Interface, Reference, Diagnostics) with a persistent status bar, rather
// than a scatter of floating windows. All interaction with the C core
// (libmorse) and the CW interfaces is funnelled through this class.
#ifndef MORSE_GUI_APP_HPP
#define MORSE_GUI_APP_HPP

#include "audio.hpp"
#include "cwio.hpp"

#include "morse/morse.h"

#include <array>
#include <string>
#include <vector>
#include <utility>
#include <vector>

class App {
public:
  App();
  ~App();

  void init();    // bring up audio + decoders; enable diagnostics; apply style
  void onFrame(); // per-frame, non-UI work (drain capture, drive keyer, FFT)
  void draw();    // emit the workspace for this frame

private:
  // ---- layout ------------------------------------------------------------
  void applyStyle();
  void drawMenuBar();
  void drawStatusBar();
  void drawEncodeTab();
  void drawDecodeTab();
  void drawKeyerTab();
  void drawCwTab();
  void drawReferenceTab();
  void drawDiagnosticsTab();

  // shared UI fragments
  void drawTimingControls(bool include_audio);
  void drawSpectrum(float height);
  void drawScope(float height);
  bool drawDeviceSelector();

  // ---- actions -----------------------------------------------------------
  void rebuildTables();
  void doEncode();
  void doRender();
  void doPlay();
  void doDecodeText();
  void listenFile(const std::string &path);
  void importMedia(const std::string &path);
  void exportMedia(const std::string &path);
  void startCapture();
  void stopCapture();
  void decodeFileMulti(const std::string &path);
  void refreshStations();
  void pullTimeline(morse_multi_detector_t *md);
  void drawTimeline(float height);
  void pushWaterfallRow(const float *samples, size_t n);
  void drawWaterfall(float height);
  void drawTree();
  void startKeyer();
  void stopKeyer();
  void updateKeyer();         // drive straight/iambic key + sidetone + decode
  void refreshSpectrum();     // recompute FFT spectrum + dominant tone
  void buildWinkeyerPreview();

  morse_variant_t variant() const {
    return variant_idx_ == 0 ? MORSE_VARIANT_INTERNATIONAL
                             : MORSE_VARIANT_INTERNATIONAL_EXT;
  }
  void fillDetectOpts(morse_detect_opts_t *o) const;

  AudioEngine audio_;
  CwIo cw_;

  // shared parameters
  int variant_idx_ = 1; // 0 = base, 1 = extended
  float wpm_ = 20.0f;
  float char_wpm_ = 0.0f; // 0 disables Farnsworth
  float weight_ = 0.5f;
  float tone_ = 600.0f;
  float amplitude_ = 0.7f;
  float ramp_ = 5.0f;
  float noise_ = 0.0f;
  int rate_ = 44100;

  // encoder
  std::vector<char> text_in_;
  std::string code_out_;

  // decoder (text)
  std::vector<char> code_in_;
  std::string text_out_;

  // rendered audio + previews
  std::vector<float> pcm_;
  std::vector<float> waveform_;
  std::vector<std::pair<std::size_t, std::size_t>> mark_regions_;
  double total_ms_ = 0.0;

  // audio decode: detection parameters (handle "any tone")
  float manual_tone_ = 0.0f; // 0 => auto-detect
  float tone_min_ = 100.0f;
  float tone_max_ = 3000.0f;
  bool track_tone_ = true;

  // offline listen / live capture
  std::vector<float> envelope_; // 0..1 power per block, for meter
  std::string listen_out_;
  float live_power_ = 0.0f;

  // spectrum display + detected pitch
  std::vector<float> spectrum_;     // normalized magnitudes (display)
  std::vector<float> spectrum_src_; // rolling raw samples for the FFT
  double detected_tone_ = 0.0;
  double tone_strength_ = 0.0;
  float spectrum_max_hz_ = 1500.0f; // x-axis extent of the plot

  // device selection
  int capture_idx_ = -1;  // -1 = default
  int playback_idx_ = -1; // for loopback
  bool use_loopback_ = false;

  // live keyer
  bool keyer_on_ = false;
  bool use_iambic_ = false;
  morse_iambic_t iambic_;
  int iambic_mode_idx_ = 1; // 0 = A, 1 = B
  bool prev_key_ = false;
  double last_edge_ = 0.0;
  double keyer_accum_ms_ = 0.0; // leftover time for sub-stepping the keyer
  bool keyer_seen_mark_ = false;
  bool straight_btn_ = false; // on-screen key held (this frame)
  bool dit_btn_ = false;      // on-screen dit paddle held
  bool dah_btn_ = false;      // on-screen dah paddle held
  std::string keyer_text_;
  morse_stream_decoder_t *keyer_dec_ = nullptr;

  // microphone / system-audio capture decode (multi-station)
  bool capturing_ = false;
  morse_multi_detector_t *multi_ = nullptr;
  std::vector<float> cap_scratch_;

  // a decoded station, shown in the Decode tab (live or from a file)
  struct StationView {
    int id = 0;
    double hz = 0.0;
    double wpm = 0.0;
    std::string text;
  };
  std::vector<StationView> stations_;
  std::vector<morse_multi_event_t> timeline_; // chronological cross-station log
  bool simultaneous_ = false; // decode several tones at once vs strongest only
  bool show_waterfall_ = true; // tone view: waterfall vs line spectrum

  // waterfall / spectrogram history (ring of normalized magnitude rows)
  static const int kWfBins = 160;
  std::vector<std::array<float, 160>> wf_rows_;
  size_t wf_max_rows_ = 160;
  float wf_peak_ = 1e-6f; // decaying global peak for normalization

  bool tree_view_ = true; // Reference tab: tree vs table

  // CW interface panel
  std::vector<char> cw_device_;
  int keyline_idx_ = 0; // 0 = DTR keys, 1 = RTS keys
  int cwd_port_ = 6789;
  std::vector<char> wk_text_;
  std::string wk_preview_;

  // codebook table
  morse_table_t *table_ = nullptr;

  // diagnostics
  bool diagnostics_ = true;

  // media panel
  std::vector<char> path_buf_;
  std::vector<char> export_buf_;
  bool ffmpeg_ok_ = false;
  std::string status_;
};

#endif // MORSE_GUI_APP_HPP
