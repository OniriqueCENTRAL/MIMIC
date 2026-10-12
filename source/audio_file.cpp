#include "audio_file.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>

#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#define DR_FLAC_NO_STDIO
#define DR_MP3_NO_STDIO
#include "third_party/dr_flac.h"
#include "third_party/dr_mp3.h"
#include "third_party/dr_wav.h"

namespace mimic {

namespace {
enum class Kind { Unknown, Wav, Flac, Mp3 };

Kind sniff (const std::vector<unsigned char>& d)
{
	if (d.size () < 12)
		return Kind::Unknown;
	if (!std::memcmp (d.data (), "RIFF", 4) || !std::memcmp (d.data (), "RF64", 4))
		return Kind::Wav;
	if (!std::memcmp (d.data (), "fLaC", 4))
		return Kind::Flac;
	if (!std::memcmp (d.data (), "ID3", 3) || (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0))
		return Kind::Mp3;
	return Kind::Unknown;
}
void freePcm (Kind k, float* p)
{
	if (!p)
		return;
	if (k == Kind::Wav) drwav_free (p, nullptr);
	else if (k == Kind::Flac) drflac_free (p, nullptr);
	else if (k == Kind::Mp3) drmp3_free (p, nullptr);
}
} // namespace

std::unique_ptr<dsp::AudioClip> loadAudioClip (const std::filesystem::path& path, std::string& error)
{
	std::ifstream f (path, std::ios::binary | std::ios::ate); // std::filesystem::path : chemins Unicode OK sous Windows
	if (!f)
	{
		error = "fichier illisible";
		return nullptr;
	}
	const auto size = static_cast<std::size_t> (f.tellg ());
	if (size < 64 || size > (300u << 20))
	{
		error = "taille de fichier non supportée";
		return nullptr;
	}
	std::vector<unsigned char> data (size);
	f.seekg (0);
	f.read (reinterpret_cast<char*> (data.data ()), static_cast<std::streamsize> (size));
	if (!f)
	{
		error = "lecture interrompue";
		return nullptr;
	}

	float* pcm = nullptr;
	Kind kind = sniff (data);
	unsigned channels = 0, sr = 0;
	std::uint64_t frames = 0;
	switch (kind)
	{
		case Kind::Wav:
		{
			drwav_uint64 fr = 0;
			pcm = drwav_open_memory_and_read_pcm_frames_f32 (data.data (), size, &channels, &sr, &fr, nullptr);
			frames = fr;
			break;
		}
		case Kind::Flac:
		{
			drflac_uint64 fr = 0;
			pcm = drflac_open_memory_and_read_pcm_frames_f32 (data.data (), size, &channels, &sr, &fr, nullptr);
			frames = fr;
			break;
		}
		case Kind::Mp3:
		{
			drmp3_config cfg {};
			drmp3_uint64 fr = 0;
			pcm = drmp3_open_memory_and_read_pcm_frames_f32 (data.data (), size, &cfg, &fr, nullptr);
			channels = cfg.channels;
			sr = cfg.sampleRate;
			frames = fr;
			break;
		}
		default:
			error = "format non supporté (WAV, FLAC ou MP3)";
			return nullptr;
	}
	if (!pcm || frames < 2 || channels == 0 || sr < 8000)
	{
		error = "décodage impossible";
		freePcm (kind, pcm);
		return nullptr;
	}

	auto clip = std::make_unique<dsp::AudioClip> ();
	clip->sampleRate = sr;
	const auto maxFrames = static_cast<std::uint64_t> (kMaxClipSeconds * sr);
	if (frames > maxFrames)
		frames = maxFrames;
	clip->l.resize (static_cast<std::size_t> (frames));
	clip->r.resize (static_cast<std::size_t> (frames));
	for (std::uint64_t i = 0; i < frames; ++i)
	{
		const float* p = pcm + i * channels;
		clip->l[i] = p[0];
		clip->r[i] = channels > 1 ? p[1] : p[0];
	}
	freePcm (kind, pcm);
	clip->name = path.filename ().string ();
	clip->computePeaks ();
	return clip;
}

} // namespace mimic
