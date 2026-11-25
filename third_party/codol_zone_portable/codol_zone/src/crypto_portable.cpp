#include "crypto_win.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <random>

#if defined(__PS4__) || defined(__ORBIS__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace codol::zone::crypto
{
	// =========================================================================
	// SHA-256 (FIPS 180-4)
	// =========================================================================
	namespace
	{
		constexpr std::uint32_t SHA256_K[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
		};

		inline std::uint32_t rotr32(std::uint32_t x, unsigned n)
		{
			return (x >> n) | (x << (32u - n));
		}

		inline std::uint32_t load_be32(const std::uint8_t* p)
		{
			return (static_cast<std::uint32_t>(p[0]) << 24) |
			       (static_cast<std::uint32_t>(p[1]) << 16) |
			       (static_cast<std::uint32_t>(p[2]) << 8)  |
			       static_cast<std::uint32_t>(p[3]);
		}

		inline void store_be32(std::uint8_t* p, std::uint32_t v)
		{
			p[0] = static_cast<std::uint8_t>(v >> 24);
			p[1] = static_cast<std::uint8_t>(v >> 16);
			p[2] = static_cast<std::uint8_t>(v >> 8);
			p[3] = static_cast<std::uint8_t>(v);
		}

		struct Sha256Ctx
		{
			std::uint32_t h[8] = {
				0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
				0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
			};
			std::uint64_t totalBits = 0;
			std::uint8_t buf[64]{};
			std::size_t bufLen = 0;

			void compress(const std::uint8_t block[64])
			{
				std::uint32_t w[64];
				for (int i = 0; i < 16; ++i)
					w[i] = load_be32(block + i * 4);
				for (int i = 16; i < 64; ++i)
				{
					const std::uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
					const std::uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
					w[i] = w[i - 16] + s0 + w[i - 7] + s1;
				}
				std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
				std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
				for (int i = 0; i < 64; ++i)
				{
					const std::uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
					const std::uint32_t ch = (e & f) ^ ((~e) & g);
					const std::uint32_t t1 = hh + S1 + ch + SHA256_K[i] + w[i];
					const std::uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
					const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
					const std::uint32_t t2 = S0 + maj;
					hh = g; g = f; f = e; e = d + t1;
					d = c; c = b; b = a; a = t1 + t2;
				}
				h[0] += a; h[1] += b; h[2] += c; h[3] += d;
				h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
			}

			void update(const std::uint8_t* data, std::size_t len)
			{
				totalBits += static_cast<std::uint64_t>(len) * 8u;
				while (len > 0)
				{
					const std::size_t take = std::min(len, 64u - bufLen);
					std::memcpy(buf + bufLen, data, take);
					bufLen += take;
					data += take;
					len -= take;
					if (bufLen == 64)
					{
						compress(buf);
						bufLen = 0;
					}
				}
			}

			void finish(std::uint8_t out[32])
			{
				buf[bufLen++] = 0x80;
				if (bufLen > 56)
				{
					std::memset(buf + bufLen, 0, 64 - bufLen);
					compress(buf);
					bufLen = 0;
				}
				std::memset(buf + bufLen, 0, 56 - bufLen);
				for (int i = 0; i < 8; ++i)
					buf[56 + i] = static_cast<std::uint8_t>(totalBits >> (56 - i * 8));
				compress(buf);
				for (int i = 0; i < 8; ++i)
					store_be32(out + i * 4, h[i]);
			}
		};

		// =====================================================================
		// AES (FIPS 197) S-box and GF(2^8) multiplication
		// =====================================================================
		constexpr std::uint8_t AES_SBOX[256] = {
			0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
			0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
			0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
			0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
			0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
			0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
			0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
			0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
			0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
			0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
			0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
			0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
			0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
			0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
			0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
			0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
		};

		constexpr std::uint8_t AES_RCON[11] = {
			0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
		};

		inline std::uint32_t sub_word(std::uint32_t w)
		{
			return (static_cast<std::uint32_t>(AES_SBOX[(w >> 24) & 0xFF]) << 24) |
			       (static_cast<std::uint32_t>(AES_SBOX[(w >> 16) & 0xFF]) << 16) |
			       (static_cast<std::uint32_t>(AES_SBOX[(w >> 8) & 0xFF]) << 8)   |
			       static_cast<std::uint32_t>(AES_SBOX[w & 0xFF]);
		}

		inline std::uint8_t xtime(std::uint8_t x)
		{
			return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
		}

		// =====================================================================
		// Arbitrary-precision unsigned BigInt (base 2^32, little-endian limbs)
		// =====================================================================
		struct BigUint
		{
			std::vector<std::uint32_t> limbs; // least-significant limb first

			void trim()
			{
				while (!limbs.empty() && limbs.back() == 0)
					limbs.pop_back();
			}

			bool is_zero() const { return limbs.empty(); }

			std::size_t bit_length() const
			{
				if (limbs.empty()) return 0;
				std::uint32_t top = limbs.back();
				std::size_t bits = (limbs.size() - 1) * 32;
				while (top != 0) { top >>= 1; ++bits; }
				return bits;
			}

			bool get_bit(std::size_t idx) const
			{
				const std::size_t limb = idx / 32;
				if (limb >= limbs.size()) return false;
				return ((limbs[limb] >> (idx & 31)) & 1u) != 0;
			}

			static BigUint from_be_bytes(std::span<const std::uint8_t> bytes)
			{
				BigUint r;
				const std::size_t n = bytes.size();
				r.limbs.assign((n + 3) / 4, 0);
				for (std::size_t i = 0; i < n; ++i)
				{
					const std::size_t rev = n - 1 - i;
					r.limbs[rev / 4] |= static_cast<std::uint32_t>(bytes[i]) << ((rev & 3) * 8);
				}
				r.trim();
				return r;
			}

			std::vector<std::uint8_t> to_be_bytes(std::size_t fixedLen) const
			{
				std::vector<std::uint8_t> out(fixedLen, 0);
				for (std::size_t i = 0; i < fixedLen; ++i)
				{
					const std::size_t limb = i / 4;
					if (limb < limbs.size())
						out[fixedLen - 1 - i] = static_cast<std::uint8_t>((limbs[limb] >> ((i & 3) * 8)) & 0xFF);
				}
				return out;
			}
		};

		int cmp(const BigUint& a, const BigUint& b)
		{
			if (a.limbs.size() != b.limbs.size())
				return a.limbs.size() < b.limbs.size() ? -1 : 1;
			for (std::size_t i = a.limbs.size(); i-- > 0;)
			{
				if (a.limbs[i] != b.limbs[i])
					return a.limbs[i] < b.limbs[i] ? -1 : 1;
			}
			return 0;
		}

		BigUint add(const BigUint& a, const BigUint& b)
		{
			BigUint r;
			const std::size_t n = std::max(a.limbs.size(), b.limbs.size());
			r.limbs.resize(n + 1, 0);
			std::uint64_t carry = 0;
			for (std::size_t i = 0; i < n; ++i)
			{
				const std::uint64_t av = i < a.limbs.size() ? a.limbs[i] : 0;
				const std::uint64_t bv = i < b.limbs.size() ? b.limbs[i] : 0;
				const std::uint64_t sum = av + bv + carry;
				r.limbs[i] = static_cast<std::uint32_t>(sum);
				carry = sum >> 32;
			}
			r.limbs[n] = static_cast<std::uint32_t>(carry);
			r.trim();
			return r;
		}

		// assumes a >= b
		BigUint sub(const BigUint& a, const BigUint& b)
		{
			BigUint r;
			r.limbs.resize(a.limbs.size(), 0);
			std::int64_t borrow = 0;
			for (std::size_t i = 0; i < a.limbs.size(); ++i)
			{
				const std::int64_t av = static_cast<std::int64_t>(a.limbs[i]);
				const std::int64_t bv = i < b.limbs.size() ? static_cast<std::int64_t>(b.limbs[i]) : 0;
				std::int64_t diff = av - bv - borrow;
				if (diff < 0)
				{
					diff += (1LL << 32);
					borrow = 1;
				}
				else
				{
					borrow = 0;
				}
				r.limbs[i] = static_cast<std::uint32_t>(diff);
			}
			r.trim();
			return r;
		}

		BigUint mul(const BigUint& a, const BigUint& b)
		{
			if (a.is_zero() || b.is_zero()) return {};
			BigUint r;
			r.limbs.assign(a.limbs.size() + b.limbs.size(), 0);
			for (std::size_t i = 0; i < a.limbs.size(); ++i)
			{
				std::uint64_t carry = 0;
				const std::uint64_t ai = a.limbs[i];
				for (std::size_t j = 0; j < b.limbs.size(); ++j)
				{
					const std::uint64_t cur = static_cast<std::uint64_t>(r.limbs[i + j]) + ai * b.limbs[j] + carry;
					r.limbs[i + j] = static_cast<std::uint32_t>(cur);
					carry = cur >> 32;
				}
				r.limbs[i + b.limbs.size()] += static_cast<std::uint32_t>(carry);
			}
			r.trim();
			return r;
		}

		BigUint shl_bits(const BigUint& a, std::size_t bits)
		{
			if (a.is_zero() || bits == 0) return a;
			const std::size_t wordShift = bits / 32;
			const unsigned bitShift = static_cast<unsigned>(bits & 31);
			BigUint r;
			r.limbs.assign(a.limbs.size() + wordShift + 1, 0);
			if (bitShift == 0)
			{
				for (std::size_t i = 0; i < a.limbs.size(); ++i)
					r.limbs[i + wordShift] = a.limbs[i];
			}
			else
			{
				std::uint32_t carry = 0;
				for (std::size_t i = 0; i < a.limbs.size(); ++i)
				{
					r.limbs[i + wordShift] = (a.limbs[i] << bitShift) | carry;
					carry = a.limbs[i] >> (32u - bitShift);
				}
				r.limbs[a.limbs.size() + wordShift] = carry;
			}
			r.trim();
			return r;
		}

		BigUint shr_bits(const BigUint& a, std::size_t bits)
		{
			const std::size_t wordShift = bits / 32;
			if (wordShift >= a.limbs.size()) return {};
			const unsigned bitShift = static_cast<unsigned>(bits & 31);
			BigUint r;
			const std::size_t outLen = a.limbs.size() - wordShift;
			r.limbs.assign(outLen, 0);
			if (bitShift == 0)
			{
				for (std::size_t i = 0; i < outLen; ++i)
					r.limbs[i] = a.limbs[i + wordShift];
			}
			else
			{
				for (std::size_t i = 0; i < outLen; ++i)
				{
					std::uint32_t lo = a.limbs[i + wordShift] >> bitShift;
					std::uint32_t hi = (i + wordShift + 1 < a.limbs.size()) ? (a.limbs[i + wordShift + 1] << (32u - bitShift)) : 0;
					r.limbs[i] = lo | hi;
				}
			}
			r.trim();
			return r;
		}

		// Knuth Algorithm D remainder (a % m)
		BigUint mod(const BigUint& a, const BigUint& m)
		{
			if (m.is_zero()) return {};
			if (cmp(a, m) < 0) return a;
			if (m.limbs.size() == 1)
			{
				const std::uint64_t d = m.limbs[0];
				std::uint64_t rem = 0;
				for (std::size_t i = a.limbs.size(); i-- > 0;)
					rem = ((rem << 32) | a.limbs[i]) % d;
				BigUint r;
				if (rem != 0) r.limbs.push_back(static_cast<std::uint32_t>(rem));
				return r;
			}

			// Normalize so MSB of divisor top limb is set
			const std::size_t n = m.limbs.size();
			unsigned shift = 0;
			{
				std::uint32_t top = m.limbs.back();
				while ((top & 0x80000000u) == 0) { top <<= 1; ++shift; }
			}
			BigUint u = shl_bits(a, shift);
			BigUint v = shl_bits(m, shift);
			const std::size_t mLimbs = u.limbs.size() - n;
			u.limbs.resize(mLimbs + n + 1, 0);

			const std::uint64_t v1 = v.limbs[n - 1];
			const std::uint64_t v2 = v.limbs[n - 2];

			for (std::size_t j = mLimbs + 1; j-- > 0;)
			{
				const std::uint64_t u2 = u.limbs[j + n];
				const std::uint64_t u1 = u.limbs[j + n - 1];
				const std::uint64_t u0 = u.limbs[j + n - 2];
				std::uint64_t num = (u2 << 32) | u1;
				std::uint64_t qhat = (u2 == v1) ? 0xFFFFFFFFULL : (num / v1);
				std::uint64_t rhat = num - qhat * v1;
				while (rhat < (1ULL << 32) && qhat * v2 > ((rhat << 32) | u0))
				{
					--qhat;
					rhat += v1;
				}
				// u[j..j+n] -= qhat * v[0..n-1]
				std::int64_t borrow = 0;
				for (std::size_t i = 0; i < n; ++i)
				{
					const std::uint64_t prod = qhat * v.limbs[i];
					const std::int64_t t = static_cast<std::int64_t>(u.limbs[j + i]) - static_cast<std::int64_t>(prod & 0xFFFFFFFFULL) - borrow;
					u.limbs[j + i] = static_cast<std::uint32_t>(t);
					borrow = static_cast<std::int64_t>(prod >> 32) - (t >> 32);
				}
				const std::int64_t t = static_cast<std::int64_t>(u.limbs[j + n]) - borrow;
				u.limbs[j + n] = static_cast<std::uint32_t>(t);
				if (t < 0)
				{
					// Add back v
					std::uint64_t carry = 0;
					for (std::size_t i = 0; i < n; ++i)
					{
						const std::uint64_t sum = static_cast<std::uint64_t>(u.limbs[j + i]) + v.limbs[i] + carry;
						u.limbs[j + i] = static_cast<std::uint32_t>(sum);
						carry = sum >> 32;
					}
					u.limbs[j + n] += static_cast<std::uint32_t>(carry);
				}
			}
			u.limbs.resize(n);
			u.trim();
			return shr_bits(u, shift);
		}

		// Montgomery multiplication for fast RSA modexp (odd modulus m)
		struct MontCtx
		{
			BigUint m;
			std::size_t k = 0;
			std::uint32_t n0inv = 0; // -m[0]^{-1} mod 2^32
			BigUint r2;              // R^2 mod m where R = 2^(32*k)

			bool init(const BigUint& modIn)
			{
				if (modIn.is_zero() || (modIn.limbs[0] & 1u) == 0) return false;
				m = modIn;
				k = m.limbs.size();
				// Newton-Raphson inverse mod 2^32
				std::uint32_t inv = 1;
				for (int i = 0; i < 5; ++i)
					inv *= 2u - m.limbs[0] * inv;
				n0inv = 0u - inv;

				// Compute R^2 mod m = 2^(64*k) mod m
				BigUint r;
				r.limbs.assign(2 * k + 1, 0);
				r.limbs[2 * k] = 1;
				r2 = mod(r, m);
				return true;
			}

			BigUint mont_mul(const BigUint& a, const BigUint& b) const
			{
				// CIOS Montgomery multiplication
				std::vector<std::uint32_t> t(k + 2, 0);
				for (std::size_t i = 0; i < k; ++i)
				{
					const std::uint64_t ai = i < a.limbs.size() ? a.limbs[i] : 0;
					std::uint64_t carry = 0;
					for (std::size_t j = 0; j < k; ++j)
					{
						const std::uint64_t bj = j < b.limbs.size() ? b.limbs[j] : 0;
						const std::uint64_t cur = static_cast<std::uint64_t>(t[j]) + ai * bj + carry;
						t[j] = static_cast<std::uint32_t>(cur);
						carry = cur >> 32;
					}
					const std::uint64_t sum = static_cast<std::uint64_t>(t[k]) + carry;
					t[k] = static_cast<std::uint32_t>(sum);
					t[k + 1] = static_cast<std::uint32_t>(sum >> 32);

					const std::uint64_t u = static_cast<std::uint32_t>(t[0] * n0inv);
					carry = (static_cast<std::uint64_t>(t[0]) + u * m.limbs[0]) >> 32;
					for (std::size_t j = 1; j < k; ++j)
					{
						const std::uint64_t cur = static_cast<std::uint64_t>(t[j]) + u * m.limbs[j] + carry;
						t[j - 1] = static_cast<std::uint32_t>(cur);
						carry = cur >> 32;
					}
					const std::uint64_t s1 = static_cast<std::uint64_t>(t[k]) + carry;
					t[k - 1] = static_cast<std::uint32_t>(s1);
					t[k] = t[k + 1] + static_cast<std::uint32_t>(s1 >> 32);
					t[k + 1] = 0;
				}
				BigUint res;
				res.limbs.assign(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(k + 1));
				res.trim();
				if (cmp(res, m) >= 0)
					res = sub(res, m);
				return res;
			}

			BigUint pow_mod(const BigUint& baseIn, const BigUint& exp) const
			{
				BigUint one;
				one.limbs = { 1u };
				BigUint base = mod(baseIn, m);
				BigUint acc = mont_mul(one, r2);   // 1 in Montgomery form
				BigUint cur = mont_mul(base, r2);  // base in Montgomery form
				const std::size_t bits = exp.bit_length();
				for (std::size_t i = 0; i < bits; ++i)
				{
					if (exp.get_bit(i))
						acc = mont_mul(acc, cur);
					cur = mont_mul(cur, cur);
				}
				return mont_mul(acc, one); // out of Montgomery form
			}
		};

		BigUint pow_mod(const BigUint& base, const BigUint& exp, const BigUint& m)
		{
			MontCtx ctx;
			if (ctx.init(m))
				return ctx.pow_mod(base, exp);
			BigUint res;
			res.limbs = { 1u };
			BigUint b = mod(base, m);
			const std::size_t bits = exp.bit_length();
			for (std::size_t i = 0; i < bits; ++i)
			{
				if (exp.get_bit(i))
					res = mod(mul(res, b), m);
				b = mod(mul(b, b), m);
			}
			return res;
		}

		void mgf1_sha256(std::span<const std::uint8_t> seed, std::span<std::uint8_t> out)
		{
			std::vector<std::uint8_t> input(seed.size() + 4);
			std::memcpy(input.data(), seed.data(), seed.size());
			std::size_t produced = 0;
			std::uint32_t counter = 0;
			std::uint8_t digest[32];
			while (produced < out.size())
			{
				store_be32(input.data() + seed.size(), counter++);
				sha256(input, digest);
				const std::size_t take = std::min<std::size_t>(32, out.size() - produced);
				std::memcpy(out.data() + produced, digest, take);
				produced += take;
			}
		}

		struct Der
		{
			const std::uint8_t* p;
			const std::uint8_t* end;

			bool header(const std::uint8_t tag, std::size_t& length)
			{
				if (end - p < 2 || *p != tag) return false;
				++p;
				std::size_t len = *p++;
				if (len & 0x80)
				{
					const std::size_t n = len & 0x7F;
					if (n == 0 || n > 4 || static_cast<std::size_t>(end - p) < n) return false;
					len = 0;
					for (std::size_t i = 0; i < n; ++i)
						len = (len << 8) | *p++;
				}
				if (static_cast<std::size_t>(end - p) < len) return false;
				length = len;
				return true;
			}

			bool integer(std::vector<std::uint8_t>& value)
			{
				std::size_t len = 0;
				if (!header(0x02, len) || len == 0) return false;
				const std::uint8_t* s = p;
				p += len;
				while (len > 1 && *s == 0) { ++s; --len; }
				value.assign(s, s + len);
				return true;
			}
		};
	}

	// =========================================================================
	// Public API implementations
	// =========================================================================

	bool sha256(const std::span<const std::uint8_t> data, std::uint8_t digest[32])
	{
		Sha256Ctx ctx;
		if (!data.empty())
			ctx.update(data.data(), data.size());
		ctx.finish(digest);
		return true;
	}

	bool random_bytes(const std::span<std::uint8_t> out)
	{
		if (out.empty()) return true;
#if defined(_WIN32)
		return BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
		int fd = ::open("/dev/urandom", O_RDONLY);
		if (fd >= 0)
		{
			std::size_t total = 0;
			while (total < out.size())
			{
				const ssize_t n = ::read(fd, out.data() + total, out.size() - total);
				if (n <= 0) break;
				total += static_cast<std::size_t>(n);
			}
			::close(fd);
			if (total == out.size()) return true;
		}
		std::random_device rd;
		for (std::size_t i = 0; i < out.size(); ++i)
			out[i] = static_cast<std::uint8_t>(rd() & 0xFFu);
		return true;
#endif
	}

	bool AesBlock::init(const std::span<const std::uint8_t> key)
	{
		if (key.size() != 16 && key.size() != 24 && key.size() != 32)
			return false;
		const int nk = static_cast<int>(key.size() / 4);
		rounds_ = nk + 6;
		const int totalWords = 4 * (rounds_ + 1);
		for (int i = 0; i < nk; ++i)
			rk_[i] = load_be32(key.data() + i * 4);
		for (int i = nk; i < totalWords; ++i)
		{
			std::uint32_t temp = rk_[i - 1];
			if (i % nk == 0)
			{
				temp = ((temp << 8) | (temp >> 24));
				temp = sub_word(temp) ^ (static_cast<std::uint32_t>(AES_RCON[i / nk]) << 24);
			}
			else if (nk > 6 && (i % nk) == 4)
			{
				temp = sub_word(temp);
			}
			rk_[i] = rk_[i - nk] ^ temp;
		}
		return true;
	}

	void AesBlock::encrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const
	{
		std::uint8_t s[16];
		std::memcpy(s, in, 16);

		auto add_round_key = [&](int round)
		{
			for (int c = 0; c < 4; ++c)
			{
				const std::uint32_t w = rk_[round * 4 + c];
				s[c * 4 + 0] ^= static_cast<std::uint8_t>(w >> 24);
				s[c * 4 + 1] ^= static_cast<std::uint8_t>(w >> 16);
				s[c * 4 + 2] ^= static_cast<std::uint8_t>(w >> 8);
				s[c * 4 + 3] ^= static_cast<std::uint8_t>(w);
			}
		};

		add_round_key(0);
		for (int round = 1; round <= rounds_; ++round)
		{
			// SubBytes
			for (int i = 0; i < 16; ++i)
				s[i] = AES_SBOX[s[i]];

			// ShiftRows (column-major layout: col c, row r -> index c*4 + r)
			std::uint8_t t;
			// row 1: shift left 1
			t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
			// row 2: shift left 2
			t = s[2]; s[2] = s[10]; s[10] = t;
			t = s[6]; s[6] = s[14]; s[14] = t;
			// row 3: shift left 3 (right 1)
			t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;

			if (round < rounds_)
			{
				// MixColumns
				for (int c = 0; c < 4; ++c)
				{
					const std::uint8_t a0 = s[c * 4 + 0];
					const std::uint8_t a1 = s[c * 4 + 1];
					const std::uint8_t a2 = s[c * 4 + 2];
					const std::uint8_t a3 = s[c * 4 + 3];
					const std::uint8_t xr = a0 ^ a1 ^ a2 ^ a3;
					s[c * 4 + 0] = a0 ^ xr ^ xtime(a0 ^ a1);
					s[c * 4 + 1] = a1 ^ xr ^ xtime(a1 ^ a2);
					s[c * 4 + 2] = a2 ^ xr ^ xtime(a2 ^ a3);
					s[c * 4 + 3] = a3 ^ xr ^ xtime(a3 ^ a0);
				}
			}
			add_round_key(round);
		}
		std::memcpy(out, s, 16);
	}

	bool AesCtr::init(const std::span<const std::uint8_t> key)
	{
		return aes_.init(key);
	}

	bool AesCtr::transform(const std::uint8_t iv[16], const std::span<std::uint8_t> data)
	{
		if (!aes_.valid()) return false;
		if (data.empty()) return true;
		std::uint8_t counter[16];
		std::uint8_t ks[16];
		std::memcpy(counter, iv, 16);
		for (std::size_t off = 0; off < data.size(); off += 16)
		{
			aes_.encrypt_block(counter, ks);
			const std::size_t n = std::min<std::size_t>(16, data.size() - off);
			for (std::size_t i = 0; i < n; ++i)
				data[off + i] ^= ks[i];
			// Little-endian counter increment (FastFiles)
			for (int i = 0; i < 16; ++i)
			{
				if (++counter[i] != 0) break;
			}
		}
		return true;
	}

	bool AesCtr::transform_be(const std::uint8_t iv[16], const std::span<std::uint8_t> data) const
	{
		if (!aes_.valid()) return false;
		if (data.empty()) return true;
		std::uint8_t counter[16];
		std::uint8_t ks[16];
		std::memcpy(counter, iv, 16);
		for (std::size_t off = 0; off < data.size(); off += 16)
		{
			aes_.encrypt_block(counter, ks);
			const std::size_t n = std::min<std::size_t>(16, data.size() - off);
			for (std::size_t i = 0; i < n; ++i)
				data[off + i] ^= ks[i];
			// Big-endian counter increment (IWD archives)
			for (int i = 15; i >= 0; --i)
			{
				if (++counter[i] != 0) break;
			}
		}
		return true;
	}

	bool parse_pkcs1_private_key(const std::span<const std::uint8_t> der, RsaPrivateKey& key)
	{
		key = {};
		Der d{ der.data(), der.data() + der.size() };
		std::size_t seq = 0;
		if (!d.header(0x30, seq)) return false;
		d.end = d.p + seq;
		key.derLength = static_cast<std::size_t>(d.end - der.data());

		std::vector<std::uint8_t> version;
		if (!d.integer(version) || version.size() != 1 || version[0] != 0) return false;
		if (!d.integer(key.modulus) || !d.integer(key.publicExponent) || !d.integer(key.privateExponent)
			|| !d.integer(key.prime1) || !d.integer(key.prime2))
			return false;
		// Optional CRT fields (always present in standard PKCS#1 RSAPrivateKey DER)
		if (d.p < d.end)
		{
			d.integer(key.exponent1);
			d.integer(key.exponent2);
			d.integer(key.coefficient);
		}
		return true;
	}

	bool Rsa::import(const RsaPrivateKey& key)
	{
		if (key.modulus.empty() || key.publicExponent.empty() || key.privateExponent.empty())
			return false;
		key_ = key;
		valid_ = true;
		return true;
	}

	bool Rsa::decrypt_oaep_sha256(const std::span<const std::uint8_t> block, std::vector<std::uint8_t>& plain) const
	{
		plain.clear();
		if (!valid_) return false;
		const std::size_t k = key_.modulus.size();
		constexpr std::size_t hLen = 32;
		if (block.size() != k || k < 2 * hLen + 2) return false;

		const BigUint c = BigUint::from_be_bytes(block);
		const BigUint n = BigUint::from_be_bytes(key_.modulus);
		if (cmp(c, n) >= 0) return false;

		BigUint m;
		if (!key_.prime1.empty() && !key_.prime2.empty() && !key_.exponent1.empty()
			&& !key_.exponent2.empty() && !key_.coefficient.empty())
		{
			const BigUint p = BigUint::from_be_bytes(key_.prime1);
			const BigUint q = BigUint::from_be_bytes(key_.prime2);
			const BigUint dp = BigUint::from_be_bytes(key_.exponent1);
			const BigUint dq = BigUint::from_be_bytes(key_.exponent2);
			const BigUint qinv = BigUint::from_be_bytes(key_.coefficient);

			const BigUint m1 = pow_mod(c, dp, p);
			const BigUint m2 = pow_mod(c, dq, q);
			const BigUint m2modP = mod(m2, p);
			const BigUint diff = (cmp(m1, m2modP) >= 0) ? sub(m1, m2modP) : sub(add(m1, p), m2modP);
			const BigUint h = mod(mul(qinv, diff), p);
			m = add(m2, mul(h, q));
		}
		else
		{
			const BigUint d = BigUint::from_be_bytes(key_.privateExponent);
			m = pow_mod(c, d, n);
		}

		std::vector<std::uint8_t> em = m.to_be_bytes(k);
		const std::size_t dbLen = k - hLen - 1;
		const std::uint8_t y = em[0];

		std::array<std::uint8_t, hLen> seedMask{};
		mgf1_sha256({ em.data() + 1 + hLen, dbLen }, seedMask);
		std::array<std::uint8_t, hLen> seed{};
		for (std::size_t i = 0; i < hLen; ++i)
			seed[i] = em[1 + i] ^ seedMask[i];

		std::vector<std::uint8_t> dbMask(dbLen);
		mgf1_sha256(seed, dbMask);
		std::vector<std::uint8_t> db(dbLen);
		for (std::size_t i = 0; i < dbLen; ++i)
			db[i] = em[1 + hLen + i] ^ dbMask[i];

		std::uint8_t lHash[32];
		sha256({}, lHash);
		if (y != 0 || std::memcmp(db.data(), lHash, hLen) != 0)
			return false;

		std::size_t idx = hLen;
		while (idx < dbLen && db[idx] == 0x00)
			++idx;
		if (idx >= dbLen || db[idx] != 0x01)
			return false;

		plain.assign(db.begin() + static_cast<std::ptrdiff_t>(idx + 1), db.end());
		return true;
	}

	bool Rsa::encrypt_oaep_sha256(const std::span<const std::uint8_t> plain, std::vector<std::uint8_t>& block) const
	{
		block.clear();
		if (!valid_) return false;
		const std::size_t k = key_.modulus.size();
		constexpr std::size_t hLen = 32;
		if (k < 2 * hLen + 2 || plain.size() > k - 2 * hLen - 2) return false;

		const std::size_t dbLen = k - hLen - 1;
		std::vector<std::uint8_t> db(dbLen, 0);
		sha256({}, db.data());
		const std::size_t psLen = dbLen - hLen - 1 - plain.size();
		db[hLen + psLen] = 0x01;
		if (!plain.empty())
			std::memcpy(db.data() + hLen + psLen + 1, plain.data(), plain.size());

		std::array<std::uint8_t, hLen> seed{};
		if (!random_bytes(seed)) return false;

		std::vector<std::uint8_t> dbMask(dbLen);
		mgf1_sha256(seed, dbMask);
		std::vector<std::uint8_t> maskedDB(dbLen);
		for (std::size_t i = 0; i < dbLen; ++i)
			maskedDB[i] = db[i] ^ dbMask[i];

		std::array<std::uint8_t, hLen> seedMask{};
		mgf1_sha256(maskedDB, seedMask);
		std::vector<std::uint8_t> em(k, 0);
		em[0] = 0x00;
		for (std::size_t i = 0; i < hLen; ++i)
			em[1 + i] = seed[i] ^ seedMask[i];
		std::memcpy(em.data() + 1 + hLen, maskedDB.data(), dbLen);

		const BigUint m = BigUint::from_be_bytes(em);
		const BigUint e = BigUint::from_be_bytes(key_.publicExponent);
		const BigUint n = BigUint::from_be_bytes(key_.modulus);
		const BigUint c = pow_mod(m, e, n);
		block = c.to_be_bytes(k);
		return true;
	}
}
