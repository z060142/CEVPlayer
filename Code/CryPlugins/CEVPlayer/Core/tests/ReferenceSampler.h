// CPU reference of the consumer side of the packed planar layouts (texture source contract, CTS_LAYOUT_PACKED_YUV420 /
// CTS_LAYOUT_PACKED_YUVA420). Test and documentation only - never on a runtime path. The mathematics is written out
// in docs/design/M6-ui-source.md section 12 ("Reference implementation of planar sampling").
//
// Input: one R8 picture as the source uploads it (packedWidth x packedHeight texels, tight rows) and the rectangles,
// matrix, range and chroma siting of the contract snapshot. Output: premultiplied RGBA8, width x height (the picture).
//   SampleNearest  - chroma texel (x >> 1, y >> 1), 16.16 fixed point: bit-exact with ConvertYuvaToRgbaPremultiplied
//                    (what the source produces for CTS_LAYOUT_RGBA8).
//   SampleBilinear - what a consumer shader does: chroma bilinear at the sited position, the sample coordinate
//                    clamped half a texel inside each plane rectangle, float colour math, clamp, then premultiply.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cevref
{
	struct Rect { int x, y, w, h; };

	enum class Siting { Left = 0, Center = 1, TopLeft = 2 }; // CTS_SITING_*

	struct Packed
	{
		const uint8_t* data = nullptr; // R8, tight rows
		int packedWidth = 0;
		int packedHeight = 0;
		int width = 0;                 // picture
		int height = 0;
		Rect y{0, 0, 0, 0}, u{0, 0, 0, 0}, v{0, 0, 0, 0}, a{0, 0, 0, 0}; // a.w == 0: opaque
		bool bt709 = false;            // colorMatrix 1
		bool full = false;             // colorRange 1
		Siting siting = Siting::Left;
	};

	inline uint8_t Texel(const Packed& p, const Rect& r, int x, int y)
	{
		return p.data[static_cast<ptrdiff_t>(r.y + y) * p.packedWidth + r.x + x];
	}

	// ---- nearest: the source's own converter, re-derived from the packed picture ----
	inline void SampleNearest(const Packed& p, uint8_t* dst, int dstStride)
	{
		struct M { int yOff, yk, rv, gu, gv, bu; };
		static const M k601L = {16, 76284, 104595, 25675, 53279, 132201};
		static const M k709L = {16, 76284, 117489, 13975, 34925, 138438};
		static const M k601F = {0, 65536, 91881, 22554, 46802, 116130};
		static const M k709F = {0, 65536, 103206, 12276, 30679, 121609};
		const M& m = p.bt709 ? (p.full ? k709F : k709L) : (p.full ? k601F : k601L);
		const auto clamp8 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
		const auto premul = [](int c, int a) { return static_cast<uint8_t>(((c * a + 128) * 257) >> 16); };
		for (int y = 0; y < p.height; ++y)
		{
			uint8_t* out = dst + static_cast<ptrdiff_t>(y) * dstStride;
			for (int x = 0; x < p.width; ++x, out += 4)
			{
				const int cu = static_cast<int>(Texel(p, p.u, x >> 1, y >> 1)) - 128;
				const int cv = static_cast<int>(Texel(p, p.v, x >> 1, y >> 1)) - 128;
				const int luma = m.yk * (static_cast<int>(Texel(p, p.y, x, y)) - m.yOff) + 32768;
				const int r = clamp8((luma + m.rv * cv) >> 16);
				const int g = clamp8((luma - m.gu * cu - m.gv * cv) >> 16);
				const int b = clamp8((luma + m.bu * cu) >> 16);
				const int alpha = p.a.w > 0 ? Texel(p, p.a, x, y) : 255;
				if (alpha == 255)
				{
					out[0] = static_cast<uint8_t>(r); out[1] = static_cast<uint8_t>(g); out[2] = static_cast<uint8_t>(b); out[3] = 255;
				}
				else
				{
					out[0] = premul(r, alpha); out[1] = premul(g, alpha); out[2] = premul(b, alpha); out[3] = static_cast<uint8_t>(alpha);
				}
			}
		}
	}

	// ---- bilinear: the consumer's shader in double precision ----
	// Bilinear fetch of plane `r` at continuous texel coordinates (cx, cy) where texel i has its centre at i. The
	// coordinate is clamped to [0, w-1] x [0, h-1], i.e. half a texel inside the rectangle in texture space, so no
	// neighbouring plane is ever touched.
	inline double Fetch(const Packed& p, const Rect& r, double cx, double cy)
	{
		cx = std::min(std::max(cx, 0.0), static_cast<double>(r.w - 1));
		cy = std::min(std::max(cy, 0.0), static_cast<double>(r.h - 1));
		const int x0 = static_cast<int>(std::floor(cx));
		const int y0 = static_cast<int>(std::floor(cy));
		const int x1 = std::min(x0 + 1, r.w - 1);
		const int y1 = std::min(y0 + 1, r.h - 1);
		const double fx = cx - x0, fy = cy - y0;
		const double top = Texel(p, r, x0, y0) * (1.0 - fx) + Texel(p, r, x1, y0) * fx;
		const double bot = Texel(p, r, x0, y1) * (1.0 - fx) + Texel(p, r, x1, y1) * fx;
		return top * (1.0 - fy) + bot * fy;
	}

	inline void SampleBilinear(const Packed& p, uint8_t* dst, int dstStride)
	{
		const double kr = p.bt709 ? 0.2126 : 0.299;
		const double kb = p.bt709 ? 0.0722 : 0.114;
		const double kg = 1.0 - kr - kb;
		// Chroma position of luma pixel (x, y) in chroma texel coordinates (centre of chroma texel i at i).
		const double offX = p.siting == Siting::Center ? -0.5 : 0.0;   // left / top-left: co-sited with even luma columns
		const double offY = p.siting == Siting::TopLeft ? 0.0 : -0.5;  // left / center: vertically between two luma rows
		for (int y = 0; y < p.height; ++y)
		{
			uint8_t* out = dst + static_cast<ptrdiff_t>(y) * dstStride;
			for (int x = 0; x < p.width; ++x, out += 4)
			{
				const double cx = (x + offX) * 0.5;
				const double cy = (y + offY) * 0.5;
				const double Y = Texel(p, p.y, x, y);
				const double U = Fetch(p, p.u, cx, cy);
				const double V = Fetch(p, p.v, cx, cy);
				const double yn = p.full ? Y / 255.0 : (Y - 16.0) / 219.0;
				const double cb = p.full ? (U - 128.0) / 255.0 : (U - 128.0) / 224.0;
				const double cr = p.full ? (V - 128.0) / 255.0 : (V - 128.0) / 224.0;
				double rgb[3] = {yn + 2.0 * (1.0 - kr) * cr,
				                 yn - 2.0 * kb * (1.0 - kb) / kg * cb - 2.0 * kr * (1.0 - kr) / kg * cr,
				                 yn + 2.0 * (1.0 - kb) * cb};
				const double alpha = p.a.w > 0 ? Texel(p, p.a, x, y) / 255.0 : 1.0; // flags bit3 = 0: straight colour
				for (int c = 0; c < 3; ++c)
				{
					const double clamped = std::min(std::max(rgb[c], 0.0), 1.0);
					out[c] = static_cast<uint8_t>(std::floor(clamped * alpha * 255.0 + 0.5));
				}
				out[3] = static_cast<uint8_t>(std::floor(alpha * 255.0 + 0.5));
			}
		}
	}
}
