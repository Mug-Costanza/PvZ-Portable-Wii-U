/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable.
 *
 * PvZ-Portable is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PvZ-Portable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with PvZ-Portable. If not, see <https://www.gnu.org/licenses/>.
 */

#include <opengx.h>
#include <ogc/system.h>

#include "OgxTexture.h"

// Internal OpenGX (0.17) helper, exported with C linkage but not declared in
// opengx.h. Copies the GXTexObj for a GL texture name without touching the
// bound-texture state, so it's safe to call off the render path.
extern "C" bool _ogx_texture_get_texobj(GLuint texture_name, GXTexObj *texobj);

namespace
{

inline uint32_t Expand565(uint16_t v)
{
	uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
	r = (r << 3) | (r >> 2);
	g = (g << 2) | (g >> 4);
	b = (b << 3) | (b >> 2);
	return 0xFF000000u | (r << 16) | (g << 8) | b;
}

inline uint16_t ReadBE16(const uint8_t* p)
{
	return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t Mix(uint32_t a, uint32_t b, int wa, int wb, int div)
{
	uint32_t out = 0xFF000000u;
	for (int shift = 0; shift <= 16; shift += 8)
	{
		uint32_t ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
		out |= ((ca * wa + cb * wb) / div) << shift;
	}
	return out;
}

// GX texel layouts (all tiled, tiles in row-major order):
//   RGBA8  - 4x4 tiles, 64 bytes: 16 AR pairs, then 16 GB pairs
//   RGB565 - 4x4 tiles, 32 bytes: 16 big-endian texels
//   CMPR   - 8x8 tiles of four 4x4 DXT1 sub-blocks (TL, TR, BL, BR); each
//            block is two big-endian 565 endpoints plus one byte per row,
//            2 bits per texel with the leftmost texel in the top bits
uint32_t TexelRGBA8(const uint8_t* texels, int tilesW, int x, int y)
{
	const uint8_t* tile = texels + ((y >> 2) * tilesW + (x >> 2)) * 64;
	int i = ((y & 3) * 4 + (x & 3)) * 2;
	return (uint32_t(tile[i]) << 24) | (uint32_t(tile[i + 1]) << 16)
		| (uint32_t(tile[32 + i]) << 8) | tile[32 + i + 1];
}

uint32_t TexelRGB565(const uint8_t* texels, int tilesW, int x, int y)
{
	const uint8_t* tile = texels + ((y >> 2) * tilesW + (x >> 2)) * 32;
	return Expand565(ReadBE16(tile + ((y & 3) * 4 + (x & 3)) * 2));
}

// RGB5A3 - 4x4 tiles like RGB565; top bit set means 1RRRRRGGGGGBBBBB,
//          clear means 0AAARRRRGGGGBBBB (expanded the way the GPU does)
uint32_t TexelRGB5A3(const uint8_t* texels, int tilesW, int x, int y)
{
	const uint8_t* tile = texels + ((y >> 2) * tilesW + (x >> 2)) * 32;
	uint16_t v = ReadBE16(tile + ((y & 3) * 4 + (x & 3)) * 2);
	if (v & 0x8000)
	{
		uint32_t r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
		r = (r << 3) | (r >> 2);
		g = (g << 3) | (g >> 2);
		b = (b << 3) | (b >> 2);
		return 0xFF000000u | (r << 16) | (g << 8) | b;
	}
	uint32_t a = (v >> 12) & 0x7;
	uint32_t r = ((v >> 8) & 0xF) * 0x11, g = ((v >> 4) & 0xF) * 0x11, b = (v & 0xF) * 0x11;
	a = (a << 5) | (a << 2) | (a >> 1);
	return (a << 24) | (r << 16) | (g << 8) | b;
}

uint32_t TexelCMPR(const uint8_t* texels, int tilesW, int x, int y)
{
	const uint8_t* tile = texels + ((y >> 3) * tilesW + (x >> 3)) * 32;
	const uint8_t* block = tile + (((y & 7) >> 2) * 2 + ((x & 7) >> 2)) * 8;
	uint16_t e0 = ReadBE16(block), e1 = ReadBE16(block + 2);
	uint32_t c0 = Expand565(e0), c1 = Expand565(e1);
	int idx = (block[4 + (y & 3)] >> (6 - 2 * (x & 3))) & 3;
	switch (idx)
	{
	case 0: return c0;
	case 1: return c1;
	case 2: return e0 > e1 ? Mix(c0, c1, 2, 1, 3) : Mix(c0, c1, 1, 1, 2);
	default: return e0 > e1 ? Mix(c0, c1, 1, 2, 3) : 0;
	}
}

} // namespace

uint32_t WiiTextureBytes(unsigned int theTexture)
{
	GXTexObj obj;
	if (!_ogx_texture_get_texobj(theTexture, &obj) || GX_GetTexObjData(&obj) == nullptr)
		return 0;
	return GX_GetTexBufferSize(GX_GetTexObjWidth(&obj), GX_GetTexObjHeight(&obj),
		GX_GetTexObjFmt(&obj), GX_FALSE, 0);
}

bool WiiReadTexturePixels(unsigned int theTexture, uint32_t* theDest, int theDestStride,
	int theWidth, int theHeight)
{
	GXTexObj obj;
	if (!_ogx_texture_get_texobj(theTexture, &obj))
		return false;

	void* phys = GX_GetTexObjData(&obj);
	if (phys == nullptr)
		return false;
	const uint8_t* texels = static_cast<const uint8_t*>(MEM_PHYSICAL_TO_K0(phys));
	int texW = GX_GetTexObjWidth(&obj);
	int texH = GX_GetTexObjHeight(&obj);
	if (theWidth > texW || theHeight > texH)
		return false;

	uint32_t (*texel)(const uint8_t*, int, int, int);
	int tilesW;
	switch (GX_GetTexObjFmt(&obj))
	{
	case GX_TF_RGBA8:  texel = TexelRGBA8;  tilesW = (texW + 3) / 4; break;
	case GX_TF_RGB565: texel = TexelRGB565; tilesW = (texW + 3) / 4; break;
	case GX_TF_RGB5A3: texel = TexelRGB5A3; tilesW = (texW + 3) / 4; break;
	case GX_TF_CMPR:   texel = TexelCMPR;   tilesW = (texW + 7) / 8; break;
	default:           return false;
	}

	for (int y = 0; y < theHeight; y++)
	{
		uint32_t* row = theDest + y * theDestStride;
		for (int x = 0; x < theWidth; x++)
			row[x] = texel(texels, tilesW, x, y);
	}
	return true;
}
