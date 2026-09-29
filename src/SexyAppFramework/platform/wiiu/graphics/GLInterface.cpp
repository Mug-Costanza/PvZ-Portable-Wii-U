/*
 * Portions of this file are based on the PopCap Games Framework
 * Copyright (C) 2005-2009 PopCap Games, Inc.
 *
 * Copyright (C) 2026 Zhou Qiankang <wszqkzqk@qq.com>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later AND LicenseRef-PopCap
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

#include <SDL.h>

#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "graphics/Graphics.h"
#include "graphics/MemoryImage.h"
#include "SexyAppBase.h"
#include <algorithm>
#include <vector>

using namespace Sexy;

// Wii U CPU (Espresso) is big-endian PowerPC, unlike every other platform
// this project targets. Colors here are therefore never packed into a
// uint32_t and bit-shifted the way the desktop backend does (that trick only
// works on little-endian hosts) - every color is carried as separate SDL_Color
// bytes from the point it's first read, so there is nothing to get backwards.

static SDL_Renderer* gRenderer = nullptr;
static int gMaxTextureSize = 4096;
static SDL_BlendMode gCurrentBlendMode = SDL_BLENDMODE_BLEND;
static bool gLinearFilter = false;

static SDL_BlendMode GetAdditiveBlendMode()
{
	static SDL_BlendMode aMode = SDL_ComposeCustomBlendMode(
		SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
		SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
	return aMode;
}

static inline SDL_Color ColorToSDLColor(const Color &theColor)
{
	SDL_Color c;
	c.r = (Uint8)theColor.mRed;
	c.g = (Uint8)theColor.mGreen;
	c.b = (Uint8)theColor.mBlue;
	c.a = (Uint8)theColor.mAlpha;
	return c;
}

// theArgb is a plain logical 0xAARRGGBB integer (as documented on
// TriVertex::color) - not a byte-order-normalized GPU value - so extracting
// channels via bit shifts here is safe on any host endianness.
static inline SDL_Color ArgbToSDLColor(uint32_t theArgb, const SDL_Color &theFallback)
{
	if (theArgb == 0)
		return theFallback;
	SDL_Color c;
	c.a = (Uint8)((theArgb >> 24) & 0xFF);
	c.r = (Uint8)((theArgb >> 16) & 0xFF);
	c.g = (Uint8)((theArgb >> 8) & 0xFF);
	c.b = (Uint8)(theArgb & 0xFF);
	return c;
}

static inline uint32_t ColorToArgb(const Color &theColor)
{
	return ((uint32_t)theColor.mAlpha << 24) | ((uint32_t)theColor.mRed << 16)
		| ((uint32_t)theColor.mGreen << 8) | (uint32_t)theColor.mBlue;
}

struct WUVertex
{
	float x, y;
	float u, v;
	SDL_Color color;
};

static void DrawTexturedQuad(SDL_Texture *theTexture, const WUVertex theVerts[4])
{
	SDL_SetTextureBlendMode(theTexture, gCurrentBlendMode);
	SDL_SetTextureScaleMode(theTexture, gLinearFilter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);

	SDL_Vertex v[4];
	for (int i = 0; i < 4; i++)
	{
		v[i].position.x = theVerts[i].x;
		v[i].position.y = theVerts[i].y;
		v[i].tex_coord.x = theVerts[i].u;
		v[i].tex_coord.y = theVerts[i].v;
		v[i].color = theVerts[i].color;
	}
	static const int kQuadIndices[6] = { 0, 1, 2, 1, 2, 3 };
	SDL_RenderGeometry(gRenderer, theTexture, v, 4, kQuadIndices, 6);
}

//////////////////////////////////////////////////////////////////////////////
// TextureData
//////////////////////////////////////////////////////////////////////////////

TextureData::TextureData()
	: mWidth(0), mHeight(0), mTexVecWidth(0), mTexVecHeight(0),
	  mTexPieceWidth(0), mTexPieceHeight(0), mBitsChangedCount(0), mTexMemSize(0),
	  mMaxTotalU(1.0f), mMaxTotalV(1.0f), mImageFlags(0), mHasTextures(false)
{
}

TextureData::~TextureData()
{
	ReleaseTextures();
}

void TextureData::ReleaseTextures()
{
	for (auto &piece : mTextures)
	{
		if (piece.mTexture)
			SDL_DestroyTexture(piece.mTexture);
	}
	mTextures.clear();
	mTexMemSize = 0;
	mHasTextures = false;
}

void TextureData::CreateTextureDimensions(MemoryImage *theImage)
{
	int aWidth = theImage->GetWidth();
	int aHeight = theImage->GetHeight();

	mTexPieceWidth = std::min(aWidth, gMaxTextureSize);
	mTexPieceHeight = std::min(aHeight, gMaxTextureSize);
	if (mImageFlags & RenderImageFlag_Use64By64Subdivisions)
	{
		mTexPieceWidth = std::min(64, gMaxTextureSize);
		mTexPieceHeight = std::min(64, gMaxTextureSize);
	}

	mTexVecWidth = (aWidth + mTexPieceWidth - 1) / mTexPieceWidth;
	mTexVecHeight = (aHeight + mTexPieceHeight - 1) / mTexPieceHeight;

	mTextures.assign((size_t)mTexVecWidth * mTexVecHeight, TextureDataPiece{ nullptr, 0, 0 });
	for (int row = 0; row < mTexVecHeight; row++)
	{
		for (int col = 0; col < mTexVecWidth; col++)
		{
			TextureDataPiece &p = mTextures[row * mTexVecWidth + col];
			p.mWidth = std::min(mTexPieceWidth, aWidth - col * mTexPieceWidth);
			p.mHeight = std::min(mTexPieceHeight, aHeight - row * mTexPieceHeight);
		}
	}

	mMaxTotalU = aWidth / (float)mTexPieceWidth;
	mMaxTotalV = aHeight / (float)mTexPieceHeight;
}

void TextureData::CreateTextures(MemoryImage *theImage)
{
	theImage->DeleteSWBuffers();
	theImage->CommitBits();

	bool wantRecreateDims = !mHasTextures
		|| mWidth != theImage->mWidth || mHeight != theImage->mHeight
		|| (theImage->mRenderFlags & RenderImageFlag_TextureMask) != mImageFlags;

	if (wantRecreateDims)
	{
		ReleaseTextures();
		mImageFlags = theImage->mRenderFlags & RenderImageFlag_TextureMask;
		CreateTextureDimensions(theImage);
	}

	int aWidth = theImage->GetWidth();
	int aHeight = theImage->GetHeight();

	// Tightly-packed R,G,B,A bytes per pixel, in that memory order - matches
	// SDL_PIXELFORMAT_RGBA32 regardless of host endianness (SDL resolves that
	// constant to whichever real format has this memory layout).
	static std::vector<uint8_t> sScratch;

	int idx = 0;
	for (int row = 0; row < mTexVecHeight; row++)
	{
		for (int col = 0; col < mTexVecWidth; col++, idx++)
		{
			TextureDataPiece &piece = mTextures[idx];
			int offx = col * mTexPieceWidth;
			int offy = row * mTexPieceHeight;

			sScratch.resize((size_t)piece.mWidth * piece.mHeight * 4);
			uint8_t *dst = sScratch.data();

			if (theImage->mColorTable == nullptr)
			{
				uint32_t *srcRow = theImage->GetBits() + (size_t)offy * aWidth + offx;
				for (int y = 0; y < piece.mHeight; y++)
				{
					uint32_t *s = srcRow;
					for (int x = 0; x < piece.mWidth; x++)
					{
						uint32_t argb = *s++;
						*dst++ = (uint8_t)((argb >> 16) & 0xFF);
						*dst++ = (uint8_t)((argb >> 8) & 0xFF);
						*dst++ = (uint8_t)(argb & 0xFF);
						*dst++ = (uint8_t)((argb >> 24) & 0xFF);
					}
					srcRow += aWidth;
				}
			}
			else
			{
				uint8_t *srcRow = theImage->mColorIndices + (size_t)offy * aWidth + offx;
				uint32_t *pal = theImage->mColorTable;
				for (int y = 0; y < piece.mHeight; y++)
				{
					uint8_t *s = srcRow;
					for (int x = 0; x < piece.mWidth; x++)
					{
						uint32_t argb = pal[*s++];
						*dst++ = (uint8_t)((argb >> 16) & 0xFF);
						*dst++ = (uint8_t)((argb >> 8) & 0xFF);
						*dst++ = (uint8_t)(argb & 0xFF);
						*dst++ = (uint8_t)((argb >> 24) & 0xFF);
					}
					srcRow += aWidth;
				}
			}

			if (wantRecreateDims || piece.mTexture == nullptr)
			{
				if (piece.mTexture)
					SDL_DestroyTexture(piece.mTexture);
				piece.mTexture = SDL_CreateTexture(gRenderer, SDL_PIXELFORMAT_RGBA32,
					SDL_TEXTUREACCESS_TARGET, piece.mWidth, piece.mHeight);
				mTexMemSize += piece.mWidth * piece.mHeight * 4;
			}

			SDL_UpdateTexture(piece.mTexture, nullptr, sScratch.data(), piece.mWidth * 4);
		}
	}

	mWidth = theImage->mWidth;
	mHeight = theImage->mHeight;
	mBitsChangedCount = theImage->mBitsChangedCount;
	mHasTextures = true;
}

void TextureData::CheckCreateTextures(MemoryImage *theImage)
{
	if (!mHasTextures
		|| theImage->mWidth != mWidth || theImage->mHeight != mHeight
		|| theImage->mBitsChangedCount != mBitsChangedCount
		|| (theImage->mRenderFlags & RenderImageFlag_TextureMask) != mImageFlags)
		CreateTextures(theImage);
}

SDL_Texture* TextureData::GetTexture(int x, int y, int &width, int &height,
	float &u1, float &v1, float &u2, float &v2)
{
	int tx = x / mTexPieceWidth, ty = y / mTexPieceHeight;
	TextureDataPiece &p = mTextures[ty * mTexVecWidth + tx];

	int left = x % mTexPieceWidth, top = y % mTexPieceHeight;
	int right = std::min(left + width, p.mWidth);
	int bottom = std::min(top + height, p.mHeight);
	width = right - left;
	height = bottom - top;

	u1 = (float)left / p.mWidth;   v1 = (float)top / p.mHeight;
	u2 = (float)right / p.mWidth;  v2 = (float)bottom / p.mHeight;
	return p.mTexture;
}

SDL_Texture* TextureData::GetTextureF(float x, float y, float &width, float &height,
	float &u1, float &v1, float &u2, float &v2)
{
	int tx = (int)(x / mTexPieceWidth), ty = (int)(y / mTexPieceHeight);
	TextureDataPiece &p = mTextures[ty * mTexVecWidth + tx];

	float left = x - tx * mTexPieceWidth;
	float top = y - ty * mTexPieceHeight;
	float right = std::min(left + width, (float)p.mWidth);
	float bottom = std::min(top + height, (float)p.mHeight);
	width = right - left;
	height = bottom - top;

	u1 = left / p.mWidth;   v1 = top / p.mHeight;
	u2 = right / p.mWidth; v2 = bottom / p.mHeight;
	return p.mTexture;
}

void TextureData::Blt(float theX, float theY, const Rect &theSrcRect, const Color &theColor)
{
	int srcLeft = theSrcRect.mX, srcTop = theSrcRect.mY;
	int srcRight = srcLeft + theSrcRect.mWidth, srcBottom = srcTop + theSrcRect.mHeight;
	if (srcLeft >= srcRight || srcTop >= srcBottom)
		return;

	SDL_Color col = ColorToSDLColor(theColor);

	int srcY = srcTop;
	float dstY = theY;
	while (srcY < srcBottom)
	{
		int srcX = srcLeft;
		float dstX = theX;
		int h = 0;
		while (srcX < srcRight)
		{
			int w = srcRight - srcX;
			h = srcBottom - srcY;
			float u1, v1, u2, v2;
			SDL_Texture *tex = GetTexture(srcX, srcY, w, h, u1, v1, u2, v2);

			WUVertex v[4] = {
				{ dstX,     dstY,     u1, v1, col },
				{ dstX,     dstY + h, u1, v2, col },
				{ dstX + w, dstY,     u2, v1, col },
				{ dstX + w, dstY + h, u2, v2, col },
			};
			DrawTexturedQuad(tex, v);

			srcX += w; dstX += w;
		}
		srcY += h; dstY += h;
	}
}

void TextureData::BltTransformed(const SexyMatrix3 &theTrans, const Rect &theSrcRect,
	const Color &theColor, float theX, float theY, bool center)
{
	int srcLeft = theSrcRect.mX, srcTop = theSrcRect.mY;
	int srcRight = srcLeft + theSrcRect.mWidth, srcBottom = srcTop + theSrcRect.mHeight;
	if (srcLeft >= srcRight || srcTop >= srcBottom)
		return;

	float startx = 0, starty = 0;
	if (center)
	{
		startx = -theSrcRect.mWidth / 2.0f;
		starty = -theSrcRect.mHeight / 2.0f;
	}

	SDL_Color col = ColorToSDLColor(theColor);

	int srcY = srcTop;
	float dstY = starty;
	while (srcY < srcBottom)
	{
		int srcX = srcLeft;
		float dstX = startx;
		int h = 0;
		while (srcX < srcRight)
		{
			int w = srcRight - srcX;
			h = srcBottom - srcY;
			float u1, v1, u2, v2;
			SDL_Texture *tex = GetTexture(srcX, srcY, w, h, u1, v1, u2, v2);

			float x = dstX, y = dstY;
			SexyVector2 p[4] = { {x, y}, {x, y + h}, {x + w, y}, {x + w, y + h} };
			SexyVector2 tp[4];
			for (int i = 0; i < 4; i++)
			{
				tp[i] = theTrans * p[i];
				tp[i].x += theX;
				tp[i].y += theY;
			}

			WUVertex v[4] = {
				{ tp[0].x, tp[0].y, u1, v1, col },
				{ tp[1].x, tp[1].y, u1, v2, col },
				{ tp[2].x, tp[2].y, u2, v1, col },
				{ tp[3].x, tp[3].y, u2, v2, col },
			};
			DrawTexturedQuad(tex, v);

			srcX += w; dstX += w;
		}
		srcY += h; dstY += h;
	}
}

void TextureData::BltTriangles(const TriVertex theVertices[][3], int theNumTriangles,
	unsigned int theColor, float tx, float ty, bool clampUv)
{
	// clampUv is accepted for interface parity with the other backends but is
	// a no-op here: SDL_Renderer has no public texture-wrap-mode API, so
	// RenderImageFlag_Repeat textures (used only by the decorative Zen Garden
	// pool caustic effect) are drawn clamped instead of tiled.
	(void)clampUv;

	SDL_Color fallback = ArgbToSDLColor(theColor, SDL_Color{ 255, 255, 255, 255 });

	if (mTextures.size() == 1)
	{
		// Common case - and in practice the only case, since GX2 supports
		// far larger textures than anything this game ships: draw every
		// triangle directly against the single piece.
		TextureDataPiece &piece = mTextures[0];
		std::vector<SDL_Vertex> verts((size_t)theNumTriangles * 3);
		for (int tri = 0; tri < theNumTriangles; tri++)
		{
			const TriVertex *tv = theVertices[tri];
			for (int i = 0; i < 3; i++)
			{
				SDL_Vertex &sv = verts[(size_t)tri * 3 + i];
				sv.position.x = tv[i].x + tx;
				sv.position.y = tv[i].y + ty;
				sv.tex_coord.x = tv[i].u * mMaxTotalU;
				sv.tex_coord.y = tv[i].v * mMaxTotalV;
				sv.color = ArgbToSDLColor(tv[i].color, fallback);
			}
		}
		SDL_SetTextureBlendMode(piece.mTexture, gCurrentBlendMode);
		SDL_SetTextureScaleMode(piece.mTexture, gLinearFilter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
		SDL_RenderGeometry(gRenderer, piece.mTexture, verts.data(), (int)verts.size(), nullptr, 0);
		return;
	}

	// Rare multi-piece path for an image larger than the renderer's max
	// texture size: bind each triangle to whichever piece its first vertex
	// falls in. A triangle that straddles a piece boundary could show a
	// visible seam here, but given the max texture size on this hardware,
	// no asset this game ships should ever actually take this path.
	for (int tri = 0; tri < theNumTriangles; tri++)
	{
		const TriVertex *tv = theVertices[tri];
		float u0 = tv[0].u * mMaxTotalU * mTexPieceWidth;
		float v0 = tv[0].v * mMaxTotalV * mTexPieceHeight;
		int col = std::clamp((int)(u0 / mTexPieceWidth), 0, mTexVecWidth - 1);
		int row = std::clamp((int)(v0 / mTexPieceHeight), 0, mTexVecHeight - 1);
		TextureDataPiece &piece = mTextures[row * mTexVecWidth + col];

		SDL_Vertex sv[3];
		for (int i = 0; i < 3; i++)
		{
			float u = tv[i].u * mMaxTotalU * mTexPieceWidth - col * mTexPieceWidth;
			float v = tv[i].v * mMaxTotalV * mTexPieceHeight - row * mTexPieceHeight;
			sv[i].position.x = tv[i].x + tx;
			sv[i].position.y = tv[i].y + ty;
			sv[i].tex_coord.x = u / piece.mWidth;
			sv[i].tex_coord.y = v / piece.mHeight;
			sv[i].color = ArgbToSDLColor(tv[i].color, fallback);
		}
		SDL_SetTextureBlendMode(piece.mTexture, gCurrentBlendMode);
		SDL_SetTextureScaleMode(piece.mTexture, gLinearFilter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
		SDL_RenderGeometry(gRenderer, piece.mTexture, sv, 3, nullptr, 0);
	}
}

//////////////////////////////////////////////////////////////////////////////
// GLInterface
//////////////////////////////////////////////////////////////////////////////

GLInterface::GLInterface(SexyAppBase *theApp)
{
	mApp = theApp;
	mRenderer = nullptr;
	mWidth = mApp->mWidth;
	mHeight = mApp->mHeight;
	mDisplayWidth = mWidth;
	mDisplayHeight = mHeight;
	mPresentationRect = Rect(0, 0, mWidth, mHeight);
	mRefreshRate = 60;
	mMillisecondsPerFrame = 1000 / mRefreshRate;
	mScreenImage = nullptr;
	mNextCursorX = mNextCursorY = 0;
	mCursorX = mCursorY = 0;
}

GLInterface::~GLInterface()
{
	for (auto *img : mImageSet)
	{
		delete (TextureData*)img->mRenderData;
		img->mRenderData = nullptr;
	}
}

void GLInterface::SetDrawMode(int theDrawMode)
{
	gCurrentBlendMode = (theDrawMode == Graphics::DRAWMODE_NORMAL) ? SDL_BLENDMODE_BLEND : GetAdditiveBlendMode();
	SDL_SetRenderDrawBlendMode(mRenderer, gCurrentBlendMode);
}

void GLInterface::AddGLImage(GLImage *theGLImage)
{
	std::scoped_lock lk(mCritSect);
	mGLImageSet.insert(theGLImage);
}

void GLInterface::RemoveGLImage(GLImage *theGLImage)
{
	std::scoped_lock lk(mCritSect);
	mGLImageSet.erase(theGLImage);
}

void GLInterface::Remove3DData(MemoryImage *theImage)
{
	if (theImage->mRenderData)
	{
		delete (TextureData*)theImage->mRenderData;
		theImage->mRenderData = nullptr;
		std::scoped_lock lk(mCritSect);
		mImageSet.erase(theImage);
	}
}

GLImage* GLInterface::GetScreenImage() { return mScreenImage; }

void GLInterface::UpdateViewport()
{
	int width, height;
	SDL_GetRendererOutputSize(mRenderer, &width, &height);

	int vx = 0, vy = 0, vw = width, vh = height;

	// Letterbox to 4:3, matching every other backend in this project.
	if (width * 3 > height * 4)
	{
		vw = height * 4 / 3;
		vx = (width - vw) / 2;
	}
	else if (width * 3 < height * 4)
	{
		vh = width * 3 / 4;
		vy = (height - vh) / 2;
	}

	mPresentationRect = Rect(vx, vy, vw, vh);
}

int GLInterface::Init(bool /*IsWindowed*/)
{
	mRenderer = (SDL_Renderer*)mApp->mContext;
	gRenderer = mRenderer;

	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(mRenderer, &info) == 0 && info.max_texture_width > 0 && info.max_texture_height > 0)
		gMaxTextureSize = std::min(info.max_texture_width, info.max_texture_height);
	else
		gMaxTextureSize = 4096; // conservative fallback if the driver doesn't report a limit

	SDL_RenderSetLogicalSize(mRenderer, mWidth, mHeight);
	SDL_SetRenderDrawBlendMode(mRenderer, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(mRenderer, 0, 0, 0, 255);
	SDL_RenderClear(mRenderer);

	mRGBBits = 32;
	mRedBits = 8; mGreenBits = 8; mBlueBits = 8;
	mRedShift = 0; mGreenShift = 8; mBlueShift = 16;
	mRedMask = 0xFFu << mRedShift;
	mGreenMask = 0xFFu << mGreenShift;
	mBlueMask = 0xFFu << mBlueShift;

	UpdateViewport();
	SetVideoOnlyDraw(false);
	return 1;
}

