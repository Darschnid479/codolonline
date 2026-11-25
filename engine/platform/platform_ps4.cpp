#if defined(DRAGON_PS4) || defined(__PS4__) || defined(__ORBIS__)

#include "platform.hpp"

#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dragon::platform
{
	namespace
	{
		PlatformInfo g_info;
		std::atomic<bool> g_quit{ false };
		std::uint64_t g_start_us = 0;

		std::string lower_ascii(std::string s)
		{
			for (char& c : s)
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
			return s;
		}
	}

	std::uint64_t time_now_ns()
	{
		// sceKernelGetProcessTime returns microseconds since process launch
		return sceKernelGetProcessTime() * 1000ULL;
	}

	double time_now_sec()
	{
		const std::uint64_t now_us = sceKernelGetProcessTime();
		if (g_start_us == 0) g_start_us = now_us;
		return static_cast<double>(now_us - g_start_us) * 1e-6;
	}

	void sleep_us(std::uint32_t microseconds)
	{
		sceKernelUsleep(microseconds);
	}

	bool file_exists(const std::string& path)
	{
		struct stat st{};
		return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
	}

	bool dir_exists(const std::string& path)
	{
		struct stat st{};
		return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
	}

	bool ensure_dir(const std::string& path)
	{
		if (path.empty()) return false;
		if (dir_exists(path)) return true;
		std::string cur;
		for (std::size_t i = 0; i < path.size(); ++i)
		{
			cur.push_back(path[i]);
			if (path[i] == '/' && cur.size() > 1)
				mkdir(cur.c_str(), 0777);
		}
		return mkdir(path.c_str(), 0777) == 0 || dir_exists(path);
	}

	bool read_file_bytes(const std::string& path, std::vector<std::uint8_t>& out)
	{
		out.clear();
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if (!f) return false;
		if (fseeko(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
		const off_t sz = ftello(f);
		if (sz < 0 || fseeko(f, 0, SEEK_SET) != 0) { std::fclose(f); return false; }
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
		DIR* d = opendir(dir.c_str());
		if (!d) return out;
		while (struct dirent* ent = readdir(d))
		{
			std::string name(ent->d_name);
			if (name == "." || name == "..") continue;
			if (!extension_lower.empty())
			{
				const auto dot = name.rfind('.');
				if (dot == std::string::npos) continue;
				if (lower_ascii(name.substr(dot)) != extension_lower) continue;
			}
			out.push_back(std::move(name));
		}
		closedir(d);
		std::sort(out.begin(), out.end());
		return out;
	}

	std::string locate_game_install(const char* hint_dir)
	{
		std::vector<std::string> candidates;
		if (hint_dir && hint_dir[0] != '\0')
			candidates.emplace_back(hint_dir);
		candidates.emplace_back("/data/codol");
		candidates.emplace_back("/data/project_dragon/game");
		candidates.emplace_back("/usb0/codol");
		candidates.emplace_back("/usb1/codol");
		candidates.emplace_back("/app0/codol");

		for (const auto& dir : candidates)
		{
			if (file_exists(dir + "/codoMP_client_shipRetail.exe") ||
			    file_exists(dir + "/C1.exe") ||
			    dir_exists(dir + "/client_shipRetail"))
			{
				return dir;
			}
		}
		return "/data/codol";
	}

	bool alloc_direct_memory(std::size_t size, std::size_t alignment, DirectMemBlock& out)
	{
		out = {};
		if (alignment < 0x10000) alignment = 0x10000;
		const std::size_t aligned = (size + alignment - 1) / alignment * alignment;
		off_t phys = 0;
		const std::size_t total_dmem = sceKernelGetDirectMemorySize();
		int32_t rc = sceKernelAllocateDirectMemory(0, static_cast<off_t>(total_dmem), aligned, alignment, 3 /* WB_ONION */, &phys);
		if (rc < 0) return false;
		void* addr = nullptr;
		rc = sceKernelMapDirectMemory(&addr, aligned, 0x33 /* CPU RW + GPU RW */, 0, phys, alignment);
		if (rc < 0)
		{
			sceKernelReleaseDirectMemory(phys, aligned);
			return false;
		}
		out.ptr = addr;
		out.phys_offset = static_cast<std::int64_t>(phys);
		out.size = aligned;
		MemoryStats::instance().add(MemTag::Gpu, static_cast<std::int64_t>(aligned));
		return true;
	}

	void free_direct_memory(DirectMemBlock& blk)
	{
		if (blk.size > 0)
		{
			sceKernelReleaseDirectMemory(static_cast<off_t>(blk.phys_offset), blk.size);
			MemoryStats::instance().add(MemTag::Gpu, -static_cast<std::int64_t>(blk.size));
		}
		blk = {};
	}

	// =========================================================================
	// Native PS4 pthread worker pool (AMD Jaguar 6-core friendly)
	// =========================================================================
	struct ThreadPool::Impl
	{
		std::vector<pthread_t> threads;
		pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
		pthread_cond_t cv_work = PTHREAD_COND_INITIALIZER;
		pthread_cond_t cv_done = PTHREAD_COND_INITIALIZER;
		bool stop = false;
		std::uint64_t generation = 0;
		int total_items = 0;
		std::atomic<int> next_item{ 0 };
		int active_workers = 0;
		const std::function<void(int)>* task = nullptr;

		static void* worker_main(void* arg)
		{
			auto* self = static_cast<Impl*>(arg);
			std::uint64_t seen_gen = 0;
			for (;;)
			{
				pthread_mutex_lock(&self->mtx);
				while (!self->stop && self->generation == seen_gen)
					pthread_cond_wait(&self->cv_work, &self->mtx);
				if (self->stop)
				{
					pthread_mutex_unlock(&self->mtx);
					return nullptr;
				}
				seen_gen = self->generation;
				const auto* fn = self->task;
				const int total = self->total_items;
				pthread_mutex_unlock(&self->mtx);

				for (;;)
				{
					const int idx = self->next_item.fetch_add(1, std::memory_order_relaxed);
					if (idx >= total) break;
					(*fn)(idx);
				}

				pthread_mutex_lock(&self->mtx);
				if (--self->active_workers == 0)
					pthread_cond_signal(&self->cv_done);
				pthread_mutex_unlock(&self->mtx);
			}
		}
	};

	ThreadPool::ThreadPool(int worker_count)
	{
		if (worker_count <= 0) worker_count = 4; // 4 background workers + main thread on PS4 Jaguar
		if (worker_count > 6) worker_count = 6;
		worker_count_ = worker_count;
		impl_ = new Impl();
		impl_->threads.resize(static_cast<std::size_t>(worker_count_));
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 512 * 1024);
		for (int i = 0; i < worker_count_; ++i)
			pthread_create(&impl_->threads[static_cast<std::size_t>(i)], &attr, &Impl::worker_main, impl_);
		pthread_attr_destroy(&attr);
	}

	ThreadPool::~ThreadPool()
	{
		if (!impl_) return;
		pthread_mutex_lock(&impl_->mtx);
		impl_->stop = true;
		pthread_cond_broadcast(&impl_->cv_work);
		pthread_mutex_unlock(&impl_->mtx);
		for (pthread_t t : impl_->threads)
			pthread_join(t, nullptr);
		pthread_cond_destroy(&impl_->cv_work);
		pthread_cond_destroy(&impl_->cv_done);
		pthread_mutex_destroy(&impl_->mtx);
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
		pthread_mutex_lock(&impl_->mtx);
		impl_->task = &fn;
		impl_->total_items = count;
		impl_->next_item.store(0, std::memory_order_relaxed);
		impl_->active_workers = worker_count_;
		++impl_->generation;
		pthread_cond_broadcast(&impl_->cv_work);
		pthread_mutex_unlock(&impl_->mtx);

		// Main thread also participates in work-stealing
		for (;;)
		{
			const int idx = impl_->next_item.fetch_add(1, std::memory_order_relaxed);
			if (idx >= count) break;
			fn(idx);
		}

		pthread_mutex_lock(&impl_->mtx);
		while (impl_->active_workers > 0)
			pthread_cond_wait(&impl_->cv_done, &impl_->mtx);
		impl_->task = nullptr;
		pthread_mutex_unlock(&impl_->mtx);
	}

	bool init(const char* override_game_dir)
	{
		setvbuf(stdout, nullptr, _IONBF, 0);
		setvbuf(stderr, nullptr, _IONBF, 0);

		g_start_us = sceKernelGetProcessTime();

		OrbisUserServiceInitializeParams user_params{};
		user_params.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
		sceUserServiceInitialize(&user_params);

		sceSystemServiceHideSplashScreen();

		g_info.platform_name = "PlayStation 4 (OrbisOS Native x86-64)";
		g_info.cpu_arch = "AMD Jaguar x86-64 (btver2)";
		g_info.cpu_cores = 6;
		g_info.direct_memory_bytes = sceKernelGetDirectMemorySize();
		g_info.log_dir = "/data/project_dragon";
		g_info.crash_log_path = "/data/project_dragon/crash.log";
		ensure_dir(g_info.log_dir);
		crash_logger_init(g_info.crash_log_path);

		g_info.game_data_dir = locate_game_install(override_game_dir);

		log_info("Project Dragon initialized on %s", g_info.platform_name.c_str());
		log_info("Direct memory available: %llu MiB",
			static_cast<unsigned long long>(g_info.direct_memory_bytes / (1024ULL * 1024ULL)));
		log_info("Game data root: %s", g_info.game_data_dir.c_str());

		input_init();
		audio_init();
		return true;
	}

	void shutdown()
	{
		log_info("Project Dragon PS4 shutdown initiated");
		audio_shutdown();
		input_shutdown();
		sceSystemServiceLoadExec("exit", nullptr);
	}

	const PlatformInfo& info() { return g_info; }
	bool should_quit() { return g_quit.load(std::memory_order_relaxed); }
	void request_quit() { g_quit.store(true, std::memory_order_relaxed); }
}

#endif
