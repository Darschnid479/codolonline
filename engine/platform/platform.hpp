#pragma once

// Project Dragon — Native PlayStation 4 (OrbisOS) & PC Debug Platform Foundation
// Provides native filesystem, memory tracking, multithreading, DualShock 4 input,
// 48kHz stereo audio output, crash logging, debug console, and performance monitoring.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dragon::platform
{
	enum ButtonMask : std::uint32_t
	{
		BTN_NONE      = 0,
		BTN_CROSS     = 1u << 0,
		BTN_CIRCLE    = 1u << 1,
		BTN_SQUARE    = 1u << 2,
		BTN_TRIANGLE  = 1u << 3,
		BTN_L1        = 1u << 4,
		BTN_R1        = 1u << 5,
		BTN_L2        = 1u << 6,
		BTN_R2        = 1u << 7,
		BTN_L3        = 1u << 8,
		BTN_R3        = 1u << 9,
		BTN_OPTIONS   = 1u << 10,
		BTN_TOUCHPAD  = 1u << 11,
		BTN_DPAD_UP   = 1u << 12,
		BTN_DPAD_DOWN = 1u << 13,
		BTN_DPAD_LEFT = 1u << 14,
		BTN_DPAD_RIGHT= 1u << 15,
	};

	struct InputState
	{
		float lx = 0.0f; // [-1, 1] left stick horizontal
		float ly = 0.0f; // [-1, 1] left stick vertical (+1 = forward/up)
		float rx = 0.0f; // [-1, 1] right stick horizontal
		float ry = 0.0f; // [-1, 1] right stick vertical (+1 = look up)
		float l2 = 0.0f; // [0, 1]
		float r2 = 0.0f; // [0, 1]
		std::uint32_t buttons = 0;
		std::uint32_t prev_buttons = 0;
		bool connected = false;

		bool down(std::uint32_t mask) const { return (buttons & mask) != 0; }
		bool pressed(std::uint32_t mask) const { return ((buttons & mask) != 0) && ((prev_buttons & mask) == 0); }
		bool released(std::uint32_t mask) const { return ((buttons & mask) == 0) && ((prev_buttons & mask) != 0); }
	};

	struct PlatformInfo
	{
		std::string platform_name;
		std::string cpu_arch;
		int cpu_cores = 1;
		std::uint64_t direct_memory_bytes = 0;
		std::string log_dir;
		std::string crash_log_path;
		std::string game_data_dir;
	};

	struct PerfMetrics
	{
		std::uint64_t frame_index = 0;
		double fps = 0.0;
		double frame_ms = 0.0;
		double update_ms = 0.0;
		double cull_ms = 0.0;
		double render_ms = 0.0;
		double present_ms = 0.0;
		std::uint32_t visible_surfaces = 0;
		std::uint32_t drawn_triangles = 0;
		std::uint32_t drawn_models = 0;
		std::uint64_t zone_memory_bytes = 0;
		std::uint64_t texture_memory_bytes = 0;
		std::uint64_t geometry_memory_bytes = 0;
		std::uint64_t gpu_memory_bytes = 0;
		std::string active_gpu_backend;
	};

	enum class MemTag : std::uint8_t
	{
		General = 0,
		Zones,
		Textures,
		Geometry,
		Collision,
		Audio,
		Gpu,
		Count
	};

	class MemoryStats
	{
	public:
		static MemoryStats& instance();
		void add(MemTag tag, std::int64_t delta);
		std::uint64_t bytes(MemTag tag) const;
		std::uint64_t total_bytes() const;

	private:
		std::atomic<std::int64_t> counters_[static_cast<std::size_t>(MemTag::Count)]{};
	};

	// High-resolution monotonic clock
	std::uint64_t time_now_ns();
	double time_now_sec();
	void sleep_us(std::uint32_t microseconds);

	// Lifecycle
	bool init(const char* override_game_dir = nullptr);
	void shutdown();
	const PlatformInfo& info();
	bool should_quit();
	void request_quit();

	// File system (PS4 OrbisOS safe — uses POSIX fopen/opendir/stat rather than musl *at stubs)
	bool file_exists(const std::string& path);
	bool dir_exists(const std::string& path);
	bool ensure_dir(const std::string& path);
	bool read_file_bytes(const std::string& path, std::vector<std::uint8_t>& out);
	bool write_file_bytes(const std::string& path, const void* data, std::size_t size);
	std::vector<std::string> list_dir_files(const std::string& dir, std::string_view extension_lower = "");
	std::string locate_game_install(const char* hint_dir = nullptr);

	// Direct memory helpers (real sceKernelAllocateDirectMemory on PS4, aligned malloc on PC)
	struct DirectMemBlock
	{
		void* ptr = nullptr;
		std::int64_t phys_offset = 0;
		std::size_t size = 0;
	};
	bool alloc_direct_memory(std::size_t size, std::size_t alignment, DirectMemBlock& out);
	void free_direct_memory(DirectMemBlock& blk);

	// Multithreading job pool (native pthread on PS4 & POSIX, std::thread on Windows)
	class ThreadPool
	{
	public:
		explicit ThreadPool(int worker_count = 0);
		~ThreadPool();
		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;

		int worker_count() const { return worker_count_; }
		void parallel_for(int count, const std::function<void(int index)>& fn);

	private:
		struct Impl;
		Impl* impl_ = nullptr;
		int worker_count_ = 1;
	};

	// DualShock 4 / PC input
	bool input_init();
	void input_shutdown();
	void input_poll(InputState& state);
	void input_set_lightbar(std::uint8_t r, std::uint8_t g, std::uint8_t b);
	void input_set_rumble(std::uint8_t small_motor, std::uint8_t large_motor);

	// Native 48kHz stereo S16 audio output & mixer
	bool audio_init();
	void audio_shutdown();
	void audio_play_pcm_s16(const std::int16_t* samples, std::size_t frame_count, int channels, int sample_rate, float gain = 0.8f);
	void audio_play_tone(float freq_hz, float duration_sec, float gain = 0.15f);
	bool audio_is_ready();

	// Crash logging & boot trace
	void crash_logger_init(const std::string& crash_log_path);
	void crash_logger_trace(const char* line);
	void log_info(const char* fmt, ...);
	void log_warn(const char* fmt, ...);
	void log_error(const char* fmt, ...);

	// In-game Debug Console & Dvar registry
	struct Dvar
	{
		std::string name;
		std::string value;
		std::string description;
		float as_float() const;
		int as_int() const;
		bool as_bool() const;
	};

	class DebugConsole
	{
	public:
		using CommandFn = std::function<std::string(const std::vector<std::string>& args)>;

		static DebugConsole& instance();

		void print(std::string line);
		void register_dvar(const std::string& name, const std::string& default_val, const std::string& desc);
		bool set_dvar(const std::string& name, const std::string& val);
		const Dvar* find_dvar(const std::string& name) const;
		float dvar_float(const std::string& name, float fallback = 0.0f) const;
		int dvar_int(const std::string& name, int fallback = 0) const;
		bool dvar_bool(const std::string& name, bool fallback = false) const;

		void register_command(const std::string& name, CommandFn fn, const std::string& help);
		std::string execute(const std::string& line);
		std::vector<std::string> recent_lines(std::size_t max_lines) const;

		bool visible() const { return visible_.load(); }
		void set_visible(bool v) { visible_.store(v); }
		void toggle_visible() { visible_.store(!visible_.load()); }

	private:
		DebugConsole();
		mutable std::mutex mutex_;
		std::vector<std::string> lines_;
		std::unordered_map<std::string, Dvar> dvars_;
		struct CmdEntry { CommandFn fn; std::string help; };
		std::unordered_map<std::string, CmdEntry> commands_;
		std::atomic<bool> visible_{ false };
	};
}
