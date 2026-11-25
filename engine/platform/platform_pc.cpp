#if !defined(DRAGON_PS4) && !defined(__PS4__) && !defined(__ORBIS__)

#include "platform.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <malloc.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dragon::platform
{
	namespace
	{
		PlatformInfo g_info;
		std::atomic<bool> g_quit{ false };
		const auto g_t0 = std::chrono::steady_clock::now();
		std::atomic<bool> g_audio_ready{ false };
		std::atomic<std::uint64_t> g_audio_played_frames{ 0 };

		std::string lower_ascii(std::string s)
		{
			for (char& c : s)
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
			return s;
		}
	}

	std::uint64_t time_now_ns()
	{
		const auto d = std::chrono::steady_clock::now() - g_t0;
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
	}

	double time_now_sec()
	{
		return static_cast<double>(time_now_ns()) * 1e-9;
	}

	void sleep_us(std::uint32_t microseconds)
	{
		std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
	}

	bool file_exists(const std::string& path)
	{
		std::error_code ec;
		return std::filesystem::is_regular_file(path, ec);
	}

	bool dir_exists(const std::string& path)
	{
		std::error_code ec;
		return std::filesystem::is_directory(path, ec);
	}

	bool ensure_dir(const std::string& path)
	{
		std::error_code ec;
		if (std::filesystem::is_directory(path, ec)) return true;
		return std::filesystem::create_directories(path, ec);
	}

	bool read_file_bytes(const std::string& path, std::vector<std::uint8_t>& out)
	{
		out.clear();
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if (!f) return false;
		std::fseek(f, 0, SEEK_END);
		const long sz = std::ftell(f);
		if (sz < 0) { std::fclose(f); return false; }
		std::fseek(f, 0, SEEK_SET);
		out.resize(static_cast<std::size_t>(sz));
		const std::size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
		std::fclose(f);
		return got == out.size();
	}

	bool write_file_bytes(const std::string& path, const void* data, std::size_t size)
	{
		std::FILE* f = std::fopen(path.c_str(), "wb");
		if (!f) return false;
		const std::size_t put = (size > 0 && data) ? std::fwrite(data, 1, size, f) : 0;
		std::fclose(f);
		return put == size;
	}

	std::vector<std::string> list_dir_files(const std::string& dir, std::string_view extension_lower)
	{
		std::vector<std::string> out;
		std::error_code ec;
		for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
		{
			if (!it->is_regular_file(ec)) continue;
			std::string name = it->path().filename().string();
			if (!extension_lower.empty())
			{
				const std::string ext = lower_ascii(it->path().extension().string());
				if (ext != extension_lower) continue;
			}
			out.push_back(std::move(name));
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	std::string locate_game_install(const char* hint_dir)
	{
		std::vector<std::string> candidates;
		if (hint_dir && hint_dir[0] != '\0')
			candidates.emplace_back(hint_dir);
		if (const char* env = std::getenv("CODOL_GAME_DIR"); env && env[0] != '\0')
			candidates.emplace_back(env);
		candidates.emplace_back("/home/antigravity/codol-game/extracted");
		candidates.emplace_back("/home/antigravity/codol-game/extracted/codol-classic");
		candidates.emplace_back("./codol");
		candidates.emplace_back("../codol");

		for (const auto& dir : candidates)
		{
			if (file_exists(dir + "/codoMP_client_shipRetail.exe") ||
			    file_exists(dir + "/C1.exe") ||
			    dir_exists(dir + "/client_shipRetail"))
			{
				return dir;
			}
		}
		return hint_dir ? std::string(hint_dir) : std::string();
	}

	bool alloc_direct_memory(std::size_t size, std::size_t alignment, DirectMemBlock& out)
	{
		out = {};
		if (alignment < 64) alignment = 64;
		const std::size_t aligned = (size + alignment - 1) / alignment * alignment;
#if defined(_WIN32)
		void* p = _aligned_malloc(aligned, alignment);
#else
		void* p = nullptr;
		if (posix_memalign(&p, alignment, aligned) != 0) p = nullptr;
#endif
		if (!p) return false;
		out.ptr = p;
		out.phys_offset = 0;
		out.size = aligned;
		MemoryStats::instance().add(MemTag::Gpu, static_cast<std::int64_t>(aligned));
		return true;
	}

	void free_direct_memory(DirectMemBlock& blk)
	{
		if (blk.ptr)
		{
#if defined(_WIN32)
			_aligned_free(blk.ptr);
#else
			std::free(blk.ptr);
#endif
			MemoryStats::instance().add(MemTag::Gpu, -static_cast<std::int64_t>(blk.size));
		}
		blk = {};
	}

	// =========================================================================
	// C++20 std::thread worker pool for PC Debug builds
	// =========================================================================
	struct ThreadPool::Impl
	{
		std::vector<std::thread> threads;
		std::mutex mtx;
		std::condition_variable cv_work;
		std::condition_variable cv_done;
		bool stop = false;
		std::uint64_t generation = 0;
		int total_items = 0;
		std::atomic<int> next_item{ 0 };
		int active_workers = 0;
		const std::function<void(int)>* task = nullptr;

		void worker_loop()
		{
			std::uint64_t seen_gen = 0;
			for (;;)
			{
				const std::function<void(int)>* fn = nullptr;
				int total = 0;
				{
					std::unique_lock<std::mutex> lock(mtx);
					cv_work.wait(lock, [&] { return stop || generation != seen_gen; });
					if (stop) return;
					seen_gen = generation;
					fn = task;
					total = total_items;
				}
				for (;;)
				{
					const int idx = next_item.fetch_add(1, std::memory_order_relaxed);
					if (idx >= total) break;
					(*fn)(idx);
				}
				{
					std::lock_guard<std::mutex> lock(mtx);
					if (--active_workers == 0)
						cv_done.notify_one();
				}
			}
		}
	};

	ThreadPool::ThreadPool(int worker_count)
	{
		if (worker_count <= 0)
		{
			const unsigned hc = std::thread::hardware_concurrency();
			worker_count = hc > 1 ? static_cast<int>(std::min<unsigned>(hc, 8u)) : 2;
		}
		worker_count_ = worker_count;
		impl_ = new Impl();
		for (int i = 0; i < worker_count_; ++i)
			impl_->threads.emplace_back(&Impl::worker_loop, impl_);
	}

	ThreadPool::~ThreadPool()
	{
		if (!impl_) return;
		{
			std::lock_guard<std::mutex> lock(impl_->mtx);
			impl_->stop = true;
		}
		impl_->cv_work.notify_all();
		for (auto& t : impl_->threads)
			if (t.joinable()) t.join();
		delete impl_;
		impl_ = nullptr;
	}

	void ThreadPool::parallel_for(int count, const std::function<void(int index)>& fn)
	{
		if (count <= 0) return;
		if (!impl_ || worker_count_ <= 1 || count == 1)
		{
			for (int i = 0; i < count; ++i) fn(i);
			return;
		}
		{
			std::lock_guard<std::mutex> lock(impl_->mtx);
			impl_->task = &fn;
			impl_->total_items = count;
			impl_->next_item.store(0, std::memory_order_relaxed);
			impl_->active_workers = worker_count_;
			++impl_->generation;
		}
		impl_->cv_work.notify_all();
		for (;;)
		{
			const int idx = impl_->next_item.fetch_add(1, std::memory_order_relaxed);
			if (idx >= count) break;
			fn(idx);
		}
		std::unique_lock<std::mutex> lock(impl_->mtx);
		impl_->cv_done.wait(lock, [&] { return impl_->active_workers == 0; });
		impl_->task = nullptr;
	}

	bool input_init() { return true; }
	void input_shutdown() {}
	void input_poll(InputState& state)
	{
		state.prev_buttons = state.buttons;
		state.connected = true;
	}
	void input_set_lightbar(std::uint8_t, std::uint8_t, std::uint8_t) {}
	void input_set_rumble(std::uint8_t, std::uint8_t) {}

	bool audio_init()
	{
		g_audio_ready.store(true);
		return true;
	}

	void audio_shutdown()
	{
		g_audio_ready.store(false);
	}

	void audio_play_pcm_s16(const std::int16_t* samples, std::size_t frame_count, int channels, int sample_rate, float)
	{
		if (samples && frame_count > 0 && channels > 0 && sample_rate > 0)
			g_audio_played_frames.fetch_add(frame_count, std::memory_order_relaxed);
	}

	void audio_play_tone(float, float duration_sec, float)
	{
		if (duration_sec > 0.0f)
			g_audio_played_frames.fetch_add(static_cast<std::uint64_t>(duration_sec * 48000.0f), std::memory_order_relaxed);
	}

	bool audio_is_ready() { return g_audio_ready.load(std::memory_order_relaxed); }

	bool init(const char* override_game_dir)
	{
#if defined(_WIN32)
		g_info.platform_name = "Windows PC Debug Host";
#else
		g_info.platform_name = "Linux PC Debug Host";
#endif
		g_info.cpu_arch = "x86-64";
		g_info.cpu_cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
		g_info.direct_memory_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
		g_info.log_dir = "logs";
		g_info.crash_log_path = "logs/crash.log";
		ensure_dir(g_info.log_dir);
		crash_logger_init(g_info.crash_log_path);
		g_info.game_data_dir = locate_game_install(override_game_dir);

		log_info("Project Dragon initialized on %s (%d cores)", g_info.platform_name.c_str(), g_info.cpu_cores);
		if (!g_info.game_data_dir.empty())
			log_info("Game data root: %s", g_info.game_data_dir.c_str());
		else
			log_warn("No CODOL 1.1.1.13 game directory found yet (pass --game <dir> or set CODOL_GAME_DIR)");

		input_init();
		audio_init();
		return true;
	}

	void shutdown()
	{
		audio_shutdown();
		input_shutdown();
	}

	const PlatformInfo& info() { return g_info; }
	bool should_quit() { return g_quit.load(std::memory_order_relaxed); }
	void request_quit() { g_quit.store(true, std::memory_order_relaxed); }
}

#endif
