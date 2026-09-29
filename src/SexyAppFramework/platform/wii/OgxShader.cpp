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

// OpenGX cannot compile GLSL: glCompileShader/glLinkProgram only succeed
// through a registered program processor that describes each shader's
// uniforms/attributes and emulates it with GX TEV stages. GLInterface.cpp
// has exactly one shader (vertex color, optionally modulated by a texture),
// which maps directly onto a single TEV stage.
//
// Kept in its own translation unit because opengx.h pulls in <GL/gl.h>,
// which conflicts with the glad GLES2 header used by the rest of the renderer.

#define GL_GLEXT_PROTOTYPES
#include <opengx.h>

#include "OgxShader.h"

// From the vendored, patched opengx (third_party/opengx); the portlibs
// opengx.h this file includes doesn't declare it.
extern "C" void ogx_invalidate_state(void);

namespace
{

struct ProgramLocations
{
	GLuint program = 0;
	GLint viewProj = -1;
	GLint useTexture = -1;
};

ProgramLocations gLocs;

void EnsureLocations(GLuint program)
{
	if (gLocs.program == program)
		return;
	gLocs.program = program;
	gLocs.viewProj = glGetUniformLocation(program, "u_viewProj");
	gLocs.useTexture = glGetUniformLocation(program, "u_useTexture");
}

bool CompileShader(GLuint shader)
{
	GLint type = 0;
	glGetShaderiv(shader, GL_SHADER_TYPE, &type);
	if (type == GL_VERTEX_SHADER)
	{
		ogx_shader_add_uniforms(shader, 1,
			"u_viewProj", GL_FLOAT_MAT4);
		ogx_shader_add_attributes(shader, 3,
			"a_position", GL_FLOAT_VEC3, GX_VA_POS,
			"a_color",    GL_FLOAT_VEC4, GX_VA_CLR0,
			"a_uv",       GL_FLOAT_VEC2, GX_VA_TEX0);
	}
	else
	{
		// u_uvBounds/u_clampUvEnabled are declared so glGetUniformLocation
		// and glUniform* on them stay valid, but the clamp itself isn't
		// emulated; GX texture wrap modes cover the common case.
		ogx_shader_add_uniforms(shader, 4,
			"u_texture",        GL_SAMPLER_2D,
			"u_useTexture",     GL_INT,
			"u_uvBounds",       GL_FLOAT_VEC4,
			"u_clampUvEnabled", GL_INT);
	}
	return true;
}

void SetupMatrices(GLuint program, void* /*user_data*/)
{
	EnsureLocations(program);
	GLfloat m[16];
	glGetUniformfv(program, gLocs.viewProj, m);
	ogx_shader_set_mvp_gl(m);
}

void SetupDraw(GLuint program, const OgxDrawData* /*draw_data*/, void* /*user_data*/)
{
	EnsureLocations(program);
	GLint useTexture = 0;
	glGetUniformiv(program, gLocs.useTexture, &useTexture);

	// Rasterized color comes straight from the per-vertex color, no lighting.
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	u8 stage = GX_TEVSTAGE0 + ogx_gpu_resources->tevstage_first++;
	// SDL's cursor code can leave a channel-swapping mode on the TEV stages.
	GX_SetTevSwapMode(stage, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GXTexObj* texObj = useTexture ? ogx_shader_get_texobj(0) : nullptr;
	if (texObj)
	{
		u8 texCoord = GX_TEXCOORD0 + ogx_gpu_resources->texcoord_first++;
		u8 texMap = GX_TEXMAP0 + ogx_gpu_resources->texmap_first++;
		GX_LoadTexObj(texObj, texMap);
		GX_SetTexCoordGen(texCoord, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
		GX_SetNumTexGens(ogx_gpu_resources->texcoord_first);
		GX_SetTevOrder(stage, texCoord, texMap, GX_COLOR0A0);
		GX_SetTevOp(stage, GX_MODULATE);
	}
	else
	{
		GX_SetNumTexGens(ogx_gpu_resources->texcoord_first);
		GX_SetTevOrder(stage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevOp(stage, GX_PASSCLR);
	}
	GX_SetNumTevStages(ogx_gpu_resources->tevstage_first);
}

GLenum LinkProgram(GLuint program)
{
	ogx_shader_program_set_setup_matrices_cb(program, SetupMatrices);
	ogx_shader_program_set_setup_draw_cb(program, SetupDraw);
	return GL_NO_ERROR;
}

const OgxProgramProcessor gProcessor = {
	CompileShader,
	nullptr,
	LinkProgram,
};

} // namespace

void WiiInvalidateGXState()
{
	ogx_invalidate_state();
}

void WiiRegisterOgxShaderProcessor()
{
	ogx_shader_register_program_processor(&gProcessor);
}
