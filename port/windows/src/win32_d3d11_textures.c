/*
WIN32_D3D11_TEXTURES.C

The Direct3D 11 device's texture shim (port/windows/include/halo_d3d11.h):
the OpenGL texture calls of the texture cache (xbox_textures.c), the
high-res HUD and text (hud_hires.c, text_hires.c) and the menus' art
(menu_files.c), kept as Direct3D 11 textures, so that those files are the
same in every build.

A texture's levels are kept as they are given (glTexImage2D and the rest)
until a draw first samples it, when it is made, all at once, as Direct3D 11
makes textures; its pixels are then dropped. A later change of a level
(glTexSubImage2D: the text atlas as glyphs are added) goes to the texture;
a new level 0 starts the texture again. Mip levels are generated
(glGenerateMipmap) from the first one on the CPU. Feature level 9_3 cannot
have mip levels on a texture whose sides are not powers of two: such a
texture is made with its first level only.

GL's channel swizzles (GL_TEXTURE_SWIZZLE_*), which Direct3D 11 does not
have, are the pixel shader's (nv2a_pixel_shader_key channels).
*/

#ifdef HALO_D3D11

#include "win32_d3d11_internal.h"
#include "../include/halo_d3d11.h"

#include <stdlib.h>
#include <string.h>

/* gl.h's pointers are defined here (gl_functions.c defines them for GL) */
#define GL_FUNCTIONS_DEFINE 1
#include "gl.h"

#define GL_DEFINE_FUNCTION(name) __typeof__(&name) halo_##name;
GL_FUNCTIONS(GL_DEFINE_FUNCTION)
#undef GL_DEFINE_FUNCTION

#define MAXIMUM_LEVELS 16

enum
{
	_format_none,
	_format_bgra,
	_format_rgba,
	_format_dxt1,
	_format_dxt3,
	_format_dxt5,
};

struct level
{
	/* its pixels (NULL for a level given none, which starts black) */
	unsigned char *data;
	unsigned int width, height, depth;
	BOOL defined;
};

struct texture
{
	BOOL used;
	/* GL_TEXTURE_2D, GL_TEXTURE_3D or GL_TEXTURE_CUBE_MAP */
	GLenum target;
	int format;
	int maximum_level;
	GLint swizzle[4];
	struct level levels[6][MAXIMUM_LEVELS];
	ID3D11Resource *resource;
	ID3D11ShaderResourceView *view;
	/* its levels changed since it was made */
	BOOL dirty;
	int made_levels;
};

/* name - 1 */
static struct texture *textures;
static unsigned int texture_count;
/* the textures bound to GL_TEXTURE_2D, GL_TEXTURE_3D and GL_TEXTURE_CUBE_MAP */
static GLuint bound_names[3];

static int target_slot(GLenum target)
{
	if (target == GL_TEXTURE_3D)
		return 1;
	if (target == GL_TEXTURE_CUBE_MAP || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z))
		return 2;
	return 0;
}

static struct texture *texture_get(GLuint name)
{
	return name && name <= texture_count && textures[name - 1].used ? &textures[name - 1] : NULL;
}