bool GLInterface::Redraw(Rect*)
{
	Flush();
	return true;
}

void GLInterface::SetVideoOnlyDraw(bool)
{
	delete mScreenImage;
	mScreenImage = new GLImage(this);
	mScreenImage->mWidth = mWidth;
	mScreenImage->mHeight = mHeight;
	mScreenImage->SetImageMode(false, false);
}

void GLInterface::SetCursorPos(int x, int y)
{
	mNextCursorX = x;
	mNextCursorY = y;
}

bool GLInterface::PreDraw()
{
	return true;
}

void GLInterface::Flush()
{
	SDL_RenderPresent(mRenderer);
	SDL_SetRenderDrawColor(mRenderer, 0, 0, 0, 255);
	SDL_RenderClear(mRenderer);
}

bool GLInterface::CreateImageTexture(MemoryImage *theImage)
{
	bool wantPurge = false;
	if (theImage->mRenderData == nullptr)
	{
		theImage->mRenderData = new TextureData();
		wantPurge = theImage->mPurgeBits;
		std::scoped_lock lk(mCritSect);
		mImageSet.insert(theImage);
	}

	TextureData *data = (TextureData*)theImage->mRenderData;
	data->CheckCreateTextures(theImage);

	if (wantPurge)
		theImage->PurgeBits();
	return data->mHasTextures;
}

