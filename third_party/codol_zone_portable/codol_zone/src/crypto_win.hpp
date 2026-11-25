#pragma once

// Portable pure C++20 cryptographic primitives for Project Dragon / codol_zone:
// SHA-256 (FIPS 180-4), AES-128/192/256 CTR (little-endian counter for FastFiles,
// big-endian counter helper for IWD archives), PKCS#1 DER parsing, and RSA-OAEP(SHA-256).
// Zero external dependencies — compiles natively on PS4 OrbisOS, Linux, and Windows.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace codol::zone::crypto
{
	bool sha256(std::span<const std::uint8_t> data, std::uint8_t digest[32]);

	// System/PRNG random bytes
	bool random_bytes(std::span<std::uint8_t> out);

	// Pure C++20 AES block cipher core (ECB single block + CTR keystreams)
	class AesBlock
	{
	public:
		bool init(std::span<const std::uint8_t> key);
		void encrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const;
		bool valid() const { return rounds_ != 0; }

	private:
		std::uint32_t rk_[60]{};
		int rounds_ = 0;
	};

	// LibTomCrypt-style CTR with a little-endian counter (FastFiles)
	class AesCtr
	{
	public:
		AesCtr() = default;
		~AesCtr() = default;
		AesCtr(const AesCtr&) = delete;
		AesCtr& operator=(const AesCtr&) = delete;

		bool init(std::span<const std::uint8_t> key);
		bool transform(const std::uint8_t iv[16], std::span<std::uint8_t> data);

		// Big-endian counter variant used by CoDOL 1.1.1.13 IWD archives
		bool transform_be(const std::uint8_t iv[16], std::span<std::uint8_t> data) const;

	private:
		AesBlock aes_;
	};

	// PKCS#1 RSAPrivateKey (DER) -> big-endian integers
	struct RsaPrivateKey
	{
		std::vector<std::uint8_t> modulus;
		std::vector<std::uint8_t> publicExponent;
		std::vector<std::uint8_t> privateExponent;
		std::vector<std::uint8_t> prime1;
		std::vector<std::uint8_t> prime2;
		std::vector<std::uint8_t> exponent1;
		std::vector<std::uint8_t> exponent2;
		std::vector<std::uint8_t> coefficient;
		std::size_t derLength = 0;
	};

	bool parse_pkcs1_private_key(std::span<const std::uint8_t> der, RsaPrivateKey& key);

	class Rsa
	{
	public:
		Rsa() = default;
		~Rsa() = default;
		Rsa(const Rsa&) = delete;
		Rsa& operator=(const Rsa&) = delete;

		bool import(const RsaPrivateKey& key);
		bool valid() const { return valid_; }
		bool decrypt_oaep_sha256(std::span<const std::uint8_t> block, std::vector<std::uint8_t>& plain) const;
		bool encrypt_oaep_sha256(std::span<const std::uint8_t> plain, std::vector<std::uint8_t>& block) const;

	private:
		bool valid_ = false;
		RsaPrivateKey key_;
	};
}

namespace codol::zone
{
	struct Status;

	namespace detail
	{
		Status zlib_inflate_zone(std::span<const std::uint8_t> compressed, std::vector<std::uint8_t>& zone);
	}
}