/* the bound texture of an image target, and the face it names */
static struct texture *bound_texture(GLenum target, int *face)
{
	*face = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ?
		(int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
	return texture_get(bound_names[target_slot(target)]);
}

static void texture_release(struct texture *texture)
{
	if (texture->view)
		ID3D11ShaderResourceView_Release(texture->view);
	if (texture->resource)
		ID3D11Resource_Release(texture->resource);
	texture->view = NULL;
	texture->resource = NULL;
	texture->made_levels = 0;
}

static void levels_free(struct texture *texture)
{
	int face, level;

	for (face = 0; face < 6; face++)
	{
		for (level = 0; level < MAXIMUM_LEVELS; level++)
		{
			free(texture->levels[face][level].data);
			memset(&texture->levels[face][level], 0, sizeof(texture->levels[face][level]));
		}
	}
}

static BOOL format_compressed(int format)
{
	return format >= _format_dxt1;
}

/* bytes of a row of texels (of blocks: four rows), and of a level */
static unsigned int row_bytes(int format, unsigned int width)
{
	if (format_compressed(format))
		return ((width + 3) / 4) * (format == _format_dxt1 ? 8 : 16);
	return width * 4;
}

static unsigned int level_bytes(int format, unsigned int width, unsigned int height, unsigned int depth)
{
	unsigned int rows = format_compressed(format) ? (height + 3) / 4 : height;

	return row_bytes(format, width) * rows * depth;
}

/* a level given its pixels; level 0 at another size or format starts the
texture again */
static void level_define(GLenum target, GLint level, int format, GLsizei width, GLsizei height, GLsizei depth,
	const void *data)
{
	int face;
	struct texture *texture = bound_texture(target, &face);
	struct level *entry;
	unsigned int bytes;

	if (!texture || level < 0 || level >= MAXIMUM_LEVELS || width <= 0 || height <= 0 || depth <= 0)
		return;
	if (texture->target == 0)
		texture->target = target_slot(target) == 2 ? GL_TEXTURE_CUBE_MAP : target_slot(target) == 1 ? GL_TEXTURE_3D :
			GL_TEXTURE_2D;
	entry = &texture->levels[face][level];
	if (level == 0 && (texture->format != format || entry->width != (unsigned int)width ||
		entry->height != (unsigned int)height || entry->depth != (unsigned int)depth))
	{
		levels_free(texture);
		texture_release(texture);
		texture->format = format;
	}
	else if (texture->format != format)
	{
		return;
	}
	free(entry->data);
	entry->data = NULL;
	entry->width = (unsigned int)width;
	entry->height = (unsigned int)height;
	entry->depth = (unsigned int)depth;
	entry->defined = TRUE;
	bytes = level_bytes(format, entry->width, entry->height, entry->depth);
	if (data)
	{
		entry->data = malloc(bytes);
		if (entry->data)
			memcpy(entry->data, data, bytes);
	}
	texture->dirty = TRUE;
}

static int uncompressed_format(GLenum format)
{
	return format == GL_BGRA ? _format_bgra : _format_rgba;
}

static int compressed_format(GLenum internal_format)
{
	switch (internal_format)
	{
	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: return _format_dxt1;
	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: return _format_dxt3;
	default: return _format_dxt5;
	}
}

/* ---------- the GL calls */

static void APIENTRY shim_glGenTextures(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		unsigned int slot;

		for (slot = 0; slot < texture_count && textures[slot].used; slot++)
		{
		}
		if (slot == texture_count)
		{
			struct texture *grown = realloc(textures, (texture_count + 64) * sizeof(*textures));

			if (!grown)
			{
				names[index] = 0;
				continue;
			}
			textures = grown;
			memset(textures + texture_count, 0, 64 * sizeof(*textures));
			texture_count += 64;
		}
		memset(&textures[slot], 0, sizeof(textures[slot]));
		textures[slot].used = TRUE;
		textures[slot].maximum_level = 1000;
		textures[slot].swizzle[0] = GL_RED;
		textures[slot].swizzle[1] = GL_GREEN;
		textures[slot].swizzle[2] = GL_BLUE;
		textures[slot].swizzle[3] = GL_ALPHA;
		names[index] = slot + 1;
	}
}

static void APIENTRY shim_glDeleteTextures(GLsizei count, const GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		struct texture *texture = texture_get(names[index]);
		int slot;

		if (!texture)
			continue;
		texture_release(texture);
		levels_free(texture);
		texture->used = FALSE;
		for (slot = 0; slot < 3; slot++)
		{
			if (bound_names[slot] == names[index])
				bound_names[slot] = 0;
		}
	}
}

static void APIENTRY shim_glBindTexture(GLenum target, GLuint name)
{
	bound_names[target_slot(target)] = name;
}

static void APIENTRY shim_glTexParameteri(GLenum target, GLenum parameter, GLint value)
{
	struct texture *texture = texture_get(bound_names[target_slot(target)]);

	if (!texture)
		return;
	switch (parameter)
	{
	case GL_TEXTURE_MAX_LEVEL:
		/* (made again only for a new value: its pixels are gone once made) */
		if (texture->maximum_level != value)
		{
			texture->maximum_level = value;
			texture->dirty = TRUE;
		}
		break;
	case GL_TEXTURE_SWIZZLE_R: texture->swizzle[0] = value; break;
	case GL_TEXTURE_SWIZZLE_G: texture->swizzle[1] = value; break;
	case GL_TEXTURE_SWIZZLE_B: texture->swizzle[2] = value; break;
	case GL_TEXTURE_SWIZZLE_A: texture->swizzle[3] = value; break;
	default: break;
	}
}

