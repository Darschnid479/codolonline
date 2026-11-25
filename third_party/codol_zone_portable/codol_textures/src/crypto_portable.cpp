#include "crypto_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace codol::render
{
	namespace
	{
		struct crc_table
		{
			std::uint32_t entries[256];

			constexpr crc_table() : entries()
			{
				for (std::uint32_t i = 0; i < 256; i++)
				{
					std::uint32_t c = i;
					for (int k = 0; k < 8; k++)
					{
						c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
					}
					entries[i] = c;
				}
			}
		};
		constexpr crc_table CRC_TABLE;
	}

	std::uint32_t crc32(const byte_span data, const std::uint32_t initial)
	{
		std::uint32_t crc = ~initial;
		for (const std::uint8_t b : data)
		{
			crc = CRC_TABLE.entries[(crc ^ b) & 0xFF] ^ (crc >> 8);
		}
		return ~crc;
	}

	std::string hex32(const std::uint32_t value)
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), "0x%08X", value);
		return buf;
	}

	zone_view flat_zone_view(const byte_span data, const std::uint32_t base)
	{
		zone_view view;
		view.read = [data, base](const std::uint32_t ptr, const std::uint32_t size) -> byte_span
		{
			if (ptr == 0 || ptr < base) return {};
			const std::uint64_t off = static_cast<std::uint64_t>(ptr - base);
			if (off + size > data.size()) return {};
			return data.subspan(static_cast<std::size_t>(off), size);
		};
		view.string = [data, base](const std::uint32_t ptr, std::string& out) -> bool
		{
			if (ptr == 0 || ptr < base) return false;
			const std::size_t off = static_cast<std::size_t>(ptr - base);
			if (off >= data.size()) return false;
			std::size_t end = off;
			while (end < data.size() && data[end] != 0) ++end;
			if (end >= data.size()) return false;
			out.assign(reinterpret_cast<const char*>(data.data() + off), end - off);
			return true;
		};
		return view;
	}

	namespace detail
	{
		std::FILE* fopen_rb(const std::filesystem::path& path)
		{
#if defined(_WIN32)
			return _wfopen(path.c_str(), L"rb");
#else
			return std::fopen(path.c_str(), "rb");
#endif
		}

		bool fseek64(std::FILE* f, const std::int64_t offset, const int origin)
		{
#if defined(_WIN32)
			return _fseeki64(f, static_cast<long long>(offset), origin) == 0;
#else
			return fseeko(f, static_cast<off_t>(offset), origin) == 0;
#endif
		}

		std::int64_t ftell64(std::FILE* f)
		{
#if defined(_WIN32)
			return static_cast<std::int64_t>(_ftelli64(f));
#else
			return static_cast<std::int64_t>(ftello(f));
#endif
		}

		bool aes_ecb::init(const std::uint8_t* key, const std::size_t key_size, std::string* error)
		{
			if (!aes_.init({ key, key_size }))
			{
				set_error(error, "AES key initialisation failed");
				return false;
			}
			return true;
		}

		bool aes_ecb::ctr_transform(const std::uint8_t* iv, std::uint8_t* data, const std::size_t size, std::string* error) const
		{
			if (size == 0) return true;
			if (!aes_.transform_be(iv, { data, size }))
			{
				set_error(error, "AES-CTR transform failed");
				return false;
			}
			return true;
		}

		bool read_file_range(const std::filesystem::path& path, const std::uint64_t offset, const std::size_t size, std::vector<std::uint8_t>& out)
		{
			std::FILE* file = fopen_rb(path);
			if (!file) return false;
			out.resize(size);
			const bool ok = fseek64(file, static_cast<std::int64_t>(offset), SEEK_SET)
				&& std::fread(out.data(), 1, size, file) == size;
			std::fclose(file);
			return ok;
		}

		bool read_whole_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out)
		{
			std::FILE* file = fopen_rb(path);
			if (!file) return false;
			bool ok = fseek64(file, 0, SEEK_END);
			const std::int64_t size = ok ? ftell64(file) : -1;
			ok = ok && size >= 0 && fseek64(file, 0, SEEK_SET);
			if (ok)
			{
				out.resize(static_cast<std::size_t>(size));
				ok = std::fread(out.data(), 1, out.size(), file) == out.size();
			}
			std::fclose(file);
			return ok;
		}
	}

	bool aes_ctr_transform(const iwd_key& key, const std::uint8_t* iv, std::uint8_t* data, const std::size_t size, std::string* error)
	{
		try
		{
			detail::aes_ecb aes;
			return aes.init(key.data(), key.size(), error) && aes.ctr_transform(iv, data, size, error);
		}
		catch (...)
		{
			set_error(error, "out of memory");
			return false;
		}
	}

	bool derive_iwd_key(const std::filesystem::path& exe, iwd_key& key, std::string* error)
	{
		try
		{
			std::vector<std::uint8_t> image;
			if (!detail::read_whole_file(exe, image))
			{
				set_error(error, "cannot read " + exe.string());
				return false;
			}
			const std::size_t blob_offset = static_cast<std::size_t>(build::EXE_IWD_KEY_BLOB);
			if (image.size() < blob_offset + 256)
			{
				set_error(error, exe.string() + " is too small to contain the IWD key blob");
				return false;
			}

			// Find PKCS#1 RSA-2048 private key in the executable
			static constexpr std::uint8_t DER_PREFIX[] = { 0x30, 0x82, 0x04, 0xA3, 0x02, 0x01, 0x00, 0x02, 0x82, 0x01, 0x01, 0x00 };
			codol::zone::crypto::Rsa rsa;

			auto try_at = [&](const std::size_t offset) -> bool
			{
				if (offset + sizeof(DER_PREFIX) > image.size() || std::memcmp(image.data() + offset, DER_PREFIX, sizeof(DER_PREFIX)) != 0)
					return false;
				const std::size_t length = 4 + ((static_cast<std::size_t>(image[offset + 2]) << 8) | image[offset + 3]);
				if (offset + length > image.size())
					return false;
				codol::zone::crypto::RsaPrivateKey parsed;
				return codol::zone::crypto::parse_pkcs1_private_key({ image.data() + offset, length }, parsed)
					&& parsed.modulus.size() == 256
					&& rsa.import(parsed);
			};

			bool found = try_at(static_cast<std::size_t>(build::EXE_RSA_PRIVATE_KEY_DER));
			if (!found)
			{
				auto it = image.begin();
				while (it != image.end())
				{
					it = std::search(it, image.end(), std::begin(DER_PREFIX), std::end(DER_PREFIX));
					if (it == image.end()) break;
					if (try_at(static_cast<std::size_t>(it - image.begin())))
					{
						found = true;
						break;
					}
					++it;
				}
			}
			if (!found)
			{
				set_error(error, "no PKCS#1 RSA-2048 private key found in " + exe.string());
				return false;
			}

			std::vector<std::uint8_t> plain;
			if (!rsa.decrypt_oaep_sha256({ image.data() + blob_offset, 256 }, plain))
			{
				set_error(error, "the IWD key blob did not decrypt; the executable is not the 1.1.1.13 client");
				return false;
			}
			if (plain.size() != key.size())
			{
				set_error(error, "the IWD key blob decrypted to " + std::to_string(plain.size()) + " bytes instead of 24");
				return false;
			}
			std::memcpy(key.data(), plain.data(), key.size());
			return true;
		}
		catch (...)
		{
			set_error(error, "out of memory");
			return false;
		}
	}
}
