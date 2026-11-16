// Copyright (C) 2017-2020 Ilya Chernetsov. All rights reserved. Contacts: <chernecoff@gmail.com>
// License: https://github.com/afrostalin/CEVPlayer/blob/master/LICENSE

#include "StdAfx.h"
#include "YuvToRgba.h"

namespace CEVPlayer
{
	namespace
	{
		inline uint8_t ClampToByte(int value)
		{
			return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
		}

		// BT.601 limited range, 8.8 fixed point:
		//   R = 1.164 * (Y - 16)                    + 1.596 * (V - 128)
		//   G = 1.164 * (Y - 16) - 0.392 * (U - 128) - 0.813 * (V - 128)
		//   B = 1.164 * (Y - 16) + 2.017 * (U - 128)
		inline void StoreRgba(uint8_t* pDst, int y, int cu, int cv)
		{
			const int c = 298 * (y - 16) + 128;
			pDst[0] = ClampToByte((c + 409 * cv) >> 8);
			pDst[1] = ClampToByte((c - 100 * cu - 208 * cv) >> 8);
			pDst[2] = ClampToByte((c + 516 * cu) >> 8);
			pDst[3] = 255;
		}
	}

	void ConvertI420ToRgba(
		const uint8_t* pY, int yStride,
		const uint8_t* pU, int uStride,
		const uint8_t* pV, int vStride,
		int width, int height,
		uint8_t* pRgba, int rgbaStride)
	{
		if (pY == nullptr || pU == nullptr || pV == nullptr || pRgba == nullptr || width <= 0 || height <= 0)
			return;

		for (int row = 0; row < height; ++row)
		{
			const uint8_t* y = pY + static_cast<ptrdiff_t>(row) * yStride;
			const uint8_t* u = pU + static_cast<ptrdiff_t>(row >> 1) * uStride;
			const uint8_t* v = pV + static_cast<ptrdiff_t>(row >> 1) * vStride;
			uint8_t* dst = pRgba + static_cast<ptrdiff_t>(row) * rgbaStride;

			int x = 0;
			// Two pixels share one chroma sample.
			for (; x + 1 < width; x += 2)
			{
				const int cu = static_cast<int>(u[x >> 1]) - 128;
				const int cv = static_cast<int>(v[x >> 1]) - 128;
				StoreRgba(dst + x * 4, y[x], cu, cv);
				StoreRgba(dst + x * 4 + 4, y[x + 1], cu, cv);
			}

			// Odd width: the last pixel has its own (partially covered) chroma sample.
			if (x < width)
			{
				const int cu = static_cast<int>(u[x >> 1]) - 128;
				const int cv = static_cast<int>(v[x >> 1]) - 128;
				StoreRgba(dst + x * 4, y[x], cu, cv);
			}
		}
	}
}
