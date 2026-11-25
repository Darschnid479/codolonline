#pragma once

// Portable crypto and 64-bit file I/O helpers shared by codol_textures (iwd.cpp, wavelet.cpp, crypto_portable.cpp).
// Uses codol::zone::crypto (pure C++20 SHA-256, AES-192-CTR, RSA-2048-OAEP) — zero Windows CNG dependency.

#include "codol_render/iwd.hpp"
#include "crypto_win.hpp"

#include <cstdio>

namespace codol::render::detail
{
	class aes_ecb
	{
	public:
		aes_ecb() = default;
		~aes_ecb() = default;
		aes_ecb(const aes_ecb&) = delete;
		aes_ecb& operator=(const aes_ecb&) = delete;

		bool init(const std::uint8_t* key, std::size_t key_size, std::string* error);
		bool ctr_transform(const std::uint8_t* iv, std::uint8_t* data, std::size_t size, std::string* error) const;

	private:
		codol::zone::crypto::AesCtr aes_;
	};

	std::FILE* fopen_rb(const std::filesystem::path& path);
	bool fseek64(std::FILE* f, std::int64_t offset, int origin);
	std::int64_t ftell64(std::FILE* f);

	bool read_file_range(const std::filesystem::path& path, std::uint64_t offset, std::size_t size, std::vector<std::uint8_t>& out);
	bool read_whole_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out);
}