bool GLInterface::RecoverBits(MemoryImage *theImage)
{
	if (!theImage->mRenderData)
		return false;

	TextureData *data = (TextureData*)theImage->mRenderData;
	if (data->mBitsChangedCount != theImage->mBitsChangedCount)
		return false;

	SDL_Texture *prevTarget = SDL_GetRenderTarget(mRenderer);
	std::vector<uint8_t> buf;

	for (int row = 0; row < data->mTexVecHeight; row++)
	{
		for (int col = 0; col < data->mTexVecWidth; col++)
		{
			TextureDataPiece &piece = data->mTextures[row * data->mTexVecWidth + col];
			int offx = col * data->mTexPieceWidth;
			int offy = row * data->mTexPieceHeight;

			if (SDL_SetRenderTarget(mRenderer, piece.mTexture) != 0)
			{
				SDL_SetRenderTarget(mRenderer, prevTarget);
				return false;
			}

			buf.resize((size_t)piece.mWidth * piece.mHeight * 4);
			bool ok = SDL_RenderReadPixels(mRenderer, nullptr, SDL_PIXELFORMAT_RGBA32, buf.data(), piece.mWidth * 4) == 0;
			if (!ok)
			{
				SDL_SetRenderTarget(mRenderer, prevTarget);
				return false;
			}

			uint32_t *dst = theImage->GetBits() + (size_t)offy * theImage->GetWidth() + offx;
			const uint8_t *src = buf.data();
			for (int y = 0; y < piece.mHeight; y++)
			{
				uint32_t *d = dst;
				const uint8_t *s = src;
				for (int x = 0; x < piece.mWidth; x++)
				{
					uint32_t r = s[0], g = s[1], b = s[2], a = s[3];
					*d++ = (a << 24) | (r << 16) | (g << 8) | b;
					s += 4;
				}
				dst += theImage->GetWidth();
				src += (size_t)piece.mWidth * 4;
			}
		}
	}

	SDL_SetRenderTarget(mRenderer, prevTarget);
	return true;
}

