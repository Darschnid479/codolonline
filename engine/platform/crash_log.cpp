#include "platform.hpp"

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dragon::platform
{
	namespace
	{
		constexpr std::size_t TRACE_RING_LINES = 64;
		constexpr std::size_t TRACE_LINE_MAX = 256;

		char g_crash_path[512] = "crash.log";
		char g_trace_ring[TRACE_RING_LINES][TRACE_LINE_MAX]{};
		std::atomic<std::uint32_t> g_trace_head{ 0 };
		std::mutex g_log_mutex;
		std::FILE* g_log_file = nullptr;

		void write_str_fd(int fd, const char* s)
		{
#if !defined(_WIN32)
			if (fd >= 0 && s)
			{
				const std::size_t len = std::strlen(s);
				(void)::write(fd, s, len);
			}
#else
			(void)fd;
			if (s) std::fputs(s, stderr);
#endif
		}

		void write_hex64_fd(int fd, std::uint64_t v)
		{
			char buf[20];
			buf[0] = '0';
			buf[1] = 'x';
			static constexpr char HEX[] = "0123456789ABCDEF";
			for (int i = 0; i < 16; ++i)
				buf[2 + i] = HEX[(v >> (60 - i * 4)) & 0xFu];
			buf[18] = '\0';
			write_str_fd(fd, buf);
		}

		void write_dec_fd(int fd, int v)
		{
			char buf[32];
			int pos = 0;
			if (v < 0) { buf[pos++] = '-'; v = -v; }
			char tmp[16];
			int t = 0;
			do { tmp[t++] = static_cast<char>('0' + (v % 10)); v /= 10; } while (v > 0);
			while (t > 0) buf[pos++] = tmp[--t];
			buf[pos] = '\0';
			write_str_fd(fd, buf);
		}

#if !defined(_WIN32)
		void dragon_signal_handler(int sig, siginfo_t* info, void* /*uctx*/)
		{
			const int fd = ::open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			const char* sig_name = "UNKNOWN";
			switch (sig)
			{
			case SIGSEGV: sig_name = "SIGSEGV"; break;
			case SIGBUS:  sig_name = "SIGBUS";  break;
			case SIGILL:  sig_name = "SIGILL";  break;
			case SIGFPE:  sig_name = "SIGFPE";  break;
			case SIGABRT: sig_name = "SIGABRT"; break;
			default: break;
			}
			write_str_fd(fd, "=== PROJECT DRAGON CRASH REPORT ===\n");
			write_str_fd(fd, "Signal: ");
			write_str_fd(fd, sig_name);
			write_str_fd(fd, " (");
			write_dec_fd(fd, sig);
			write_str_fd(fd, ")\n");
			if (info)
			{
				write_str_fd(fd, "Fault addr: ");
				write_hex64_fd(fd, reinterpret_cast<std::uintptr_t>(info->si_addr));
				write_str_fd(fd, "\nSi code: ");
				write_dec_fd(fd, info->si_code);
				write_str_fd(fd, "\n");
			}
			write_str_fd(fd, "\n--- Recent Boot / Execution Trace ---\n");
			const std::uint32_t head = g_trace_head.load(std::memory_order_relaxed);
			const std::uint32_t start = head > TRACE_RING_LINES ? head - TRACE_RING_LINES : 0;
			for (std::uint32_t i = start; i < head; ++i)
			{
				write_str_fd(fd, g_trace_ring[i % TRACE_RING_LINES]);
				write_str_fd(fd, "\n");
			}
			if (fd >= 0) ::close(fd);

			// Re-raise default handler
			struct sigaction sa{};
			sa.sa_handler = SIG_DFL;
			sigemptyset(&sa.sa_mask);
			sigaction(sig, &sa, nullptr);
			raise(sig);
		}
#endif

		void log_vprint(const char* level, const char* fmt, va_list ap)
		{
			char msg[1024];
			std::vsnprintf(msg, sizeof(msg), fmt, ap);

			char line[1100];
			const double sec = time_now_sec();
			std::snprintf(line, sizeof(line), "[%8.3f][%s] %s", sec, level, msg);

			crash_logger_trace(line);
			DebugConsole::instance().print(line);

			std::lock_guard<std::mutex> lock(g_log_mutex);
			std::fprintf(stderr, "%s\n", line);
			std::fflush(stderr);
			if (g_log_file)
			{
				std::fprintf(g_log_file, "%s\n", line);
				std::fflush(g_log_file);
			}
		}
	}

	MemoryStats& MemoryStats::instance()
	{
		static MemoryStats s;
		return s;
	}

	void MemoryStats::add(MemTag tag, std::int64_t delta)
	{
		counters_[static_cast<std::size_t>(tag)].fetch_add(delta, std::memory_order_relaxed);
	}

	std::uint64_t MemoryStats::bytes(MemTag tag) const
	{
		const auto v = counters_[static_cast<std::size_t>(tag)].load(std::memory_order_relaxed);
		return v > 0 ? static_cast<std::uint64_t>(v) : 0;
	}

	std::uint64_t MemoryStats::total_bytes() const
	{
		std::uint64_t sum = 0;
		for (std::size_t i = 0; i < static_cast<std::size_t>(MemTag::Count); ++i)
		{
			const auto v = counters_[i].load(std::memory_order_relaxed);
			if (v > 0) sum += static_cast<std::uint64_t>(v);
		}
		return sum;
	}

	void crash_logger_init(const std::string& crash_log_path)
	{
		std::snprintf(g_crash_path, sizeof(g_crash_path), "%s", crash_log_path.c_str());

		std::string run_log = crash_log_path;
		const auto pos = run_log.rfind('/');
		if (pos != std::string::npos)
			run_log = run_log.substr(0, pos + 1) + "dragon.log";
		else
			run_log = "dragon.log";

		{
			std::lock_guard<std::mutex> lock(g_log_mutex);
			if (g_log_file) std::fclose(g_log_file);
			g_log_file = std::fopen(run_log.c_str(), "w");
		}

#if !defined(_WIN32)
		struct sigaction sa{};
#if defined(DRAGON_PS4) || defined(__PS4__) || defined(__ORBIS__)
		sa.__sa_handler.__sa_sigaction = dragon_signal_handler;
#else
		sa.sa_sigaction = dragon_signal_handler;
#endif
		sa.sa_flags = SA_SIGINFO;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGSEGV, &sa, nullptr);
		sigaction(SIGBUS, &sa, nullptr);
		sigaction(SIGILL, &sa, nullptr);
		sigaction(SIGFPE, &sa, nullptr);
		sigaction(SIGABRT, &sa, nullptr);
#endif
		crash_logger_trace("crash_logger_init complete");
	}

	void crash_logger_trace(const char* line)
	{
		if (!line) return;
		const std::uint32_t idx = g_trace_head.fetch_add(1, std::memory_order_relaxed) % TRACE_RING_LINES;
		std::snprintf(g_trace_ring[idx], TRACE_LINE_MAX, "%s", line);
	}

	void log_info(const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		log_vprint("INFO", fmt, ap);
		va_end(ap);
	}

	void log_warn(const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		log_vprint("WARN", fmt, ap);
		va_end(ap);
	}

	void log_error(const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		log_vprint("ERR ", fmt, ap);
		va_end(ap);
	}
}
