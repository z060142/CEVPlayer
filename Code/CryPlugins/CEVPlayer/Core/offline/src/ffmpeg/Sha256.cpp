// SHA-256 (FIPS 180-4), straightforward implementation.
#include <cstdio>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <vector>

#include "CEVOffline/FfmpegInstall.h"
#include "FfmpegInternal.h"

namespace cev
{
	namespace offline
	{
		namespace ffmpeg
		{
			namespace
			{
				const uint32_t K[64] = {
					0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
					0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
					0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
					0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
					0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
					0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
					0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
					0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

				inline uint32_t Rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
			}

			void Sha256::Reset()
			{
				static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
				std::memcpy(m_state, init, sizeof(m_state));
				m_total = 0;
				m_fill = 0;
			}

			void Sha256::Block(const uint8_t* p)
			{
				uint32_t w[64];
				for (int i = 0; i < 16; ++i)
					w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
				for (int i = 16; i < 64; ++i)
				{
					const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
					const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
					w[i] = w[i - 16] + s0 + w[i - 7] + s1;
				}
				uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
				uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
				for (int i = 0; i < 64; ++i)
				{
					const uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
					const uint32_t ch = (e & f) ^ (~e & g);
					const uint32_t t1 = h + S1 + ch + K[i] + w[i];
					const uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
					const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
					const uint32_t t2 = S0 + maj;
					h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
				}
				m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
				m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
			}

			void Sha256::Update(const void* data, size_t size)
			{
				const uint8_t* p = static_cast<const uint8_t*>(data);
				m_total += size;
				if (m_fill != 0)
				{
					const size_t n = size < 64 - m_fill ? size : 64 - m_fill;
					std::memcpy(m_buffer + m_fill, p, n);
					m_fill += n;
					p += n;
					size -= n;
					if (m_fill == 64)
					{
						Block(m_buffer);
						m_fill = 0;
					}
				}
				while (size >= 64)
				{
					Block(p);
					p += 64;
					size -= 64;
				}
				if (size != 0)
				{
					std::memcpy(m_buffer, p, size);
					m_fill = size;
				}
			}

			std::string Sha256::FinalHex()
			{
				const uint64_t bits = m_total * 8;
				const uint8_t one = 0x80;
				Update(&one, 1);
				const uint8_t zero = 0;
				while (m_fill != 56)
					Update(&zero, 1);
				uint8_t len[8];
				for (int i = 0; i < 8; ++i)
					len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
				Update(len, 8);
				static const char* const hex = "0123456789abcdef";
				std::string out;
				for (int i = 0; i < 8; ++i)
					for (int s = 28; s >= 0; s -= 4)
						out.push_back(hex[(m_state[i] >> s) & 0xF]);
				return out;
			}

			std::string Sha256Hex(const void* data, size_t size)
			{
				Sha256 h;
				h.Update(data, size);
				return h.FinalHex();
			}

			Result Sha256FileHex(const std::string& path, std::string& hexOut, const Control& ctl)
			{
				using detail::Res;
				hexOut.clear();
				std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
				if (!in)
					return Res(Status::IoError, "cannot open " + path);
				Sha256 h;
				std::vector<char> buf(1u << 20);
				for (;;)
				{
					if (ctl.cancel != nullptr && ctl.cancel->load(std::memory_order_relaxed))
						return Res(Status::Cancelled, "cancelled");
					in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
					const std::streamsize n = in.gcount();
					if (n > 0)
						h.Update(buf.data(), static_cast<size_t>(n));
					if (!in)
					{
						if (!in.eof())
							return Res(Status::IoError, "read error in " + path);
						break;
					}
				}
				hexOut = h.FinalHex();
				return detail::Okay();
			}
		}
	}
}
