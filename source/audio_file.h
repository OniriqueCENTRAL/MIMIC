// MIMIC — décodage de fichiers audio (WAV / FLAC / MP3) vers un AudioClip. Thread principal uniquement (E/S disque).
#pragma once
#include "dsp/fileplayer.h"
#include <filesystem>
#include <memory>
#include <string>

namespace mimic {

constexpr double kMaxClipSeconds = 180.0;

// Renvoie nullptr et remplit `error` en cas d'échec.
std::unique_ptr<dsp::AudioClip> loadAudioClip (const std::filesystem::path& path, std::string& error);

} // namespace mimic
