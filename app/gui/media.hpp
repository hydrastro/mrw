// media.hpp - Optional ffmpeg-backed import/export of arbitrary audio formats.
//
// The core library reads and writes plain PCM .wav itself. To open an mp3 / ogg
// / flac / m4a or to export to a compressed format, we shell out to ffmpeg,
// transcoding through a temporary mono 16-bit WAV that libmorse understands.
// ffmpeg is entirely optional: if it is not on PATH these helpers report false
// and the GUI simply restricts itself to .wav.
#ifndef MORSE_GUI_MEDIA_HPP
#define MORSE_GUI_MEDIA_HPP

#include <string>

namespace media {

// True if an `ffmpeg` binary is callable on PATH.
bool ffmpegAvailable();

// Transcode any ffmpeg-readable file into a mono WAV at `sample_rate`.
// Returns true on success.
bool importToWav(const std::string &input, const std::string &wav_out,
                 unsigned int sample_rate);

// Transcode a WAV into any format ffmpeg infers from `output`'s extension.
bool exportFromWav(const std::string &wav_in, const std::string &output);

} // namespace media

#endif // MORSE_GUI_MEDIA_HPP