void GLInterface::PushTransform(const SexyMatrix3 &theTransform, bool concatenate)
{
	if (mTransformStack.empty() || !concatenate)
		mTransformStack.push_back(theTransform);
	else
		mTransformStack.push_back(theTransform * mTransformStack.back());
}

void GLInterface::PopTransform()
{
	if (!mTransformStack.empty())
		mTransformStack.pop_back();
}

void GLInterface::Blt(Image *theImage, float theX, float theY,
	const Rect &theSrcRect, const Color &theColor, int theDrawMode, bool linearFilter)
{
	if (!mTransformStack.empty())
	{
		BltClipF(theImage, theX, theY, theSrcRect, nullptr, theColor, theDrawMode, linearFilter);
		return;
	}
	if (!PreDraw()) return;

	MemoryImage *mem = (MemoryImage*)theImage;
	if (!CreateImageTexture(mem)) return;

	SetDrawMode(theDrawMode);
	gLinearFilter = linearFilter;
	((TextureData*)mem->mRenderData)->Blt(theX, theY, theSrcRect, theColor);
}

void GLInterface::BltClipF(Image *theImage, float theX, float theY,
	const Rect &theSrcRect, const Rect *theClipRect, const Color &theColor, int theDrawMode, bool linearFilter)
{
	SexyTransform2D t;
	t.Translate(theX, theY);
	BltTransformed(theImage, theClipRect, theColor, theDrawMode, theSrcRect, t, linearFilter);
}

