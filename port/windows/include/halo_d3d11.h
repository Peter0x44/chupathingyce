/*
HALO_D3D11.H

The Direct3D 11 device's two halves (HALO_D3D11: ninja windows-d3d11 and the
Windows RT build): the Xbox Direct3D device the game calls
(port/windows/src/d3d8_d3d11.c), which sees the Xbox SDK's declarations, and
what draws with Direct3D 11 (port/windows/src/win32_d3d11.c), which sees the
Windows SDK's. The two define the same names differently, so this interface
between them has plain types only. Its enumerants are OpenGL's (the Xbox's
render states hold them: GL_LEQUAL, GL_SRC_ALPHA, GL_INCR...).

Everything runs at feature level 9_3, the GPUs of Windows RT's devices
(Tegra 3 and 4, Adreno 225), unless display.d3d11_feature_level asks for
another: the textures and render targets its limits allow, shaders
compiled for shader model 4 at 9_3 (nv2a_vsh.c, nv2a_psh.c, XGPU_HLSL_PRELUDE).
*/

#ifndef __HALO_D3D11_H
#define __HALO_D3D11_H

/* ---------- the device */

/* the device and its swap chain for an SDL window (SDL_Window *), at
feature_level ("9_3", "9_1", "10_0", "11_0", or "best"), on WARP (Direct3D's
software renderer) if warp; FALSE (logged) if it cannot be made */
int win32_d3d11_initialize(void *window, const char *feature_level, int warp);
/* the largest texture or target either way */
int win32_d3d11_maximum_texture_size(void);

/* presents the back buffer, color target 'target' (a render target's id)
letterboxed into the window's x, y, width, height of window_width by
window_height pixels; vsync waits for the display */
void win32_d3d11_present(unsigned int target, int x, int y, int width, int height, int window_width,
	int window_height, int vsync);

/* ---------- render targets

A color target is B8G8R8A8 and can be sampled; a depth target is D24S8 and
cannot (9_3 samples no depth). Ids are never 0. */

unsigned int win32_d3d11_target_create(unsigned int width, unsigned int height, int depth);

/* a color target's pixels, B, G, R, A, its rows from the top, width by
height (its size); FALSE if they cannot be read (debug.screenshot_directory) */
int win32_d3d11_target_read(unsigned int target, void *pixels, unsigned int width, unsigned int height);

/* the levels' color targets (levels[0] the largest, each half the one
before) copied into a mipmapped texture of their own, kept by key (the
Xbox texture's address); the levels past count made from the last one.
Returns its id for win32_d3d11_draw's textures (kind 3). */
unsigned int win32_d3d11_target_mipmaps(unsigned long key, const unsigned int *levels, int count, int total_levels);

/* ---------- clears

The rectangle in the bound targets' pixels; color only in the channels of
color_mask (bit 0 red, 1 green, 2 blue, 3 alpha) */
void win32_d3d11_clear(unsigned int color_target, unsigned int depth_target, int left, int top, int right, int bottom,
	int clear_color, unsigned int color_mask, const float color[4], int clear_depth, float depth, int clear_stencil,
	unsigned int stencil);

/* ---------- shaders

Compiled from HLSL (shader model 4 at the device's feature level) and kept
by their text: the same text gives the same id. 0 if it does not compile
(logged). A vertex shader's input is input_mask's registers, each four
floats in register order, at TEXCOORD<register> (nv2a_vertex_shader_to_hlsl). */

unsigned int win32_d3d11_vertex_shader(const char *hlsl, unsigned long input_mask);
unsigned int win32_d3d11_pixel_shader(const char *hlsl);

/* ---------- draws */

enum
{
	_win32_d3d11_points,
	_win32_d3d11_lines,
	_win32_d3d11_line_strip,
	_win32_d3d11_triangles,
	_win32_d3d11_triangle_strip,
};

/* a texture stage's texture: none, a GL name (the texture shim,
win32_d3d11_textures.c), a render target's id, or a target_mipmaps id */
enum
{
	_win32_d3d11_texture_none,
	_win32_d3d11_texture_gl,
	_win32_d3d11_texture_target,
	_win32_d3d11_texture_mipmaps,
};

struct win32_d3d11_sampler
{
	/* GL_NEAREST or GL_LINEAR; mip GL_NONE (0), GL_NEAREST or GL_LINEAR */
	unsigned int minification, magnification, mip;
	/* GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER */
	unsigned int address[3];
	float lod_bias, minimum_lod;
	unsigned int anisotropy;
	float border[4];
};

struct win32_d3d11_draw
{
	unsigned int vertex_shader, pixel_shader;
	/* their constant buffers (struct xgpu_hlsl_vertex_constants,
	xgpu_hlsl_pixel_constants), uploaded when their serials change */
	const void *vertex_constants;
	unsigned int vertex_constants_size;
	unsigned long vertex_constants_serial;
	const void *pixel_constants;
	unsigned int pixel_constants_size;
	unsigned long pixel_constants_serial;

	/* the targets (0: none) */
	unsigned int color_target, depth_target;
	/* in the targets' pixels */
	float viewport[4];
	float depth_range[2];
	int scissor[4];

	int texture_kind[4];
	unsigned int texture[4];
	struct win32_d3d11_sampler samplers[4];

	/* GL enumerants (0: the test or blend off) */
	unsigned int depth_function;
	int depth_write;
	unsigned int stencil_function;
	unsigned int stencil_reference, stencil_read_mask, stencil_write_mask;
	unsigned int stencil_fail, stencil_depth_fail, stencil_pass;
	unsigned int blend_source, blend_destination, blend_equation;
	float blend_factor[4];
	/* bit 0 red, 1 green, 2 blue, 3 alpha */
	unsigned int color_mask;
	/* GL_FRONT, GL_BACK or 0; the front winding counter-clockwise */
	unsigned int cull;
	int front_counter_clockwise;
	int wireframe;
	/* in the depth buffer's units, and per unit of the depth slope */
	float depth_bias, slope_scaled_depth_bias;

	/* the vertices, stride bytes each (the vertex shader's inputs, four
	floats each); indices (16-bit) or none */
	int topology;
	const void *vertices;
	unsigned int vertex_count, stride;
	const unsigned short *indices;
	unsigned int index_count;
	int base_vertex;
};

void win32_d3d11_draw(const struct win32_d3d11_draw *draw);

/* ---------- occlusion queries */

/* the samples of the draws between begin and end; a query's result is
asked for later without waiting: FALSE until the GPU has it */
void win32_d3d11_query_begin(void);
unsigned int win32_d3d11_query_end(void);
/* (a query whose result is read is free again) */
int win32_d3d11_query_result(unsigned int query, unsigned long *samples);
/* a query whose result is no longer wanted */
void win32_d3d11_query_release(unsigned int query);

/* ---------- the texture shim (win32_d3d11_textures.c)

The texture cache (xbox_textures.c), the high-res HUD and text and the
menus' art make and fill textures through OpenGL's calls; in this build
those are the shim's, which keep them as Direct3D 11 textures. */

/* what a GL texture is, for the pixel shader that samples it: its kind, 1
2D, 2 3D, 3 a cube map (xgpu.h's _xgpu_sampler_*), or 0 when it has no
pixels; the channels it is sampled from (nv2a_pixel_shader_key channels);
its mip levels */
int win32_d3d11_texture_describe(unsigned int name, int *channels, int *levels);
/* the shim's GL functions put in gl.h's pointers */
void win32_d3d11_textures_install(void);

#endif
