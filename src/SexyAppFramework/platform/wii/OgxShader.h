#ifndef __OGXSHADER_H__
#define __OGXSHADER_H__

// Registers the OpenGX program processor that emulates GLInterface's GLSL
// shader with GX TEV stages. Must run before the first glCompileShader().
void WiiRegisterOgxShaderProcessor();

// Forces all GX state to be re-sent on the next draw. Call before drawing a
// frame: SDL's Wii backend draws the pointer with raw GX calls (scissor, Z,
// blend, TEV...) between frames, which opengx doesn't know about.
void WiiInvalidateGXState();

#endif // __OGXSHADER_H__
