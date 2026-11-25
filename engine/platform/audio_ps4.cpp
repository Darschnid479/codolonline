#if defined(DRAGON_PS4) || defined(__PS4__) || defined(__ORBIS__)

#include "platform.hpp"

#include <orbis/AudioOut.h>
#include <orbis/UserService.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <vector>

namespace dragon::platform
{
	namespace
	{
		constexpr std::uint32_t AUDIO_GRAIN = 256;
		constexpr std::uint32_t AUDIO_RATE = 48000;

		struct ActiveVoice
		{
			std::vector<std::int16_t> stereo_48k;
			std::size_t cursor = 0;
			float gain = 1.0f;
		};

		int32_t g_audio_handle = -1;
		pthread_t g_audio_thread{};
		std::atomic<bool> g_audio_running{ false };
		std::mutex g_voice_mutex;
		std::vector<ActiveVoice> g_voices;

		void* audio_thread_main(void*)
		{
			std::int16_t mix_buf[AUDIO_GRAIN * 2];
			float accum[AUDIO_GRAIN * 2];

			while (g_audio_running.load(std::memory_order_relaxed))
			{
				std::memset(accum, 0, sizeof(accum));
				{
					std::lock_guard<std::mutex> lock(g_voice_mutex);
					for (auto it = g_voices.begin(); it != g_voices.end();)
					{
						const std::size_t avail_frames = (it->stereo_48k.size() / 2) - it->cursor;
						const std::size_t take = std::min<std::size_t>(AUDIO_GRAIN, avail_frames);
						for (std::size_t i = 0; i < take; ++i)
						{
							const std::size_t src = (it->cursor + i) * 2;
							accum[i * 2 + 0] += static_cast<float>(it->stereo_48k[src + 0]) * it->gain;
							accum[i * 2 + 1] += static_cast<float>(it->stereo_48k[src + 1]) * it->gain;
						}
						it->cursor += take;
						if (it->cursor >= it->stereo_48k.size() / 2)
							it = g_voices.erase(it);
						else
							++it;
					}
				}
				for (std::size_t i = 0; i < AUDIO_GRAIN * 2; ++i)
				{
					const float clamped = std::clamp(accum[i], -32767.0f, 32767.0f);
					mix_buf[i] = static_cast<std::int16_t>(clamped);
				}
				sceAudioOutOutput(g_audio_handle, mix_buf);
			}
			return nullptr;
		}
	}

	bool audio_init()
	{
		if (sceAudioOutInit() != 0)
		{
			log_warn("sceAudioOutInit failed");
			return false;
		}
		g_audio_handle = sceAudioOutOpen(
			ORBIS_USER_SERVICE_USER_ID_SYSTEM,
			ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
			0,
			AUDIO_GRAIN,
			AUDIO_RATE,
			ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
		if (g_audio_handle <= 0)
		{
			log_warn("sceAudioOutOpen failed (0x%08X)", static_cast<unsigned>(g_audio_handle));
			g_audio_handle = -1;
			return false;
		}
		g_audio_running.store(true);
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 256 * 1024);
		pthread_create(&g_audio_thread, &attr, &audio_thread_main, nullptr);
		pthread_attr_destroy(&attr);
		log_info("PS4 AudioOut initialized (48kHz S16 stereo, grain=%u, handle=%d)", AUDIO_GRAIN, g_audio_handle);
		return true;
	}

	void audio_shutdown()
	{
		if (g_audio_running.exchange(false))
		{
			pthread_join(g_audio_thread, nullptr);
		}
		if (g_audio_handle > 0)
		{
			sceAudioOutClose(g_audio_handle);
			g_audio_handle = -1;
		}
		std::lock_guard<std::mutex> lock(g_voice_mutex);
		g_voices.clear();
	}

	void audio_play_pcm_s16(const std::int16_t* samples, std::size_t frame_count, int channels, int sample_rate, float gain)
	{
		if (!samples || frame_count == 0 || channels <= 0 || sample_rate <= 0 || g_audio_handle <= 0)
			return;

		// Linear resample to 48kHz stereo
		const double ratio = static_cast<double>(sample_rate) / static_cast<double>(AUDIO_RATE);
		const std::size_t out_frames = static_cast<std::size_t>(static_cast<double>(frame_count) / ratio);
		if (out_frames == 0) return;

		ActiveVoice voice;
		voice.gain = gain;
		voice.stereo_48k.resize(out_frames * 2);
		for (std::size_t i = 0; i < out_frames; ++i)
		{
			const double src_f = static_cast<double>(i) * ratio;
			const std::size_t idx = std::min(static_cast<std::size_t>(src_f), frame_count - 1);
			if (channels == 1)
			{
				const std::int16_t s = samples[idx];
				voice.stereo_48k[i * 2 + 0] = s;
				voice.stereo_48k[i * 2 + 1] = s;
			}
			else
			{
				voice.stereo_48k[i * 2 + 0] = samples[idx * static_cast<std::size_t>(channels) + 0];
				voice.stereo_48k[i * 2 + 1] = samples[idx * static_cast<std::size_t>(channels) + 1];
			}
		}

		std::lock_guard<std::mutex> lock(g_voice_mutex);
		if (g_voices.size() < 32)
			g_voices.push_back(std::move(voice));
	}

	void audio_play_tone(float freq_hz, float duration_sec, float gain)
	{
		if (g_audio_handle <= 0 || duration_sec <= 0.0f) return;
		const std::size_t frames = static_cast<std::size_t>(duration_sec * static_cast<float>(AUDIO_RATE));
		ActiveVoice voice;
		voice.gain = gain;
		voice.stereo_48k.resize(frames * 2);
		constexpr float TWO_PI = 6.28318530718f;
		for (std::size_t i = 0; i < frames; ++i)
		{
			const float t = static_cast<float>(i) / static_cast<float>(AUDIO_RATE);
			const float env = std::min(1.0f, static_cast<float>(frames - i) / (AUDIO_RATE * 0.01f));
			const std::int16_t s = static_cast<std::int16_t>(std::sin(TWO_PI * freq_hz * t) * 28000.0f * env);
			voice.stereo_48k[i * 2 + 0] = s;
			voice.stereo_48k[i * 2 + 1] = s;
		}
		std::lock_guard<std::mutex> lock(g_voice_mutex);
		if (g_voices.size() < 32)
			g_voices.push_back(std::move(voice));
	}

	bool audio_is_ready()
	{
		return g_audio_handle > 0 && g_audio_running.load(std::memory_order_relaxed);
	}
}

#endif
