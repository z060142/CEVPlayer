#include "CEVCore/ColorConvert.h"

namespace cev
{
	namespace
	{
		// 16.16 fixed point:  R = yk*(Y-yOff) + rv*Cr,  G = yk*(Y-yOff) - gu*Cb - gv*Cr,  B = yk*(Y-yOff) + bu*Cb
		struct Matrix
		{
			int yOff;
			int yk;
			int rv, gu, gv, bu;
		};

		constexpr Matrix kMatrix601Limited = {16, 76284, 104595, 25675, 53279, 132201};
		constexpr Matrix kMatrix709Limited = {16, 76284, 117489, 13975, 34925, 138438};
		constexpr Matrix kMatrix601Full = {0, 65536, 91881, 22554, 46802, 116130};
		constexpr Matrix kMatrix709Full = {0, 65536, 103206, 12276, 30679, 121609};

		inline uint8_t Clamp8(int v)
		{
			return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
		}

		inline uint8_t Premultiply(int c, int a)
		{
			return static_cast<uint8_t>(((c * a + 128) * 257) >> 16);
		}
	}

	ColorStandard ResolveColorStandard(ColorStandard requested, int matrixCoefficients, int width, int height)
	{
		if (requested == ColorStandard::BT601 || requested == ColorStandard::BT709)
			return requested;
		switch (matrixCoefficients)
		{
		case 1:  // BT.709
		case 9:  // BT.2020 non constant luminance (approximated with BT.709)
		case 10: // BT.2020 constant luminance (approximated with BT.709)
			return ColorStandard::BT709;
		case 5:  // BT.470BG
		case 6:  // SMPTE 170M
		case 7:  // SMPTE 240M (close enough to 601)
			return ColorStandard::BT601;
		default:
			break;
		}
		return (height >= 720 || width >= 1280) ? ColorStandard::BT709 : ColorStandard::BT601;
	}

	ColorRange ResolveColorRange(int matroskaRange)
	{
		return matroskaRange == 2 ? ColorRange::Full : ColorRange::Limited;
	}

	ChromaSiting ResolveChromaSiting(int horz, int vert)
	{
		if (horz == 2 && vert == 2)
			return ChromaSiting::Center;
		if (horz == 1 && vert == 1)
			return ChromaSiting::TopLeft;
		return ChromaSiting::Left; // (1,2), not tagged, unspecified or not expressible
	}

	void ConvertYuvaToRgbaPremultiplied(const YuvaImage& src, ColorStandard standard, ColorRange range,
		uint8_t* dst, int dstStride)
	{
		if (dst == nullptr || src.plane[0] == nullptr || src.plane[1] == nullptr || src.plane[2] == nullptr ||
			src.width <= 0 || src.height <= 0)
			return;

		const bool full = range == ColorRange::Full;
		const Matrix& m = standard == ColorStandard::BT709 ? (full ? kMatrix709Full : kMatrix709Limited)
		                                                   : (full ? kMatrix601Full : kMatrix601Limited);
		const int sx = src.chromaShiftX;
		const int sy = src.chromaShiftY;

		for (int row = 0; row < src.height; ++row)
		{
			const uint8_t* y = src.plane[0] + static_cast<ptrdiff_t>(row) * src.stride[0];
			const uint8_t* u = src.plane[1] + static_cast<ptrdiff_t>(row >> sy) * src.stride[1];
			const uint8_t* v = src.plane[2] + static_cast<ptrdiff_t>(row >> sy) * src.stride[2];
			const uint8_t* a = src.plane[3] != nullptr ? src.plane[3] + static_cast<ptrdiff_t>(row) * src.stride[3] : nullptr;
			uint8_t* out = dst + static_cast<ptrdiff_t>(row) * dstStride;

			for (int x = 0; x < src.width; ++x, out += 4)
			{
				const int cu = static_cast<int>(u[x >> sx]) - 128;
				const int cv = static_cast<int>(v[x >> sx]) - 128;
				const int luma = m.yk * (static_cast<int>(y[x]) - m.yOff) + 32768;
				const int r = Clamp8((luma + m.rv * cv) >> 16);
				const int g = Clamp8((luma - m.gu * cu - m.gv * cv) >> 16);
				const int b = Clamp8((luma + m.bu * cu) >> 16);
				const int alpha = a != nullptr ? a[x] : 255;
				if (alpha == 255)
				{
					out[0] = static_cast<uint8_t>(r);
					out[1] = static_cast<uint8_t>(g);
					out[2] = static_cast<uint8_t>(b);
					out[3] = 255;
				}
				else
				{
					out[0] = Premultiply(r, alpha);
					out[1] = Premultiply(g, alpha);
					out[2] = Premultiply(b, alpha);
					out[3] = static_cast<uint8_t>(alpha);
				}
			}
		}
	}
}