void GLInterface::BltMirror(Image *theImage, float theX, float theY,
	const Rect &theSrcRect, const Color &theColor, int theDrawMode, bool linearFilter)
{
	SexyTransform2D t;
	t.Translate(-theSrcRect.mWidth, 0);
	t.Scale(-1, 1);
	t.Translate(theX, theY);
	BltTransformed(theImage, nullptr, theColor, theDrawMode, theSrcRect, t, linearFilter);
}

void GLInterface::StretchBlt(Image *theImage, const Rect &theDestRect,
	const Rect &theSrcRect, const Rect *theClipRect, const Color &theColor,
	int theDrawMode, bool fastStretch, bool mirror)
{
	float xs = (float)theDestRect.mWidth / theSrcRect.mWidth;
	float ys = (float)theDestRect.mHeight / theSrcRect.mHeight;

	SexyTransform2D t;
	if (mirror)
	{
		t.Translate(-theSrcRect.mWidth, 0);
		t.Scale(-xs, ys);
	}
	else
		t.Scale(xs, ys);

	t.Translate(theDestRect.mX, theDestRect.mY);
	BltTransformed(theImage, theClipRect, theColor, theDrawMode, theSrcRect, t, !fastStretch);
}

void GLInterface::BltRotated(Image *theImage, float theX, float theY,
	const Rect *theClipRect, const Color &theColor, int theDrawMode,
	double theRot, float theRotCenterX, float theRotCenterY, const Rect &theSrcRect)
{
	SexyTransform2D t;
	t.Translate(-theRotCenterX, -theRotCenterY);
	t.RotateRad(theRot);
	t.Translate(theX + theRotCenterX, theY + theRotCenterY);
	BltTransformed(theImage, theClipRect, theColor, theDrawMode, theSrcRect, t, true);
}

