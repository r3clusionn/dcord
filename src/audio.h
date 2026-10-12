// PipeWire capture and playback streams for voice (48 kHz, float).
#pragma once

#include <functional>
#include <string>

// on_capture: mono samples from the microphone (PipeWire thread).
// on_playback: fill `frames` interleaved stereo samples (PipeWire thread).
bool audio_start(std::function<void(const float *mono, int n)> on_capture,
		 std::function<void(float *stereo, int frames)> on_playback,
		 const std::string &input_target, const std::string &output_target, bool capture = true);
void audio_stop();
void audio_set_capture_active(bool on);  // pause the mic stream when muted