static void APIENTRY shim_glPixelStorei(GLenum parameter, GLint value)
{
	/* (every upload here is tightly packed) */
	(void)parameter;
	(void)value;
}

static void APIENTRY shim_glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width,
	GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)
{
	(void)internal_format;
	(void)border;
	(void)type;
	level_define(target, level, uncompressed_format(format), width, height, 1, pixels);
}

static void APIENTRY shim_glTexImage3D(GLenum target, GLint level, GLint internal_format, GLsizei width,
	GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)
{
	(void)internal_format;
	(void)border;
	(void)type;
	level_define(target, level, uncompressed_format(format), width, height, depth, pixels);
}

static void APIENTRY shim_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal_format, GLsizei width,
	GLsizei height, GLint border, GLsizei size, const void *data)
{
	(void)border;
	(void)size;
	level_define(target, level, compressed_format(internal_format), width, height, 1, data);
}

static void APIENTRY shim_glCompressedTexImage3D(GLenum target, GLint level, GLenum internal_format, GLsizei width,
	GLsizei height, GLsizei depth, GLint border, GLsizei size, const void *data)
{
	(void)border;
	(void)size;
	level_define(target, level, compressed_format(internal_format), width, height, depth, data);
}

static void APIENTRY shim_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
	GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	int face;
	struct texture *texture = bound_texture(target, &face);
	struct level *entry;
	GLsizei row;

	(void)format;
	(void)type;
	if (!texture || level < 0 || level >= MAXIMUM_LEVELS || !pixels || format_compressed(texture->format))
		return;
	entry = &texture->levels[face][level];
	if (!entry->defined || x < 0 || y < 0 || (unsigned int)(x + width) > entry->width ||
		(unsigned int)(y + height) > entry->height)
		return;
	/* made already: into the texture */
	if (texture->resource && !texture->dirty && level < texture->made_levels)
	{
		D3D11_BOX box;

		box.left = (UINT)x;
		box.top = (UINT)y;
		box.front = 0;
		box.right = (UINT)(x + width);
		box.bottom = (UINT)(y + height);
		box.back = 1;
		ID3D11DeviceContext_UpdateSubresource(win32_d3d11_context, texture->resource,
			/* (D3D11CalcSubresource's, which C has not) */
			(UINT)level + (UINT)face * (UINT)texture->made_levels, &box, pixels, (UINT)width * 4, 0);
		return;
	}
	if (!entry->data)
	{
		entry->data = calloc(1, level_bytes(texture->format, entry->width, entry->height, 1));
		if (!entry->data)
			return;
	}
	for (row = 0; row < height; row++)
	{
		memcpy(entry->data + ((size_t)(y + row) * entry->width + (size_t)x) * 4,
			(const unsigned char *)pixels + (size_t)row * (size_t)width * 4, (size_t)width * 4);
	}
	texture->dirty = TRUE;
}