void GLInterface::BltTransformed(Image *theImage, const Rect *theClipRect,
	const Color &theColor, int theDrawMode, const Rect &theSrcRect,
	const SexyMatrix3 &theTransform, bool linearFilter, float theX, float theY, bool center)
{
	if (!PreDraw()) return;

	MemoryImage *mem = (MemoryImage*)theImage;
	if (!CreateImageTexture(mem)) return;

	SetDrawMode(theDrawMode);
	gLinearFilter = linearFilter;
	TextureData *data = (TextureData*)mem->mRenderData;

	SexyMatrix3 finalTransform = theTransform;
	float finalX = theX, finalY = theY;
	bool finalCenter = center;

	if (!mTransformStack.empty())
	{
		if (theX != 0 || theY != 0)
		{
			SexyTransform2D t;
			if (center) t.Translate(-theSrcRect.mWidth / 2.0f, -theSrcRect.mHeight / 2.0f);
			t = theTransform * t;
			t.Translate(theX, theY);
			finalTransform = mTransformStack.back() * t;
			finalX = 0; finalY = 0; finalCenter = false;
		}
		else
		{
			finalTransform = mTransformStack.back() * theTransform;
		}
	}

	bool clip = (theClipRect != nullptr);
	if (clip)
	{
		SDL_Rect r{ theClipRect->mX, theClipRect->mY, theClipRect->mWidth, theClipRect->mHeight };
		SDL_RenderSetClipRect(mRenderer, &r);
	}

	data->BltTransformed(finalTransform, theSrcRect, theColor, finalX, finalY, finalCenter);

	if (clip)
		SDL_RenderSetClipRect(mRenderer, nullptr);
}

