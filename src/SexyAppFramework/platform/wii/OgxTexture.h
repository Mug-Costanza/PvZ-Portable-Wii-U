#ifndef __OGXTEXTURE_H__
#define __OGXTEXTURE_H__

#include <cstdint>

// Decodes the top-left theWidth x theHeight texels of an OpenGX texture back
// into ARGB8888, straight from the GX texel buffer in main RAM. Stands in for
// the FBO readback GLInterface::RecoverBits uses elsewhere, which OpenGX
// can't do (its FBOs don't read texture contents back via glReadPixels).
// Handles the formats GLInterface uploads on Wii: RGBA8, RGB5A3, RGB565 and CMPR.
bool WiiReadTexturePixels(unsigned int theTexture, uint32_t* theDest, int theDestStride,
	int theWidth, int theHeight);

// [wii-debug] Bytes of GX texel memory behind an OpenGX texture (0 if none).
uint32_t WiiTextureBytes(unsigned int theTexture);

#endif // __OGXTEXTURE_H__
