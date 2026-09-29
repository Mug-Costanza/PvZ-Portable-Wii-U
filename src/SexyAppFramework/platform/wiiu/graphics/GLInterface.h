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

// Wii U rendering backend.
//
// devkitPro ships no GX2/Latte shader compiler (unlike picasso for 3DS's
// PICA200), so this backend does not talk to GX2 directly. Instead it targets
// SDL2's SDL_Renderer API, which already has a working native GX2 renderer
// on Wii U (used to implement SDL_Renderer itself) - so this reuses that
// already-compiled shader pipeline instead of hand-rolling GX2 shaders.

#ifndef __GLINTERFACE_H__
#define __GLINTERFACE_H__

#include <SDL.h>
#include <mutex>
#include <set>
#include <vector>
#include "Common.h"
#include "graphics/MemoryImage.h"
#include "graphics/NativeDisplay.h"
#include "misc/Rect.h"
#include "misc/Ratio.h"
#include "misc/SexyMatrix.h"

namespace Sexy
{

class SexyAppBase;
class GLImage;
class SexyMatrix3;
class TriVertex;

typedef std::set<GLImage*> GLImageSet;

enum RenderImageFlags
{
	RenderImageFlag_MinimizeNumSubdivisions	=			0x0001,		// subdivide image into fewest possible textures (may use more memory)
	RenderImageFlag_Use64By64Subdivisions	=			0x0002,		// good to use with image strips so the entire texture isn't pulled in when drawing just a piece
	RenderImageFlag_UseA4R4G4B4				=			0x0004,		// unused on this backend (always full 32-bit textures; Wii U has plenty of RAM)
	RenderImageFlag_UseA8R8G8B8				=			0x0008,		// unused on this backend (always full 32-bit textures)
	RenderImageFlag_Repeat					=			0x0010,		// use wrapping (repeat) sampling for this texture
	RenderImageFlag_TextureMask				=			0x001F		// mask for flags that affect texture state/layout (excludes transient flags like sanding)
};

// One piece of a (possibly tiled) image's backing texture. Images larger than
// the renderer's max texture size get split into multiple pieces, same as
// every other GLInterface backend in this project.
struct TextureDataPiece
{
	SDL_Texture* mTexture;
	int mWidth, mHeight;
};

struct TextureData
{
public:
	typedef std::vector<TextureDataPiece> TextureVector;

	TextureVector mTextures;

	int mWidth, mHeight;
	int mTexVecWidth, mTexVecHeight;
	int mTexPieceWidth, mTexPieceHeight;
	int mBitsChangedCount;
	int mTexMemSize;
	float mMaxTotalU, mMaxTotalV;
	uint32_t mImageFlags;
	bool mHasTextures;

	TextureData();
	~TextureData();

	void ReleaseTextures();

	void CreateTextureDimensions(MemoryImage *theImage);
	void CreateTextures(MemoryImage *theImage);
	void CheckCreateTextures(MemoryImage *theImage);
	SDL_Texture* GetTexture(int x, int y, int &width, int &height, float &u1, float &v1, float &u2, float &v2);
	SDL_Texture* GetTextureF(float x, float y, float &width, float &height, float &u1, float &v1, float &u2, float &v2);

	void Blt(float theX, float theY, const Rect& theSrcRect, const Color& theColor);
	void BltTransformed(const SexyMatrix3 &theTrans, const Rect& theSrcRect, const Color& theColor, float theX = 0, float theY = 0, bool center = false);
	void BltTriangles(const TriVertex theVertices[][3], int theNumTriangles, unsigned int theColor, float tx = 0, float ty = 0, bool clampUv = true);
};

class GLInterface : public NativeDisplay
{
public:
	SexyAppBase*			mApp;
	SDL_Renderer*			mRenderer;

	std::mutex				mCritSect;
	int						mWidth;
	int						mHeight;
	int						mDisplayWidth;
	int						mDisplayHeight;

	Rect					mPresentationRect;
	int						mRefreshRate;
	int						mMillisecondsPerFrame;

	GLImage*				mScreenImage;

	int						mNextCursorX;
	int						mNextCursorY;
	int						mCursorX;
	int						mCursorY;

	typedef std::set<MemoryImage*> ImageSet;
	ImageSet mImageSet;
	GLImageSet				mGLImageSet;

	typedef std::list<SexyMatrix3> TransformStack;
	TransformStack mTransformStack;

	void					SetDrawMode(int theDrawMode);

public:
	void					AddGLImage(GLImage* theDDImage);
	void					RemoveGLImage(GLImage* theDDImage);
	void					Remove3DData(MemoryImage* theImage);

public:
	GLInterface(SexyAppBase* theApp);
	virtual ~GLInterface();

	GLImage*				GetScreenImage();
	void					UpdateViewport();
	int						Init(bool IsWindowed);
	bool					Redraw(Rect* theClipRect = nullptr);
	void					SetVideoOnlyDraw(bool videoOnly);

	void					SetCursorPos(int theCursorX, int theCursorY);

public:
	void					PushTransform(const SexyMatrix3 &theTransform, bool concatenate = true);
	void					PopTransform();

	bool					PreDraw();
	void					Flush();

	bool					CreateImageTexture(MemoryImage* theImage);
	bool					RecoverBits(MemoryImage* theImage);
	void					Blt(Image* theImage, float theX, float theY, const Rect& theSrcRect, const Color& theColor, int theDrawMode, bool linearFilter = true);
	void					BltClipF(Image* theImage, float theX, float theY, const Rect& theSrcRect, const Rect *theClipRect, const Color& theColor, int theDrawMode, bool linearFilter = true);
	void					BltMirror(Image* theImage, float theX, float theY, const Rect& theSrcRect, const Color& theColor, int theDrawMode, bool linearFilter = true);
	void					StretchBlt(Image* theImage,  const Rect& theDestRect, const Rect& theSrcRect, const Rect* theClipRect, const Color &theColor, int theDrawMode, bool fastStretch, bool mirror = false);
	void					BltRotated(Image* theImage, float theX, float theY, const Rect* theClipRect, const Color& theColor, int theDrawMode, double theRot, float theRotCenterX, float theRotCenterY, const Rect& theSrcRect);
	void					BltTransformed(Image* theImage, const Rect* theClipRect, const Color& theColor, int theDrawMode, const Rect &theSrcRect, const SexyMatrix3 &theTransform, bool linearFilter, float theX = 0, float theY = 0, bool center = false);
	void					DrawLine(double theStartX, double theStartY, double theEndX, double theEndY, const Color& theColor, int theDrawMode);
	void					FillRect(const Rect& theRect, const Color& theColor, int theDrawMode);
	void					DrawTriangle(const TriVertex &p1, const TriVertex &p2, const TriVertex &p3, const Color &theColor, int theDrawMode);
	void					DrawTriangleTex(const TriVertex &p1, const TriVertex &p2, const TriVertex &p3, const Color &theColor, int theDrawMode, Image *theTexture, bool blend = true);
	void					DrawTrianglesTex(const TriVertex theVertices[][3], int theNumTriangles, const Color &theColor, int theDrawMode, Image *theTexture, float tx = 0, float ty = 0, bool blend = true);
	void					DrawTrianglesTexStrip(const TriVertex theVertices[], int theNumTriangles, const Color &theColor, int theDrawMode, Image *theTexture, float tx = 0, float ty = 0, bool blend = true);
	void					FillPoly(const Point theVertices[], int theNumVertices, const Rect *theClipRect, const Color &theColor, int theDrawMode, int tx, int ty);
};

}

#endif // __GLINTERFACE_H__