void GLInterface::DrawLine(double x1, double y1, double x2, double y2,
	const Color &theColor, int theDrawMode)
{
	if (!PreDraw()) return;
	SetDrawMode(theDrawMode);

	float fx1 = (float)x1, fy1 = (float)y1, fx2 = (float)x2, fy2 = (float)y2;
	if (!mTransformStack.empty())
	{
		SexyVector2 p1((float)x1, (float)y1), p2((float)x2, (float)y2);
		p1 = mTransformStack.back() * p1;
		p2 = mTransformStack.back() * p2;
		fx1 = p1.x; fy1 = p1.y; fx2 = p2.x; fy2 = p2.y;
	}

	SDL_Color c = ColorToSDLColor(theColor);
	SDL_SetRenderDrawColor(mRenderer, c.r, c.g, c.b, c.a);
	SDL_RenderDrawLineF(mRenderer, fx1, fy1, fx2, fy2);
}

void GLInterface::FillRect(const Rect &theRect, const Color &theColor, int theDrawMode)
{
	if (!PreDraw()) return;
	SetDrawMode(theDrawMode);

	float x = (float)theRect.mX, y = (float)theRect.mY;
	float w = (float)theRect.mWidth, h = (float)theRect.mHeight;
	SDL_Color col = ColorToSDLColor(theColor);

	SexyVector2 p[4] = { {x, y}, {x, y + h}, {x + w, y}, {x + w, y + h} };
	if (!mTransformStack.empty())
	{
		for (int i = 0; i < 4; i++)
			p[i] = mTransformStack.back() * p[i];
	}

	SDL_Vertex v[4];
	for (int i = 0; i < 4; i++)
	{
		v[i].position.x = p[i].x;
		v[i].position.y = p[i].y;
		v[i].tex_coord.x = 0; v[i].tex_coord.y = 0;
		v[i].color = col;
	}
	static const int kQuadIndices[6] = { 0, 1, 2, 1, 2, 3 };
	SDL_RenderGeometry(mRenderer, nullptr, v, 4, kQuadIndices, 6);
}

