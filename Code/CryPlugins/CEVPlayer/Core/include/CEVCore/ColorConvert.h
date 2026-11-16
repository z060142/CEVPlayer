// CPU YUV(A) -> RGBA conversion with premultiplied alpha (8 bit planar 4:2:0 / 4:2:2 / 4:4:0 / 4:4:4).
#pragma once

#include <cstdint>

namespace cev
{
	enum class ColorStandard : uint8_t
	{
		Auto,   // from the stream's matrix coefficients, else BT.601 below 720 lines and BT.709 from 720 lines up
		BT601,
		BT709,
	};

	enum class ColorRange : uint8_t
	{
		Limited, // 16..235 / 16..240 (default, also used when the stream does not say)
		Full,
	};

	struct YuvaImage
	{
		const uint8_t* plane[4] = {nullptr, nullptr, nullptr, nullptr}; // Y, U, V, A (A may be null = opaque)
		int stride[4] = {0, 0, 0, 0};
		int width = 0;
		int height = 0;
		int chromaShiftX = 1;
		int chromaShiftY = 1;
	};

	// matrixCoefficients: Matroska Colour/MatrixCoefficients (ISO 23001-8) or -1 when absent.
	ColorStandard ResolveColorStandard(ColorStandard requested, int matrixCoefficients, int width, int height);
	// matroskaRange: Matroska Colour/Range (1 = broadcast/limited, 2 = full) or -1 / 0 when absent.
	ColorRange ResolveColorRange(int matroskaRange);

	// dst must hold height rows of dstStride bytes (>= width*4). Output byte order R, G, B, A. Colors are
	// multiplied by alpha. standard must be BT601 or BT709 (resolve Auto first).
	void ConvertYuvaToRgbaPremultiplied(const YuvaImage& src, ColorStandard standard, ColorRange range,
		uint8_t* dst, int dstStride);
}
