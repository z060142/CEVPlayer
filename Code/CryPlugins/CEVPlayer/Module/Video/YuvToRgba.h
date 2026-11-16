// Copyright (C) 2017-2020 Ilya Chernetsov. All rights reserved. Contacts: <chernecoff@gmail.com>
// License: https://github.com/afrostalin/CEVPlayer/blob/master/LICENSE

#pragma once

#include <cstdint>

namespace CEVPlayer
{
	// Converts a planar 8-bit I420 (YUV 4:2:0) image to packed RGBA (byte order R, G, B, A; alpha = 255).
	//
	// - Color matrix: BT.601, limited ("studio") range (Y 16..235, U/V 16..240), fixed point.
	// - Odd widths and heights are supported: the chroma planes are expected to be
	//   ((width + 1) / 2) x ((height + 1) / 2) samples.
	// - Each plane has its own stride in bytes (strides may be larger than the visible width, or
	//   negative for bottom-up images). The destination stride is in bytes as well.
	// - Chroma is replicated (nearest neighbour), there is no chroma interpolation.
	void ConvertI420ToRgba(
		const uint8_t* pY, int yStride,
		const uint8_t* pU, int uStride,
		const uint8_t* pV, int vStride,
		int width, int height,
		uint8_t* pRgba, int rgbaStride);
}