/* each level from the one before, each texel the average of four */
static void APIENTRY shim_glGenerateMipmap(GLenum target)
{
	int face, level;
	struct texture *texture = bound_texture(target, &face);

	if (!texture || format_compressed(texture->format) || texture->target == GL_TEXTURE_3D)
		return;
	for (face = 0; face < (texture->target == GL_TEXTURE_CUBE_MAP ? 6 : 1); face++)
	{
		for (level = 1; level < MAXIMUM_LEVELS && level <= texture->maximum_level; level++)
		{
			struct level *above = &texture->levels[face][level - 1], *entry = &texture->levels[face][level];
			unsigned int x, y, channel;

			if (!above->defined || !above->data || (above->width == 1 && above->height == 1))
				break;
			free(entry->data);
			entry->width = above->width > 1 ? above->width / 2 : 1;
			entry->height = above->height > 1 ? above->height / 2 : 1;
			entry->depth = 1;
			entry->defined = TRUE;
			entry->data = malloc((size_t)entry->width * entry->height * 4);
			if (!entry->data)
				break;
			for (y = 0; y < entry->height; y++)
			{
				unsigned int y0 = y * 2 < above->height ? y * 2 : above->height - 1;
				unsigned int y1 = y * 2 + 1 < above->height ? y * 2 + 1 : y0;

				for (x = 0; x < entry->width; x++)
				{
					unsigned int x0 = x * 2 < above->width ? x * 2 : above->width - 1;
					unsigned int x1 = x * 2 + 1 < above->width ? x * 2 + 1 : x0;

					for (channel = 0; channel < 4; channel++)
					{
						unsigned int sum = above->data[((size_t)y0 * above->width + x0) * 4 + channel] +
							above->data[((size_t)y0 * above->width + x1) * 4 + channel] +
							above->data[((size_t)y1 * above->width + x0) * 4 + channel] +
							above->data[((size_t)y1 * above->width + x1) * 4 + channel];

						entry->data[((size_t)y * entry->width + x) * 4 + channel] = (unsigned char)((sum + 2) / 4);
					}
				}
			}
		}
	}
	texture->dirty = TRUE;
}

static void APIENTRY shim_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void *pixels)
{
	/* (debug.texture_dump_directory's read back: the pixels are not kept) */
	(void)target;
	(void)level;
	(void)format;
	(void)type;
	(void)pixels;
}

void win32_d3d11_textures_install(void)
{
	halo_glGenTextures = shim_glGenTextures;
	halo_glDeleteTextures = shim_glDeleteTextures;
	halo_glBindTexture = shim_glBindTexture;
	halo_glTexParameteri = shim_glTexParameteri;
	halo_glPixelStorei = shim_glPixelStorei;
	halo_glTexImage2D = shim_glTexImage2D;
	halo_glTexImage3D = shim_glTexImage3D;
	halo_glCompressedTexImage2D = shim_glCompressedTexImage2D;
	halo_glCompressedTexImage3D = shim_glCompressedTexImage3D;
	halo_glTexSubImage2D = shim_glTexSubImage2D;
	halo_glGenerateMipmap = shim_glGenerateMipmap;
	halo_glGetTexImage = shim_glGetTexImage;
}

/* ---------- the textures, as the device draws with them */

static DXGI_FORMAT dxgi_format(int format)
{
	switch (format)
	{
	case _format_bgra: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case _format_dxt1: return DXGI_FORMAT_BC1_UNORM;
	case _format_dxt3: return DXGI_FORMAT_BC2_UNORM;
	case _format_dxt5: return DXGI_FORMAT_BC3_UNORM;
	default: return DXGI_FORMAT_R8G8B8A8_UNORM;
	}
}

/* the levels from the first that every face has, in a row, up to the
maximum level */
static int texture_levels(const struct texture *texture)
{
	int faces = texture->target == GL_TEXTURE_CUBE_MAP ? 6 : 1, face, level;

	for (level = 0; level < MAXIMUM_LEVELS && level <= texture->maximum_level; level++)
	{
		for (face = 0; face < faces; face++)
		{
			if (!texture->levels[face][level].defined)
				return level;
		}
	}
	return level;
}