void GLInterface::DrawTriangle(const TriVertex &p1, const TriVertex &p2, const TriVertex &p3,
	const Color &theColor, int theDrawMode)
{
	if (!PreDraw()) return;
	SetDrawMode(theDrawMode);

	SDL_Color fallback = ColorToSDLColor(theColor);
	const TriVertex *pts[3] = { &p1, &p2, &p3 };

	SDL_Vertex v[3];
	for (int i = 0; i < 3; i++)
	{
		v[i].position.x = pts[i]->x;
		v[i].position.y = pts[i]->y;
		v[i].tex_coord.x = 0; v[i].tex_coord.y = 0;
		v[i].color = ArgbToSDLColor(pts[i]->color, fallback);
	}
	SDL_RenderGeometry(mRenderer, nullptr, v, 3, nullptr, 0);
}

void GLInterface::DrawTriangleTex(const TriVertex &p1, const TriVertex &p2, const TriVertex &p3,
	const Color &theColor, int theDrawMode, Image *theTexture, bool blend)
{
	TriVertex arr[1][3] = { {p1, p2, p3} };
	DrawTrianglesTex(arr, 1, theColor, theDrawMode, theTexture, 0, 0, blend);
}

void GLInterface::DrawTrianglesTex(const TriVertex theVertices[][3], int theNumTriangles,
	const Color &theColor, int theDrawMode, Image *theTexture, float tx, float ty, bool blend)
{
	if (!PreDraw()) return;

	MemoryImage *mem = (MemoryImage*)theTexture;
	if (!CreateImageTexture(mem)) return;

	SetDrawMode(theDrawMode);
	gLinearFilter = blend;

	uint32_t c = ColorToArgb(theColor);
	bool clampUv = (mem->mRenderFlags & RenderImageFlag_Repeat) == 0;
	((TextureData*)mem->mRenderData)->BltTriangles(theVertices, theNumTriangles, c, tx, ty, clampUv);
}

void GLInterface::DrawTrianglesTexStrip(const TriVertex theVertices[], int theNumTriangles,
	const Color &theColor, int theDrawMode, Image *theTexture, float tx, float ty, bool blend)
{
	TriVertex batch[100][3];
	int done = 0;
	while (done < theNumTriangles)
	{
		int n = std::min(100, theNumTriangles - done);
		for (int i = 0; i < n; i++)
		{
			batch[i][0] = theVertices[done];
			batch[i][1] = theVertices[done + 1];
			batch[i][2] = theVertices[done + 2];
			done++;
		}
		DrawTrianglesTex(batch, n, theColor, theDrawMode, theTexture, tx, ty, blend);
	}
}

void GLInterface::FillPoly(const Point theVertices[], int theNumVertices,
	const Rect *theClipRect, const Color &theColor, int theDrawMode, int tx, int ty)
{
	if (theNumVertices < 3) return;
	if (!PreDraw()) return;
	SetDrawMode(theDrawMode);

	SDL_Color col = ColorToSDLColor(theColor);
	std::vector<SDL_Vertex> verts(theNumVertices);
	for (int i = 0; i < theNumVertices; i++)
	{
		SexyVector2 p((float)(theVertices[i].mX + tx), (float)(theVertices[i].mY + ty));
		if (!mTransformStack.empty())
			p = mTransformStack.back() * p;
		verts[i].position.x = p.x;
		verts[i].position.y = p.y;
		verts[i].tex_coord.x = 0; verts[i].tex_coord.y = 0;
		verts[i].color = col;
	}

	std::vector<int> idx((size_t)(theNumVertices - 2) * 3);
	for (int i = 0; i < theNumVertices - 2; i++)
	{
		idx[(size_t)i * 3 + 0] = 0;
		idx[(size_t)i * 3 + 1] = i + 1;
		idx[(size_t)i * 3 + 2] = i + 2;
	}

	bool clip = (theClipRect != nullptr);
	if (clip)
	{
		SDL_Rect r{ theClipRect->mX, theClipRect->mY, theClipRect->mWidth, theClipRect->mHeight };
		SDL_RenderSetClipRect(mRenderer, &r);
	}

	SDL_RenderGeometry(mRenderer, nullptr, verts.data(), theNumVertices, idx.data(), (int)idx.size());

	if (clip)
		SDL_RenderSetClipRect(mRenderer, nullptr);
}