static BOOL texture_make(struct texture *texture, int levels)
{
	D3D11_SUBRESOURCE_DATA initial[6 * MAXIMUM_LEVELS];
	unsigned char *zeros = NULL;
	int faces = texture->target == GL_TEXTURE_CUBE_MAP ? 6 : 1, face, level;
	const struct level *top = &texture->levels[0][0];
	size_t largest = level_bytes(texture->format, top->width, top->height, top->depth);
	HRESULT result;

	for (face = 0; face < faces; face++)
	{
		for (level = 0; level < levels; level++)
		{
			const struct level *entry = &texture->levels[face][level];
			D3D11_SUBRESOURCE_DATA *data = &initial[face * levels + level];
			unsigned int rows = format_compressed(texture->format) ? (entry->height + 3) / 4 : entry->height;

			if (!entry->data && !zeros)
			{
				zeros = calloc(1, largest);
				if (!zeros)
					return FALSE;
			}
			data->pSysMem = entry->data ? entry->data : zeros;
			data->SysMemPitch = row_bytes(texture->format, entry->width);
			data->SysMemSlicePitch = data->SysMemPitch * rows;
		}
	}
	if (texture->target == GL_TEXTURE_3D)
	{
		D3D11_TEXTURE3D_DESC description;
		ID3D11Texture3D *created = NULL;

		memset(&description, 0, sizeof(description));
		description.Width = top->width;
		description.Height = top->height;
		description.Depth = top->depth;
		description.MipLevels = (UINT)levels;
		description.Format = dxgi_format(texture->format);
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		result = ID3D11Device_CreateTexture3D(win32_d3d11_device, &description, initial, &created);
		texture->resource = (ID3D11Resource *)created;
	}
	else
	{
		D3D11_TEXTURE2D_DESC description;
		ID3D11Texture2D *created = NULL;

		memset(&description, 0, sizeof(description));
		description.Width = top->width;
		description.Height = top->height;
		description.MipLevels = (UINT)levels;
		description.ArraySize = (UINT)faces;
		description.Format = dxgi_format(texture->format);
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		description.MiscFlags = faces == 6 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
		result = ID3D11Device_CreateTexture2D(win32_d3d11_device, &description, initial, &created);
		texture->resource = (ID3D11Resource *)created;
	}
	free(zeros);
	if (FAILED(result))
	{
		texture->resource = NULL;
		return FALSE;
	}
	if (FAILED(ID3D11Device_CreateShaderResourceView(win32_d3d11_device, texture->resource, NULL, &texture->view)))
	{
		texture_release(texture);
		return FALSE;
	}
	texture->made_levels = levels;
	return TRUE;
}

ID3D11ShaderResourceView *win32_d3d11_texture_view(unsigned int name)
{
	struct texture *texture = texture_get(name);
	int levels;

	if (!texture || !win32_d3d11_device)
		return NULL;
	if (texture->view && !texture->dirty)
		return texture->view;
	texture_release(texture);
	texture->dirty = FALSE;
	levels = texture_levels(texture);
	if (!levels)
		return NULL;
	/* (a side not a power of two: 9_3 has no mip levels for it) */
	if (!texture_make(texture, levels) && (levels == 1 || !texture_make(texture, 1)))
	{
		static unsigned long failures;

		if (failures++ < 20)
			platform_log("Direct3D 11: cannot make a %ux%ux%u texture (format %d, %d levels)",
				texture->levels[0][0].width, texture->levels[0][0].height, texture->levels[0][0].depth,
				texture->format, levels);
		return NULL;
	}
	/* (the pixels are the texture's now; a new level 0 brings new ones) */
	{
		int face, level;

		for (face = 0; face < 6; face++)
		{
			for (level = 0; level < MAXIMUM_LEVELS; level++)
			{
				free(texture->levels[face][level].data);
				texture->levels[face][level].data = NULL;
			}
		}
	}
	return texture->view;
}

int win32_d3d11_texture_describe(unsigned int name, int *channels, int *levels)
{
	struct texture *texture = texture_get(name);
	const GLint *swizzle;

	*channels = 0;
	*levels = 0;
	if (!texture || !texture->levels[0][0].defined)
		return 0;
	swizzle = texture->swizzle;
	/* a Halo PC HUD meter's (alpha, alpha, alpha, red) and a multipurpose
	map's (blue, green, alpha, red): xbox_textures.c upload */
	if (swizzle[0] == GL_ALPHA && swizzle[1] == GL_ALPHA && swizzle[2] == GL_ALPHA && swizzle[3] == GL_RED)
		*channels = 1;
	else if (swizzle[0] == GL_BLUE && swizzle[1] == GL_GREEN && swizzle[2] == GL_ALPHA && swizzle[3] == GL_RED)
		*channels = 2;
	*levels = texture->made_levels && !texture->dirty ? texture->made_levels : texture_levels(texture);
	return texture->target == GL_TEXTURE_3D ? 2 : texture->target == GL_TEXTURE_CUBE_MAP ? 3 : 1;
}

#endif
