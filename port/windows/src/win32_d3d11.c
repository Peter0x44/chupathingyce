/*
WIN32_D3D11.C

The Direct3D 11 device's Windows half (port/windows/include/halo_d3d11.h):
the device and its swap chain, render targets, clears, shaders compiled
from the translators' HLSL, the draws' state objects and buffers, and
occlusion queries. The Xbox Direct3D device the game calls is
d3d8_d3d11.c; the textures, the texture shim's (win32_d3d11_textures.c).

It is written for feature level 9_3, the GPUs of Windows RT's devices:
- no vertex IDs, so the clears' and the presentation's quads have vertex
  buffers;
- a render target that a draw also samples is copied first (9_3 has no
  copy-free feedback, and Direct3D 11 unbinds a view that is both);
- non-power-of-two textures are drawn without their mip levels where the
  feature level cannot have them;
- the shader compiler (d3dcompiler_47.dll, in Windows 8.1 and later) is
  loaded when the device is made, and its shaders kept by their text.
*/

#ifdef HALO_D3D11

#include "win32_d3d11_internal.h"
#include "../include/halo_d3d11.h"

#include <d3dcompiler.h>
#include <dxgi.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

ID3D11Device *win32_d3d11_device;
ID3D11DeviceContext *win32_d3d11_context;
D3D_FEATURE_LEVEL win32_d3d11_feature_level;

static IDXGISwapChain *swap_chain;
static ID3D11RenderTargetView *back_buffer_view;
static UINT swap_width, swap_height;
static pD3DCompile compile_function;
static const char *vertex_profile, *pixel_profile;

/* ---------- small helpers */

static unsigned long hash_bytes(const void *data, size_t size)
{
	const unsigned char *bytes = data;
	unsigned long hash = 2166136261UL;

	while (size--)
		hash = (hash ^ *bytes++) * 16777619UL;
	return hash;
}

#define RELEASE(object) do { if (object) { IUnknown_Release((IUnknown *)(object)); (object) = NULL; } } while (0)

/* ---------- shaders */

struct shader_entry
{
	struct shader_entry *next;
	unsigned long hash;
	char *text;
	BOOL pixel;
	unsigned long input_mask;
	ID3D11VertexShader *vertex_shader;
	ID3D11PixelShader *pixel_shader;
	ID3D11InputLayout *layout;
};

#define SHADER_BUCKETS 1024
static struct shader_entry *shader_buckets[SHADER_BUCKETS];
/* id - 1 */
static struct shader_entry **shaders;
static unsigned int shader_count, shader_capacity;

/* compiled for the device's feature level, optimized, or (where the
optimizer folds a division by zero, or fails inside) without
optimization; NULL with the errors logged */
static ID3DBlob *compile(const char *text, const char *profile, const char *what)
{
	static unsigned long failures;
	ID3DBlob *code = NULL, *errors = NULL;
	HRESULT result;

	if (!compile_function)
		return NULL;
	result = compile_function(text, strlen(text), what, NULL, NULL, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
		&code, &errors);
	if (FAILED(result))
	{
		RELEASE(errors);
		RELEASE(code);
		result = compile_function(text, strlen(text), what, NULL, NULL, "main", profile, D3DCOMPILE_SKIP_OPTIMIZATION, 0,
			&code, &errors);
	}
	if (FAILED(result))
	{
		if (failures++ < 20)
		{
			platform_log("Direct3D 11: cannot compile the %s shader: %s\n%s", what,
				errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "(no message)", text);
		}
		RELEASE(errors);
		RELEASE(code);
		return NULL;
	}
	RELEASE(errors);
	return code;
}

static unsigned int shader_get(const char *text, BOOL pixel, unsigned long input_mask)
{
	unsigned long hash = hash_bytes(text, strlen(text)) ^ (pixel ? 0x9e3779b9UL : input_mask * 2654435761UL);
	struct shader_entry **bucket = &shader_buckets[hash % SHADER_BUCKETS], *entry;
	unsigned int index;
	ID3DBlob *code;
	HRESULT result;

	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && entry->pixel == pixel && entry->input_mask == input_mask && !strcmp(entry->text, text))
			break;
	}
	if (!entry)
	{
		entry = calloc(1, sizeof(*entry));
		entry->hash = hash;
		entry->text = _strdup(text);
		entry->pixel = pixel;
		entry->input_mask = input_mask;
		entry->next = *bucket;
		*bucket = entry;
		code = compile(text, pixel ? pixel_profile : vertex_profile, pixel ? "pixel" : "vertex");
		if (code && pixel)
		{
			result = ID3D11Device_CreatePixelShader(win32_d3d11_device, ID3D10Blob_GetBufferPointer(code),
				ID3D10Blob_GetBufferSize(code), NULL, &entry->pixel_shader);
			if (FAILED(result))
				platform_log("Direct3D 11: CreatePixelShader failed (%08lx)", (unsigned long)result);
		}
		else if (code)
		{
			D3D11_INPUT_ELEMENT_DESC elements[16];
			UINT count = 0, reg;

			result = ID3D11Device_CreateVertexShader(win32_d3d11_device, ID3D10Blob_GetBufferPointer(code),
				ID3D10Blob_GetBufferSize(code), NULL, &entry->vertex_shader);
			if (FAILED(result))
				platform_log("Direct3D 11: CreateVertexShader failed (%08lx)", (unsigned long)result);
			/* each input register four floats, one after another */
			for (reg = 0; reg < 16; reg++)
			{
				if (!(input_mask & (1UL << reg)))
					continue;
				elements[count].SemanticName = "TEXCOORD";
				elements[count].SemanticIndex = reg;
				elements[count].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
				elements[count].InputSlot = 0;
				elements[count].AlignedByteOffset = count * 16;
				elements[count].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
				elements[count].InstanceDataStepRate = 0;
				count++;
			}
			if (count && SUCCEEDED(result))
			{
				result = ID3D11Device_CreateInputLayout(win32_d3d11_device, elements, count,
					ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), &entry->layout);
				if (FAILED(result))
					platform_log("Direct3D 11: CreateInputLayout failed (%08lx)", (unsigned long)result);
			}
		}
		RELEASE(code);
		if (shader_count == shader_capacity)
		{
			shader_capacity = shader_capacity ? shader_capacity * 2 : 256;
			shaders = realloc(shaders, shader_capacity * sizeof(*shaders));
		}
		shaders[shader_count++] = entry;
	}
	if (pixel ? !entry->pixel_shader : !entry->vertex_shader)
		return 0;
	for (index = shader_count; index > 0; index--)
	{
		if (shaders[index - 1] == entry)
			return index;
	}
	return 0;
}

unsigned int win32_d3d11_vertex_shader(const char *hlsl, unsigned long input_mask)
{
	return shader_get(hlsl, FALSE, input_mask);
}

unsigned int win32_d3d11_pixel_shader(const char *hlsl)
{
	return shader_get(hlsl, TRUE, 0);
}

/* ---------- state objects, made once for each description */

struct state_entry
{
	struct state_entry *next;
	unsigned long hash;
	size_t size;
	IUnknown *object;
	/* (size bytes: a blend state's description, the largest, has eight
	targets' blends) */
	unsigned char description[];
};

#define STATE_BUCKETS 256

struct state_cache
{
	struct state_entry *buckets[STATE_BUCKETS];
};

static struct state_cache blend_states, depth_states, raster_states, sampler_states;

static IUnknown *state_find(struct state_cache *cache, const void *description, size_t size, unsigned long *hash)
{
	struct state_entry *entry;

	*hash = hash_bytes(description, size);
	for (entry = cache->buckets[*hash % STATE_BUCKETS]; entry; entry = entry->next)
	{
		if (entry->hash == *hash && entry->size == size && !memcmp(entry->description, description, size))
			return entry->object;
	}
	return NULL;
}

static void state_add(struct state_cache *cache, const void *description, size_t size, unsigned long hash, IUnknown *object)
{
	struct state_entry *entry = calloc(1, sizeof(*entry) + size);

	if (!entry)
		return;
	entry->hash = hash;
	entry->size = size;
	memcpy(entry->description, description, size);
	entry->object = object;
	entry->next = cache->buckets[hash % STATE_BUCKETS];
	cache->buckets[hash % STATE_BUCKETS] = entry;
}

static ID3D11BlendState *blend_state(const D3D11_BLEND_DESC *description)
{
	unsigned long hash;
	IUnknown *object = state_find(&blend_states, description, sizeof(*description), &hash);
	ID3D11BlendState *state = NULL;

	if (object)
		return (ID3D11BlendState *)object;
	if (FAILED(ID3D11Device_CreateBlendState(win32_d3d11_device, description, &state)))
		platform_log("Direct3D 11: cannot make a blend state (%d %d %d)", (int)description->RenderTarget[0].SrcBlend,
			(int)description->RenderTarget[0].DestBlend, (int)description->RenderTarget[0].BlendOp);
	state_add(&blend_states, description, sizeof(*description), hash, (IUnknown *)state);
	return state;
}

static ID3D11DepthStencilState *depth_state(const D3D11_DEPTH_STENCIL_DESC *description)
{
	unsigned long hash;
	IUnknown *object = state_find(&depth_states, description, sizeof(*description), &hash);
	ID3D11DepthStencilState *state = NULL;

	if (object)
		return (ID3D11DepthStencilState *)object;
	if (FAILED(ID3D11Device_CreateDepthStencilState(win32_d3d11_device, description, &state)))
		platform_log("Direct3D 11: cannot make a depth stencil state");
	state_add(&depth_states, description, sizeof(*description), hash, (IUnknown *)state);
	return state;
}

static ID3D11RasterizerState *raster_state(const D3D11_RASTERIZER_DESC *description)
{
	unsigned long hash;
	IUnknown *object = state_find(&raster_states, description, sizeof(*description), &hash);
	ID3D11RasterizerState *state = NULL;

	if (object)
		return (ID3D11RasterizerState *)object;
	if (FAILED(ID3D11Device_CreateRasterizerState(win32_d3d11_device, description, &state)))
		platform_log("Direct3D 11: cannot make a rasterizer state");
	state_add(&raster_states, description, sizeof(*description), hash, (IUnknown *)state);
	return state;
}

/* (9_1 has no border addressing, and 9_x may refuse a sampler's LOD range:
each refusal tried again without it) */
static ID3D11SamplerState *sampler_state(const D3D11_SAMPLER_DESC *description)
{
	unsigned long hash;
	IUnknown *object = state_find(&sampler_states, description, sizeof(*description), &hash);
	ID3D11SamplerState *state = NULL;

	if (object)
		return (ID3D11SamplerState *)object;
	if (FAILED(ID3D11Device_CreateSamplerState(win32_d3d11_device, description, &state)))
	{
		D3D11_SAMPLER_DESC fallback = *description;

		if (fallback.AddressU == D3D11_TEXTURE_ADDRESS_BORDER)
			fallback.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		if (fallback.AddressV == D3D11_TEXTURE_ADDRESS_BORDER)
			fallback.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		if (fallback.AddressW == D3D11_TEXTURE_ADDRESS_BORDER)
			fallback.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		if (FAILED(ID3D11Device_CreateSamplerState(win32_d3d11_device, &fallback, &state)))
		{
			fallback.MinLOD = 0.0f;
			fallback.MaxLOD = D3D11_FLOAT32_MAX;
			if (fallback.MaxAnisotropy > 2)
				fallback.MaxAnisotropy = 2;
			if (FAILED(ID3D11Device_CreateSamplerState(win32_d3d11_device, &fallback, &state)))
				platform_log("Direct3D 11: cannot make a sampler state");
		}
	}
	state_add(&sampler_states, description, sizeof(*description), hash, (IUnknown *)state);
	return state;
}

/* ---------- render targets */

struct target
{
	ID3D11Texture2D *texture;
	ID3D11RenderTargetView *color_view;
	ID3D11DepthStencilView *depth_view;
	ID3D11ShaderResourceView *view;
	/* the copy a draw samples while it draws into the target */
	ID3D11Texture2D *copy;
	ID3D11ShaderResourceView *copy_view;
	unsigned int width, height;
	BOOL depth;
};

static struct target *targets;
static unsigned int target_count, target_capacity;

static struct target *target_get(unsigned int id)
{
	return id && id <= target_count ? &targets[id - 1] : NULL;
}

unsigned int win32_d3d11_target_create(unsigned int width, unsigned int height, int depth)
{
	D3D11_TEXTURE2D_DESC description;
	struct target *target;
	HRESULT result;

	if (!win32_d3d11_device)
		return 0;
	if (target_count == target_capacity)
	{
		target_capacity = target_capacity ? target_capacity * 2 : 64;
		targets = realloc(targets, target_capacity * sizeof(*targets));
	}
	target = &targets[target_count];
	memset(target, 0, sizeof(*target));
	target->width = width ? width : 1;
	target->height = height ? height : 1;
	target->depth = depth != 0;
	memset(&description, 0, sizeof(description));
	description.Width = target->width;
	description.Height = target->height;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = depth ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_B8G8R8A8_UNORM;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	result = ID3D11Device_CreateTexture2D(win32_d3d11_device, &description, NULL, &target->texture);
	if (FAILED(result))
	{
		platform_log("Direct3D 11: cannot make a %ux%u %s target (%08lx)", target->width, target->height,
			depth ? "depth" : "color", (unsigned long)result);
		return 0;
	}
	if (depth)
	{
		result = ID3D11Device_CreateDepthStencilView(win32_d3d11_device, (ID3D11Resource *)target->texture, NULL,
			&target->depth_view);
	}
	else
	{
		result = ID3D11Device_CreateRenderTargetView(win32_d3d11_device, (ID3D11Resource *)target->texture, NULL,
			&target->color_view);
		if (SUCCEEDED(result))
			result = ID3D11Device_CreateShaderResourceView(win32_d3d11_device, (ID3D11Resource *)target->texture, NULL,
				&target->view);
	}
	if (FAILED(result))
	{
		platform_log("Direct3D 11: cannot make a target's views (%08lx)", (unsigned long)result);
		RELEASE(target->texture);
		RELEASE(target->color_view);
		RELEASE(target->depth_view);
		RELEASE(target->view);
		return 0;
	}
	/* (cleared, as GL's new textures read: the first frame shows black) */
	if (depth)
		ID3D11DeviceContext_ClearDepthStencilView(win32_d3d11_context, target->depth_view,
			D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
	else
	{
		static const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		ID3D11DeviceContext_ClearRenderTargetView(win32_d3d11_context, target->color_view, black);
	}
	return ++target_count;
}

int win32_d3d11_target_read(unsigned int id, void *pixels, unsigned int width, unsigned int height)
{
	struct target *target = target_get(id);
	D3D11_TEXTURE2D_DESC description;
	D3D11_MAPPED_SUBRESOURCE mapped;
	ID3D11Texture2D *staging = NULL;
	unsigned int row;

	if (!target || target->depth || target->width != width || target->height != height)
		return FALSE;
	ID3D11Texture2D_GetDesc(target->texture, &description);
	description.Usage = D3D11_USAGE_STAGING;
	description.BindFlags = 0;
	description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(ID3D11Device_CreateTexture2D(win32_d3d11_device, &description, NULL, &staging)))
		return FALSE;
	ID3D11DeviceContext_CopyResource(win32_d3d11_context, (ID3D11Resource *)staging, (ID3D11Resource *)target->texture);
	if (FAILED(ID3D11DeviceContext_Map(win32_d3d11_context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)))
	{
		RELEASE(staging);
		return FALSE;
	}
	for (row = 0; row < height; row++)
		memcpy((unsigned char *)pixels + (size_t)row * width * 4, (unsigned char *)mapped.pData + (size_t)row * mapped.RowPitch,
			(size_t)width * 4);
	ID3D11DeviceContext_Unmap(win32_d3d11_context, (ID3D11Resource *)staging, 0);
	RELEASE(staging);
	return TRUE;
}

/* the view of a target that is also bound for drawing: its pixels copied
into a texture of their own */
static ID3D11ShaderResourceView *target_copy_view(struct target *target)
{
	if (!target->copy)
	{
		D3D11_TEXTURE2D_DESC description;

		ID3D11Texture2D_GetDesc(target->texture, &description);
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(ID3D11Device_CreateTexture2D(win32_d3d11_device, &description, NULL, &target->copy)) ||
			FAILED(ID3D11Device_CreateShaderResourceView(win32_d3d11_device, (ID3D11Resource *)target->copy, NULL,
			&target->copy_view)))
		{
			RELEASE(target->copy);
			return NULL;
		}
	}
	ID3D11DeviceContext_CopyResource(win32_d3d11_context, (ID3D11Resource *)target->copy,
		(ID3D11Resource *)target->texture);
	return target->copy_view;
}

/* ---------- render targets' mip levels in one texture */

struct mipmaps
{
	struct mipmaps *next;
	unsigned long key;
	unsigned int width, height, levels;
	ID3D11Texture2D *texture;
	ID3D11ShaderResourceView *view;
	BOOL generate;
};

static struct mipmaps *mipmaps_list;
/* id - 1 */
static struct mipmaps **mipmaps_ids;
static unsigned int mipmaps_count;

unsigned int win32_d3d11_target_mipmaps(unsigned long key, const unsigned int *levels, int count, int total_levels)
{
	struct target *top = count > 0 ? target_get(levels[0]) : NULL;
	struct mipmaps *mipmaps;
	unsigned int id;
	int level;

	if (!top || top->depth)
		return 0;
	for (mipmaps = mipmaps_list; mipmaps; mipmaps = mipmaps->next)
	{
		if (mipmaps->key == key && mipmaps->width == top->width && mipmaps->height == top->height &&
			mipmaps->levels == (unsigned int)total_levels)
			break;
	}
	if (!mipmaps)
	{
		D3D11_TEXTURE2D_DESC description;

		mipmaps = calloc(1, sizeof(*mipmaps));
		mipmaps->key = key;
		mipmaps->width = top->width;
		mipmaps->height = top->height;
		mipmaps->levels = (unsigned int)total_levels;
		memset(&description, 0, sizeof(description));
		description.Width = top->width;
		description.Height = top->height;
		description.MipLevels = (UINT)total_levels;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
		mipmaps->generate = TRUE;
		if (FAILED(ID3D11Device_CreateTexture2D(win32_d3d11_device, &description, NULL, &mipmaps->texture)))
		{
			/* (without generated levels: those the game did not draw stay
			black) */
			description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			description.MiscFlags = 0;
			mipmaps->generate = FALSE;
			if (FAILED(ID3D11Device_CreateTexture2D(win32_d3d11_device, &description, NULL, &mipmaps->texture)))
			{
				free(mipmaps);
				return 0;
			}
		}
		ID3D11Device_CreateShaderResourceView(win32_d3d11_device, (ID3D11Resource *)mipmaps->texture, NULL,
			&mipmaps->view);
		mipmaps->next = mipmaps_list;
		mipmaps_list = mipmaps;
		mipmaps_ids = realloc(mipmaps_ids, (mipmaps_count + 1) * sizeof(*mipmaps_ids));
		mipmaps_ids[mipmaps_count++] = mipmaps;
	}
	for (level = 0; level < count && level < total_levels; level++)
	{
		struct target *target = target_get(levels[level]);

		if (!target || target->depth)
			break;
		ID3D11DeviceContext_CopySubresourceRegion(win32_d3d11_context, (ID3D11Resource *)mipmaps->texture,
			(UINT)level, 0, 0, 0, (ID3D11Resource *)target->texture, 0, NULL);
	}
	/* the levels the game did not draw, from the last one it did */
	if (level < total_levels && level > 0 && mipmaps->generate)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC description;
		ID3D11ShaderResourceView *view = NULL;

		memset(&description, 0, sizeof(description));
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		description.Texture2D.MostDetailedMip = (UINT)(level - 1);
		description.Texture2D.MipLevels = (UINT)(total_levels - level + 1);
		if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(win32_d3d11_device, (ID3D11Resource *)mipmaps->texture,
			&description, &view)))
		{
			ID3D11DeviceContext_GenerateMips(win32_d3d11_context, view);
			RELEASE(view);
		}
	}
	for (id = 0; id < mipmaps_count; id++)
	{
		if (mipmaps_ids[id] == mipmaps)
			return id + 1;
	}
	return 0;
}

/* ---------- buffers */

#define VERTEX_BUFFER_SIZE (16 * 1024 * 1024)
#define INDEX_BUFFER_SIZE (4 * 1024 * 1024)

struct stream_buffer
{
	ID3D11Buffer *buffer;
	UINT size, offset;
};

static struct stream_buffer vertex_stream, index_stream;
static ID3D11Buffer *vertex_constant_buffer, *pixel_constant_buffer;
static unsigned long vertex_constants_serial = (unsigned long)-1, pixel_constants_serial = (unsigned long)-1;

static ID3D11Buffer *buffer_create(UINT size, UINT bind)
{
	D3D11_BUFFER_DESC description;
	ID3D11Buffer *buffer = NULL;

	memset(&description, 0, sizeof(description));
	description.ByteWidth = size;
	description.Usage = D3D11_USAGE_DYNAMIC;
	description.BindFlags = bind;
	description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(ID3D11Device_CreateBuffer(win32_d3d11_device, &description, NULL, &buffer)))
		platform_log("Direct3D 11: cannot make a %u-byte buffer", (unsigned)size);
	return buffer;
}

/* size bytes appended to the stream, aligned to align; their offset in it,
or (UINT)-1 */
static UINT stream_append(struct stream_buffer *stream, const void *data, UINT size, UINT align)
{
	D3D11_MAPPED_SUBRESOURCE mapped;
	D3D11_MAP map = D3D11_MAP_WRITE_NO_OVERWRITE;
	UINT offset = (stream->offset + align - 1) / align * align;

	if (!stream->buffer || size > stream->size)
		return (UINT)-1;
	if (offset + size > stream->size)
	{
		offset = 0;
		map = D3D11_MAP_WRITE_DISCARD;
	}
	if (FAILED(ID3D11DeviceContext_Map(win32_d3d11_context, (ID3D11Resource *)stream->buffer, 0, map, 0, &mapped)))
	{
		/* (a feature level whose driver will not map without discarding) */
		offset = 0;
		if (FAILED(ID3D11DeviceContext_Map(win32_d3d11_context, (ID3D11Resource *)stream->buffer, 0,
			D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return (UINT)-1;
	}
	memcpy((unsigned char *)mapped.pData + offset, data, size);
	ID3D11DeviceContext_Unmap(win32_d3d11_context, (ID3D11Resource *)stream->buffer, 0);
	stream->offset = offset + size;
	return offset;
}

static void constants_upload(ID3D11Buffer *buffer, const void *data, UINT size)
{
	D3D11_MAPPED_SUBRESOURCE mapped;

	if (buffer && SUCCEEDED(ID3D11DeviceContext_Map(win32_d3d11_context, (ID3D11Resource *)buffer, 0,
		D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		memcpy(mapped.pData, data, size);
		ID3D11DeviceContext_Unmap(win32_d3d11_context, (ID3D11Resource *)buffer, 0);
	}
}

/* ---------- the clears' and the presentation's quads */

static ID3D11VertexShader *quad_vertex_shader, *blit_vertex_shader;
static ID3D11PixelShader *clear_pixel_shader, *blit_pixel_shader;
static ID3D11InputLayout *quad_layout, *blit_layout;
static ID3D11Buffer *clear_constant_buffer;
static ID3D11SamplerState *blit_sampler;

static const char quad_vertex_hlsl[] =
	"float4 main(float4 position : TEXCOORD0) : SV_Position { return position; }\n";
static const char clear_pixel_hlsl[] =
	"cbuffer clear : register(b0) { float4 color; };\n"
	"float4 main(float4 position : SV_Position) : SV_Target { return color; }\n";
static const char blit_vertex_hlsl[] =
	"struct output { float4 position : SV_Position; float2 coordinates : TEXCOORD0; };\n"
	"output main(float4 position : TEXCOORD0)\n"
	"{\n"
	"\toutput o;\n"
	"\to.position = float4(position.xy, 0.0, 1.0);\n"
	"\to.coordinates = position.zw;\n"
	"\treturn o;\n"
	"}\n";
static const char blit_pixel_hlsl[] =
	"Texture2D picture : register(t0);\n"
	"SamplerState linear_clamp : register(s0);\n"
	"float4 main(float4 position : SV_Position, float2 coordinates : TEXCOORD0) : SV_Target\n"
	"{\n"
	/* (destination alpha is the game's scratch: the display shows none) */
	"\treturn float4(picture.Sample(linear_clamp, coordinates).rgb, 1.0);\n"
	"}\n";

static BOOL quad_shaders_create(void)
{
	static const D3D11_INPUT_ELEMENT_DESC element = {
		"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0
	};
	ID3DBlob *code;
	D3D11_SAMPLER_DESC sampler;
	BOOL success = TRUE;

	if ((code = compile(quad_vertex_hlsl, vertex_profile, "clear vertex")) != NULL)
	{
		success &= SUCCEEDED(ID3D11Device_CreateVertexShader(win32_d3d11_device, ID3D10Blob_GetBufferPointer(code),
			ID3D10Blob_GetBufferSize(code), NULL, &quad_vertex_shader));
		success &= SUCCEEDED(ID3D11Device_CreateInputLayout(win32_d3d11_device, &element, 1,
			ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), &quad_layout));
		RELEASE(code);
	}
	if ((code = compile(blit_vertex_hlsl, vertex_profile, "blit vertex")) != NULL)
	{
		success &= SUCCEEDED(ID3D11Device_CreateVertexShader(win32_d3d11_device, ID3D10Blob_GetBufferPointer(code),
			ID3D10Blob_GetBufferSize(code), NULL, &blit_vertex_shader));
		success &= SUCCEEDED(ID3D11Device_CreateInputLayout(win32_d3d11_device, &element, 1,
			ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), &blit_layout));
		RELEASE(code);
	}
	if ((code = compile(clear_pixel_hlsl, pixel_profile, "clear pixel")) != NULL)
	{
		success &= SUCCEEDED(ID3D11Device_CreatePixelShader(win32_d3d11_device, ID3D10Blob_GetBufferPointer(code),
			ID3D10Blob_GetBufferSize(code), NULL, &clear_pixel_shader));
		RELEASE(code);
	}
	if ((code = compile(blit_pixel_hlsl, pixel_profile, "blit pixel")) != NULL)
	{
		success &= SUCCEEDED(ID3D11Device_CreatePixelShader(win32_d3d11_device, ID3D10Blob_GetBufferPointer(code),
			ID3D10Blob_GetBufferSize(code), NULL, &blit_pixel_shader));
		RELEASE(code);
	}
	clear_constant_buffer = buffer_create(16, D3D11_BIND_CONSTANT_BUFFER);
	memset(&sampler, 0, sizeof(sampler));
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxAnisotropy = 1;
	sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	blit_sampler = sampler_state(&sampler);
	return success && quad_vertex_shader && blit_vertex_shader && clear_pixel_shader && blit_pixel_shader;
}

/* draws a quad of four float4 vertices (a triangle strip) */
static void quad_draw(const float vertices[4][4])
{
	UINT offset = stream_append(&vertex_stream, vertices, 4 * 4 * sizeof(float), 16), stride = 4 * sizeof(float);

	if (offset == (UINT)-1)
		return;
	ID3D11DeviceContext_IASetVertexBuffers(win32_d3d11_context, 0, 1, &vertex_stream.buffer, &stride, &offset);
	ID3D11DeviceContext_IASetPrimitiveTopology(win32_d3d11_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ID3D11DeviceContext_Draw(win32_d3d11_context, 4, 0);
}

/* ---------- the bound pipeline, as last set (a draw sets what changed) */

static struct
{
	ID3D11RenderTargetView *color_view;
	ID3D11DepthStencilView *depth_view;
} bound;

/* the state the clears and the presentation set behind the draws' backs */
static void bound_forget(void)
{
	memset(&bound, 0, sizeof(bound));
	vertex_constants_serial = pixel_constants_serial = (unsigned long)-1;
}

static void targets_bind(ID3D11RenderTargetView *color, ID3D11DepthStencilView *depth)
{
	static ID3D11ShaderResourceView *const no_views[4] = { NULL, NULL, NULL, NULL };

	if (bound.color_view == color && bound.depth_view == depth && (color || depth))
		return;
	/* (no texture of the last draw is left bound while it becomes a target) */
	ID3D11DeviceContext_PSSetShaderResources(win32_d3d11_context, 0, 4, no_views);
	ID3D11DeviceContext_OMSetRenderTargets(win32_d3d11_context, color ? 1 : 0, color ? &color : NULL, depth);
	bound.color_view = color;
	bound.depth_view = depth;
}

/* ---------- clears */

void win32_d3d11_clear(unsigned int color_target, unsigned int depth_target, int left, int top, int right, int bottom,
	int clear_color, unsigned int color_mask, const float color[4], int clear_depth, float depth, int clear_stencil,
	unsigned int stencil)
{
	struct target *color_entry = target_get(color_target), *depth_entry = target_get(depth_target);
	struct target *size = color_entry ? color_entry : depth_entry;
	D3D11_BLEND_DESC blend;
	D3D11_DEPTH_STENCIL_DESC depth_stencil;
	D3D11_RASTERIZER_DESC raster;
	D3D11_VIEWPORT viewport;
	D3D11_RECT rectangle;
	float vertices[4][4];
	BOOL whole;
	int corner;

	if (!win32_d3d11_device || !size)
		return;
	if (color_entry && color_entry->depth)
		color_entry = NULL;
	if (depth_entry && !depth_entry->depth)
		depth_entry = NULL;
	if (!color_entry)
		clear_color = FALSE;
	if (!depth_entry)
		clear_depth = clear_stencil = FALSE;
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > (int)size->width)
		right = (int)size->width;
	if (bottom > (int)size->height)
		bottom = (int)size->height;
	if (left >= right || top >= bottom || !(clear_color || clear_depth || clear_stencil))
		return;
	whole = left == 0 && top == 0 && right == (int)size->width && bottom == (int)size->height;
	/* the whole of a target, all of its channels: the views' own clears */
	if (whole && (!clear_color || (color_mask & 15) == 15))
	{
		if (clear_color)
			ID3D11DeviceContext_ClearRenderTargetView(win32_d3d11_context, color_entry->color_view, color);
		if (clear_depth || clear_stencil)
			ID3D11DeviceContext_ClearDepthStencilView(win32_d3d11_context, depth_entry->depth_view,
				(clear_depth ? D3D11_CLEAR_DEPTH : 0) | (clear_stencil ? D3D11_CLEAR_STENCIL : 0), depth, (UINT8)stencil);
		return;
	}
	/* otherwise a quad over the rectangle, writing what is cleared */
	targets_bind(color_entry ? color_entry->color_view : NULL, depth_entry ? depth_entry->depth_view : NULL);
	memset(&blend, 0, sizeof(blend));
	blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].RenderTargetWriteMask = (UINT8)(clear_color ?
		((color_mask & 1) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) | ((color_mask & 2) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
		((color_mask & 4) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) | ((color_mask & 8) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0) : 0);
	memset(&depth_stencil, 0, sizeof(depth_stencil));
	depth_stencil.DepthEnable = clear_depth;
	depth_stencil.DepthWriteMask = clear_depth ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	depth_stencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
	depth_stencil.StencilEnable = clear_stencil;
	depth_stencil.StencilReadMask = 0xff;
	depth_stencil.StencilWriteMask = 0xff;
	depth_stencil.FrontFace.StencilFailOp = depth_stencil.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_REPLACE;
	depth_stencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
	depth_stencil.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	depth_stencil.BackFace = depth_stencil.FrontFace;
	memset(&raster, 0, sizeof(raster));
	raster.FillMode = D3D11_FILL_SOLID;
	raster.CullMode = D3D11_CULL_NONE;
	raster.DepthClipEnable = TRUE;
	raster.ScissorEnable = TRUE;
	ID3D11DeviceContext_OMSetBlendState(win32_d3d11_context, blend_state(&blend), NULL, 0xffffffff);
	ID3D11DeviceContext_OMSetDepthStencilState(win32_d3d11_context, depth_state(&depth_stencil), stencil & 0xff);
	ID3D11DeviceContext_RSSetState(win32_d3d11_context, raster_state(&raster));
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = (float)size->width;
	viewport.Height = (float)size->height;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	ID3D11DeviceContext_RSSetViewports(win32_d3d11_context, 1, &viewport);
	rectangle.left = left;
	rectangle.top = top;
	rectangle.right = right;
	rectangle.bottom = bottom;
	ID3D11DeviceContext_RSSetScissorRects(win32_d3d11_context, 1, &rectangle);
	constants_upload(clear_constant_buffer, color, 4 * sizeof(float));
	ID3D11DeviceContext_PSSetConstantBuffers(win32_d3d11_context, 0, 1, &clear_constant_buffer);
	ID3D11DeviceContext_VSSetShader(win32_d3d11_context, quad_vertex_shader, NULL, 0);
	ID3D11DeviceContext_PSSetShader(win32_d3d11_context, clear_pixel_shader, NULL, 0);
	ID3D11DeviceContext_IASetInputLayout(win32_d3d11_context, quad_layout);
	for (corner = 0; corner < 4; corner++)
	{
		vertices[corner][0] = (corner & 1) ? 1.0f : -1.0f;
		vertices[corner][1] = (corner & 2) ? -1.0f : 1.0f;
		vertices[corner][2] = depth < 0.0f ? 0.0f : depth > 1.0f ? 1.0f : depth;
		vertices[corner][3] = 1.0f;
	}
	quad_draw((const float (*)[4])vertices);
	bound_forget();
	bound.color_view = color_entry ? color_entry->color_view : NULL;
	bound.depth_view = depth_entry ? depth_entry->depth_view : NULL;
}

/* ---------- draws */

static D3D11_BLEND blend_factor(unsigned int factor, BOOL alpha)
{
	switch (factor)
	{
	case GL_ZERO: return D3D11_BLEND_ZERO;
	case GL_ONE: return D3D11_BLEND_ONE;
	case GL_SRC_COLOR: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
	case GL_SRC_ALPHA: return D3D11_BLEND_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
	case GL_DST_ALPHA: return D3D11_BLEND_DEST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
	case GL_DST_COLOR: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
	case GL_ONE_MINUS_DST_COLOR: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
	case GL_SRC_ALPHA_SATURATE: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
	case GL_CONSTANT_COLOR:
	case GL_CONSTANT_ALPHA: return D3D11_BLEND_BLEND_FACTOR;
	case GL_ONE_MINUS_CONSTANT_COLOR:
	case GL_ONE_MINUS_CONSTANT_ALPHA: return D3D11_BLEND_INV_BLEND_FACTOR;
	default: return D3D11_BLEND_ONE;
	}
}

static D3D11_BLEND_OP blend_operation(unsigned int equation)
{
	switch (equation)
	{
	case GL_FUNC_SUBTRACT: return D3D11_BLEND_OP_SUBTRACT;
	case GL_FUNC_REVERSE_SUBTRACT: return D3D11_BLEND_OP_REV_SUBTRACT;
	case GL_MIN: return D3D11_BLEND_OP_MIN;
	case GL_MAX: return D3D11_BLEND_OP_MAX;
	default: return D3D11_BLEND_OP_ADD;
	}
}

static D3D11_COMPARISON_FUNC comparison(unsigned int function)
{
	return function >= GL_NEVER && function <= GL_ALWAYS ? (D3D11_COMPARISON_FUNC)(function - GL_NEVER + 1) :
		D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP stencil_operation(unsigned int operation)
{
	switch (operation)
	{
	case GL_ZERO: return D3D11_STENCIL_OP_ZERO;
	case GL_REPLACE: return D3D11_STENCIL_OP_REPLACE;
	case GL_INCR: return D3D11_STENCIL_OP_INCR_SAT;
	case GL_DECR: return D3D11_STENCIL_OP_DECR_SAT;
	case GL_INVERT: return D3D11_STENCIL_OP_INVERT;
	case GL_INCR_WRAP: return D3D11_STENCIL_OP_INCR;
	case GL_DECR_WRAP: return D3D11_STENCIL_OP_DECR;
	default: return D3D11_STENCIL_OP_KEEP;
	}
}

static D3D11_TEXTURE_ADDRESS_MODE address_mode(unsigned int mode)
{
	switch (mode)
	{
	case GL_MIRRORED_REPEAT: return D3D11_TEXTURE_ADDRESS_MIRROR;
	case GL_CLAMP_TO_EDGE: return D3D11_TEXTURE_ADDRESS_CLAMP;
	case GL_CLAMP_TO_BORDER: return D3D11_TEXTURE_ADDRESS_BORDER;
	default: return D3D11_TEXTURE_ADDRESS_WRAP;
	}
}

static ID3D11SamplerState *sampler_get(const struct win32_d3d11_sampler *sampler)
{
	D3D11_SAMPLER_DESC description;
	BOOL linear_minification = sampler->minification == GL_LINEAR;
	BOOL linear_magnification = sampler->magnification == GL_LINEAR;
	BOOL linear_mip = sampler->mip == GL_LINEAR;

	memset(&description, 0, sizeof(description));
	if (sampler->anisotropy > 1 && linear_minification)
		description.Filter = D3D11_FILTER_ANISOTROPIC;
	else
		description.Filter = (D3D11_FILTER)((linear_minification ? 0x10 : 0) | (linear_magnification ? 0x4 : 0) |
			(linear_mip ? 0x1 : 0));
	description.AddressU = address_mode(sampler->address[0]);
	description.AddressV = address_mode(sampler->address[1]);
	description.AddressW = address_mode(sampler->address[2]);
	description.MipLODBias = sampler->lod_bias < -16.0f ? -16.0f : sampler->lod_bias > 15.99f ? 15.99f : sampler->lod_bias;
	description.MaxAnisotropy = sampler->anisotropy > 1 ? (sampler->anisotropy > 16 ? 16 : sampler->anisotropy) : 1;
	description.ComparisonFunc = D3D11_COMPARISON_NEVER;
	memcpy(description.BorderColor, sampler->border, sizeof(description.BorderColor));
	description.MinLOD = sampler->minimum_lod;
	/* (no mip filter: the first level only) */
	description.MaxLOD = sampler->mip ? D3D11_FLOAT32_MAX : sampler->minimum_lod;
	return sampler_state(&description);
}

void win32_d3d11_draw(const struct win32_d3d11_draw *draw)
{
	static const D3D11_PRIMITIVE_TOPOLOGY topologies[] = {
		D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
		D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
	};
	struct target *color = target_get(draw->color_target), *depth = target_get(draw->depth_target);
	struct shader_entry *vertex_shader, *pixel_shader;
	ID3D11ShaderResourceView *views[4] = { NULL, NULL, NULL, NULL };
	ID3D11SamplerState *samplers[4] = { NULL, NULL, NULL, NULL };
	D3D11_BLEND_DESC blend;
	D3D11_DEPTH_STENCIL_DESC depth_stencil;
	D3D11_RASTERIZER_DESC raster;
	D3D11_VIEWPORT viewport;
	D3D11_RECT rectangle;
	UINT vertex_offset, index_offset = 0, stride = draw->stride;
	float factor[4];
	int stage;

	if (!win32_d3d11_device || !draw->vertex_shader || draw->vertex_shader > shader_count || !draw->pixel_shader ||
		draw->pixel_shader > shader_count || !draw->vertex_count)
		return;
	vertex_shader = shaders[draw->vertex_shader - 1];
	pixel_shader = shaders[draw->pixel_shader - 1];
	if (color && color->depth)
		color = NULL;
	if (depth && !depth->depth)
		depth = NULL;
	if (!color && !depth)
		return;
	/* the vertices first: a full stream is discarded, which leaves nothing
	bound behind */
	vertex_offset = stream_append(&vertex_stream, draw->vertices, draw->vertex_count * stride, stride);
	if (vertex_offset == (UINT)-1)
		return;
	if (draw->indices)
	{
		index_offset = stream_append(&index_stream, draw->indices, draw->index_count * sizeof(unsigned short), 4);
		if (index_offset == (UINT)-1)
			return;
	}

	targets_bind(color ? color->color_view : NULL, depth ? depth->depth_view : NULL);
	for (stage = 0; stage < 4; stage++)
	{
		struct target *target;

		switch (draw->texture_kind[stage])
		{
		case _win32_d3d11_texture_gl:
			views[stage] = win32_d3d11_texture_view(draw->texture[stage]);
			break;
		case _win32_d3d11_texture_target:
			target = target_get(draw->texture[stage]);
			if (target && !target->depth)
				views[stage] = target == color ? target_copy_view(target) : target->view;
			break;
		case _win32_d3d11_texture_mipmaps:
			if (draw->texture[stage] && draw->texture[stage] <= mipmaps_count)
				views[stage] = mipmaps_ids[draw->texture[stage] - 1]->view;
			break;
		default:
			break;
		}
		if (views[stage])
			samplers[stage] = sampler_get(&draw->samplers[stage]);
	}
	ID3D11DeviceContext_PSSetShaderResources(win32_d3d11_context, 0, 4, views);
	ID3D11DeviceContext_PSSetSamplers(win32_d3d11_context, 0, 4, samplers);

	viewport.TopLeftX = draw->viewport[0];
	viewport.TopLeftY = draw->viewport[1];
	viewport.Width = draw->viewport[2];
	viewport.Height = draw->viewport[3];
	viewport.MinDepth = draw->depth_range[0];
	viewport.MaxDepth = draw->depth_range[1];
	ID3D11DeviceContext_RSSetViewports(win32_d3d11_context, 1, &viewport);
	rectangle.left = draw->scissor[0];
	rectangle.top = draw->scissor[1];
	rectangle.right = draw->scissor[2];
	rectangle.bottom = draw->scissor[3];
	ID3D11DeviceContext_RSSetScissorRects(win32_d3d11_context, 1, &rectangle);

	memset(&blend, 0, sizeof(blend));
	if (draw->blend_equation)
	{
		blend.RenderTarget[0].BlendEnable = TRUE;
		blend.RenderTarget[0].SrcBlend = blend_factor(draw->blend_source, FALSE);
		blend.RenderTarget[0].DestBlend = blend_factor(draw->blend_destination, FALSE);
		blend.RenderTarget[0].BlendOp = blend_operation(draw->blend_equation);
		blend.RenderTarget[0].SrcBlendAlpha = blend_factor(draw->blend_source, TRUE);
		blend.RenderTarget[0].DestBlendAlpha = blend_factor(draw->blend_destination, TRUE);
		blend.RenderTarget[0].BlendOpAlpha = blend.RenderTarget[0].BlendOp;
	}
	else
	{
		blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	}
	blend.RenderTarget[0].RenderTargetWriteMask = (UINT8)(((draw->color_mask & 1) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
		((draw->color_mask & 2) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
		((draw->color_mask & 4) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
		((draw->color_mask & 8) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
	/* (a constant alpha blends by the factor's alpha in every channel) */
	memcpy(factor, draw->blend_factor, sizeof(factor));
	if (draw->blend_source == GL_CONSTANT_ALPHA || draw->blend_source == GL_ONE_MINUS_CONSTANT_ALPHA ||
		draw->blend_destination == GL_CONSTANT_ALPHA || draw->blend_destination == GL_ONE_MINUS_CONSTANT_ALPHA)
		factor[0] = factor[1] = factor[2] = factor[3];
	ID3D11DeviceContext_OMSetBlendState(win32_d3d11_context, blend_state(&blend), factor, 0xffffffff);

	memset(&depth_stencil, 0, sizeof(depth_stencil));
	depth_stencil.DepthEnable = depth && draw->depth_function;
	depth_stencil.DepthWriteMask = depth_stencil.DepthEnable && draw->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL :
		D3D11_DEPTH_WRITE_MASK_ZERO;
	depth_stencil.DepthFunc = depth_stencil.DepthEnable ? comparison(draw->depth_function) : D3D11_COMPARISON_ALWAYS;
	depth_stencil.StencilEnable = depth && draw->stencil_function;
	depth_stencil.StencilReadMask = depth_stencil.StencilEnable ? (UINT8)draw->stencil_read_mask : 0xff;
	depth_stencil.StencilWriteMask = depth_stencil.StencilEnable ? (UINT8)draw->stencil_write_mask : 0xff;
	if (depth_stencil.StencilEnable)
	{
		depth_stencil.FrontFace.StencilFailOp = stencil_operation(draw->stencil_fail);
		depth_stencil.FrontFace.StencilDepthFailOp = stencil_operation(draw->stencil_depth_fail);
		depth_stencil.FrontFace.StencilPassOp = stencil_operation(draw->stencil_pass);
		depth_stencil.FrontFace.StencilFunc = comparison(draw->stencil_function);
	}
	else
	{
		depth_stencil.FrontFace.StencilFailOp = depth_stencil.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		depth_stencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		depth_stencil.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	}
	depth_stencil.BackFace = depth_stencil.FrontFace;
	ID3D11DeviceContext_OMSetDepthStencilState(win32_d3d11_context, depth_state(&depth_stencil),
		draw->stencil_reference & 0xff);

	memset(&raster, 0, sizeof(raster));
	raster.FillMode = draw->wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
	raster.CullMode = draw->cull == GL_FRONT ? D3D11_CULL_FRONT : draw->cull == GL_BACK ? D3D11_CULL_BACK :
		D3D11_CULL_NONE;
	raster.FrontCounterClockwise = draw->front_counter_clockwise != 0;
	raster.DepthBias = (INT)floorf(draw->depth_bias + 0.5f);
	raster.SlopeScaledDepthBias = draw->slope_scaled_depth_bias;
	raster.DepthClipEnable = TRUE;
	raster.ScissorEnable = TRUE;
	ID3D11DeviceContext_RSSetState(win32_d3d11_context, raster_state(&raster));

	if (draw->vertex_constants_serial != vertex_constants_serial)
	{
		vertex_constants_serial = draw->vertex_constants_serial;
		constants_upload(vertex_constant_buffer, draw->vertex_constants, draw->vertex_constants_size);
	}
	if (draw->pixel_constants_serial != pixel_constants_serial)
	{
		pixel_constants_serial = draw->pixel_constants_serial;
		constants_upload(pixel_constant_buffer, draw->pixel_constants, draw->pixel_constants_size);
	}
	ID3D11DeviceContext_VSSetConstantBuffers(win32_d3d11_context, 0, 1, &vertex_constant_buffer);
	ID3D11DeviceContext_PSSetConstantBuffers(win32_d3d11_context, 0, 1, &pixel_constant_buffer);
	ID3D11DeviceContext_VSSetShader(win32_d3d11_context, vertex_shader->vertex_shader, NULL, 0);
	ID3D11DeviceContext_PSSetShader(win32_d3d11_context, pixel_shader->pixel_shader, NULL, 0);
	ID3D11DeviceContext_IASetInputLayout(win32_d3d11_context, vertex_shader->layout);
	ID3D11DeviceContext_IASetVertexBuffers(win32_d3d11_context, 0, 1, &vertex_stream.buffer, &stride, &vertex_offset);
	ID3D11DeviceContext_IASetPrimitiveTopology(win32_d3d11_context,
		topologies[draw->topology >= 0 && draw->topology <= _win32_d3d11_triangle_strip ? draw->topology : 0]);
	if (draw->indices)
	{
		ID3D11DeviceContext_IASetIndexBuffer(win32_d3d11_context, index_stream.buffer, DXGI_FORMAT_R16_UINT, index_offset);
		ID3D11DeviceContext_DrawIndexed(win32_d3d11_context, draw->index_count, 0, draw->base_vertex);
	}
	else
	{
		ID3D11DeviceContext_Draw(win32_d3d11_context, draw->vertex_count, 0);
	}
}

/* ---------- occlusion queries */

struct query
{
	ID3D11Query *query;
	BOOL busy;
};

static struct query *queries;
static unsigned int query_count;
static ID3D11Query *active_query;

void win32_d3d11_query_begin(void)
{
	D3D11_QUERY_DESC description = { D3D11_QUERY_OCCLUSION, 0 };
	unsigned int index;

	if (!win32_d3d11_device || active_query)
		return;
	for (index = 0; index < query_count && queries[index].busy; index++)
	{
	}
	if (index == query_count)
	{
		queries = realloc(queries, (query_count + 1) * sizeof(*queries));
		memset(&queries[query_count], 0, sizeof(queries[0]));
		if (FAILED(ID3D11Device_CreateQuery(win32_d3d11_device, &description, &queries[query_count].query)))
			return;
		query_count++;
	}
	queries[index].busy = TRUE;
	active_query = queries[index].query;
	ID3D11DeviceContext_Begin(win32_d3d11_context, (ID3D11Asynchronous *)active_query);
}

unsigned int win32_d3d11_query_end(void)
{
	unsigned int index;

	if (!active_query)
		return 0;
	ID3D11DeviceContext_End(win32_d3d11_context, (ID3D11Asynchronous *)active_query);
	for (index = 0; index < query_count && queries[index].query != active_query; index++)
	{
	}
	active_query = NULL;
	return index < query_count ? index + 1 : 0;
}

/* the query is free again once its result is read */
int win32_d3d11_query_result(unsigned int query, unsigned long *samples)
{
	UINT64 count = 0;

	if (!query || query > query_count || !queries[query - 1].busy)
		return FALSE;
	if (ID3D11DeviceContext_GetData(win32_d3d11_context, (ID3D11Asynchronous *)queries[query - 1].query, &count,
		sizeof(count), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
		return FALSE;
	queries[query - 1].busy = FALSE;
	*samples = (unsigned long)(count > 0xffffffffUL ? 0xffffffffUL : count);
	return TRUE;
}

void win32_d3d11_query_release(unsigned int query)
{
	if (query && query <= query_count && queries[query - 1].query != active_query)
		queries[query - 1].busy = FALSE;
}

/* ---------- presentation */

static BOOL swap_chain_size(UINT width, UINT height)
{
	ID3D11Texture2D *buffer = NULL;
	HRESULT result;

	if (width == swap_width && height == swap_height && back_buffer_view)
		return TRUE;
	ID3D11DeviceContext_OMSetRenderTargets(win32_d3d11_context, 0, NULL, NULL);
	bound_forget();
	RELEASE(back_buffer_view);
	result = IDXGISwapChain_ResizeBuffers(swap_chain, 0, width, height, DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(result))
	{
		platform_log("Direct3D 11: cannot resize the swap chain to %ux%u (%08lx)", width, height, (unsigned long)result);
		return FALSE;
	}
	swap_width = width;
	swap_height = height;
	if (FAILED(IDXGISwapChain_GetBuffer(swap_chain, 0, &IID_ID3D11Texture2D, (void **)&buffer)))
		return FALSE;
	result = ID3D11Device_CreateRenderTargetView(win32_d3d11_device, (ID3D11Resource *)buffer, NULL, &back_buffer_view);
	RELEASE(buffer);
	return SUCCEEDED(result);
}

void win32_d3d11_present(unsigned int target, int x, int y, int width, int height, int window_width,
	int window_height, int vsync)
{
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	static ID3D11ShaderResourceView *const no_views[4] = { NULL, NULL, NULL, NULL };
	struct target *picture = target_get(target);
	D3D11_BLEND_DESC blend;
	D3D11_DEPTH_STENCIL_DESC depth_stencil;
	D3D11_RASTERIZER_DESC raster;
	D3D11_VIEWPORT viewport;
	D3D11_RECT rectangle;
	HRESULT result;

	if (!win32_d3d11_device || window_width <= 0 || window_height <= 0 ||
		!swap_chain_size((UINT)window_width, (UINT)window_height))
		return;
	ID3D11DeviceContext_PSSetShaderResources(win32_d3d11_context, 0, 4, no_views);
	ID3D11DeviceContext_OMSetRenderTargets(win32_d3d11_context, 1, &back_buffer_view, NULL);
	ID3D11DeviceContext_ClearRenderTargetView(win32_d3d11_context, back_buffer_view, black);
	if (picture && !picture->depth && width > 0 && height > 0)
	{
		float vertices[4][4];
		int corner;

		memset(&blend, 0, sizeof(blend));
		blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		memset(&depth_stencil, 0, sizeof(depth_stencil));
		depth_stencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
		depth_stencil.FrontFace.StencilFunc = depth_stencil.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		depth_stencil.FrontFace.StencilFailOp = depth_stencil.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		depth_stencil.FrontFace.StencilDepthFailOp = depth_stencil.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		depth_stencil.FrontFace.StencilPassOp = depth_stencil.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		memset(&raster, 0, sizeof(raster));
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		raster.DepthClipEnable = TRUE;
		raster.ScissorEnable = FALSE;
		ID3D11DeviceContext_OMSetBlendState(win32_d3d11_context, blend_state(&blend), NULL, 0xffffffff);
		ID3D11DeviceContext_OMSetDepthStencilState(win32_d3d11_context, depth_state(&depth_stencil), 0);
		ID3D11DeviceContext_RSSetState(win32_d3d11_context, raster_state(&raster));
		viewport.TopLeftX = (float)x;
		viewport.TopLeftY = (float)y;
		viewport.Width = (float)width;
		viewport.Height = (float)height;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		ID3D11DeviceContext_RSSetViewports(win32_d3d11_context, 1, &viewport);
		rectangle.left = 0;
		rectangle.top = 0;
		rectangle.right = window_width;
		rectangle.bottom = window_height;
		ID3D11DeviceContext_RSSetScissorRects(win32_d3d11_context, 1, &rectangle);
		ID3D11DeviceContext_VSSetShader(win32_d3d11_context, blit_vertex_shader, NULL, 0);
		ID3D11DeviceContext_PSSetShader(win32_d3d11_context, blit_pixel_shader, NULL, 0);
		ID3D11DeviceContext_IASetInputLayout(win32_d3d11_context, blit_layout);
		ID3D11DeviceContext_PSSetShaderResources(win32_d3d11_context, 0, 1, &picture->view);
		ID3D11DeviceContext_PSSetSamplers(win32_d3d11_context, 0, 1, &blit_sampler);
		/* x, y and the texture's u, v: row 0 of the target is the top of the
		picture */
		for (corner = 0; corner < 4; corner++)
		{
			vertices[corner][0] = (corner & 1) ? 1.0f : -1.0f;
			vertices[corner][1] = (corner & 2) ? -1.0f : 1.0f;
			vertices[corner][2] = (corner & 1) ? 1.0f : 0.0f;
			vertices[corner][3] = (corner & 2) ? 1.0f : 0.0f;
		}
		quad_draw((const float (*)[4])vertices);
		ID3D11DeviceContext_PSSetShaderResources(win32_d3d11_context, 0, 4, no_views);
	}
	result = IDXGISwapChain_Present(swap_chain, vsync ? 1 : 0, 0);
	if (FAILED(result))
	{
		static unsigned long failures;

		if (failures++ < 5)
			platform_log("Direct3D 11: Present failed (%08lx)", (unsigned long)result);
	}
	bound_forget();
}

/* ---------- the device */

static const struct
{
	const char *name;
	D3D_FEATURE_LEVEL level;
} feature_levels[] = {
	{ "9_1", D3D_FEATURE_LEVEL_9_1 },
	{ "9_2", D3D_FEATURE_LEVEL_9_2 },
	{ "9_3", D3D_FEATURE_LEVEL_9_3 },
	{ "10_0", D3D_FEATURE_LEVEL_10_0 },
	{ "10_1", D3D_FEATURE_LEVEL_10_1 },
	{ "11_0", D3D_FEATURE_LEVEL_11_0 },
	{ "11_1", D3D_FEATURE_LEVEL_11_1 },
};

static const char *feature_level_name(D3D_FEATURE_LEVEL level)
{
	unsigned int index;

	for (index = 0; index < sizeof(feature_levels) / sizeof(feature_levels[0]); index++)
	{
		if (feature_levels[index].level == level)
			return feature_levels[index].name;
	}
	return "?";
}

int win32_d3d11_maximum_texture_size(void)
{
	switch (win32_d3d11_feature_level)
	{
	case D3D_FEATURE_LEVEL_9_1:
	case D3D_FEATURE_LEVEL_9_2: return 2048;
	case D3D_FEATURE_LEVEL_9_3: return 4096;
	case D3D_FEATURE_LEVEL_10_0:
	case D3D_FEATURE_LEVEL_10_1: return 8192;
	default: return 16384;
	}
}

int win32_d3d11_initialize(void *window, const char *feature_level, int warp)
{
	static const D3D_FEATURE_LEVEL best[] = {
		D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1,
	};
	const D3D_FEATURE_LEVEL *levels = best;
	UINT level_count = sizeof(best) / sizeof(best[0]), index;
	D3D_FEATURE_LEVEL wanted;
	DXGI_SWAP_CHAIN_DESC description;
	IDXGIDevice *dxgi_device = NULL;
	IDXGIAdapter *adapter = NULL;
	IDXGIFactory *factory = NULL;
	DXGI_ADAPTER_DESC adapter_description;
	HMODULE compiler;
	HWND hwnd;
	HRESULT result;
	int width = 0, height = 0;

	if (win32_d3d11_device)
		return TRUE;
	hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window *)window),
		SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
	if (!hwnd)
	{
		platform_log("Direct3D 11: the window has no handle");
		return FALSE;
	}
	/* the feature level asked for and nothing else, so that this computer
	draws as a Windows RT device would (or the best there is) */
	for (index = 0; feature_level && index < sizeof(feature_levels) / sizeof(feature_levels[0]); index++)
	{
		if (!strcmp(feature_level, feature_levels[index].name))
		{
			wanted = feature_levels[index].level;
			levels = &wanted;
			level_count = 1;
		}
	}
	/* (the 11_1 level is unknown to Windows 8's runtime: tried without it) */
	result = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, level_count,
		D3D11_SDK_VERSION, &win32_d3d11_device, &win32_d3d11_feature_level, &win32_d3d11_context);
	if (result == E_INVALIDARG && levels == best)
		result = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL, 0, best + 1,
			level_count - 1, D3D11_SDK_VERSION, &win32_d3d11_device, &win32_d3d11_feature_level, &win32_d3d11_context);
	if (FAILED(result))
	{
		platform_log("Direct3D 11: cannot make a device at feature level %s%s (%08lx)",
			levels == best ? "9_1 or better" : feature_level, warp ? " on WARP" : "", (unsigned long)result);
		return FALSE;
	}

	SDL_GetWindowSizeInPixels((SDL_Window *)window, &width, &height);
	memset(&description, 0, sizeof(description));
	description.BufferDesc.Width = (UINT)(width > 0 ? width : 640);
	description.BufferDesc.Height = (UINT)(height > 0 ? height : 480);
	description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	description.SampleDesc.Count = 1;
	description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	description.BufferCount = 1;
	description.OutputWindow = hwnd;
	description.Windowed = TRUE;
	description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	result = ID3D11Device_QueryInterface(win32_d3d11_device, &IID_IDXGIDevice, (void **)&dxgi_device);
	if (SUCCEEDED(result))
		result = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
	if (SUCCEEDED(result))
		result = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory);
	if (SUCCEEDED(result))
		result = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)win32_d3d11_device, &description, &swap_chain);
	if (SUCCEEDED(result))
	{
		/* (SDL switches to and from fullscreen itself) */
		IDXGIFactory_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
	}
	memset(&adapter_description, 0, sizeof(adapter_description));
	if (adapter)
		IDXGIAdapter_GetDesc(adapter, &adapter_description);
	RELEASE(factory);
	RELEASE(adapter);
	RELEASE(dxgi_device);
	if (FAILED(result))
	{
		platform_log("Direct3D 11: cannot make the swap chain (%08lx)", (unsigned long)result);
		return FALSE;
	}
	platform_log("Direct3D 11: feature level %s on %ls%s", feature_level_name(win32_d3d11_feature_level),
		adapter_description.Description, warp ? " (WARP)" : "");

	/* shader model 4 at the device's feature level */
	if (win32_d3d11_feature_level <= D3D_FEATURE_LEVEL_9_2)
	{
		vertex_profile = "vs_4_0_level_9_1";
		pixel_profile = "ps_4_0_level_9_1";
	}
	else if (win32_d3d11_feature_level == D3D_FEATURE_LEVEL_9_3)
	{
		vertex_profile = "vs_4_0_level_9_3";
		pixel_profile = "ps_4_0_level_9_3";
	}
	else
	{
		vertex_profile = "vs_4_0";
		pixel_profile = "ps_4_0";
	}
	compiler = LoadLibraryW(L"d3dcompiler_47.dll");
	compile_function = compiler ? (pD3DCompile)(void *)GetProcAddress(compiler, "D3DCompile") : NULL;
	if (!compile_function)
	{
		platform_log("Direct3D 11: cannot load the shader compiler (d3dcompiler_47.dll)");
		return FALSE;
	}

	vertex_stream.size = VERTEX_BUFFER_SIZE;
	vertex_stream.buffer = buffer_create(VERTEX_BUFFER_SIZE, D3D11_BIND_VERTEX_BUFFER);
	index_stream.size = INDEX_BUFFER_SIZE;
	index_stream.buffer = buffer_create(INDEX_BUFFER_SIZE, D3D11_BIND_INDEX_BUFFER);
	vertex_constant_buffer = buffer_create(16 * 212, D3D11_BIND_CONSTANT_BUFFER);
	pixel_constant_buffer = buffer_create(16 * 33, D3D11_BIND_CONSTANT_BUFFER);
	if (!vertex_stream.buffer || !index_stream.buffer || !vertex_constant_buffer || !pixel_constant_buffer ||
		!quad_shaders_create())
	{
		platform_log("Direct3D 11: cannot make the device's buffers and shaders");
		return FALSE;
	}
	swap_width = swap_height = 0;
	if (!swap_chain_size(description.BufferDesc.Width, description.BufferDesc.Height))
		return FALSE;
	win32_d3d11_textures_install();
	return TRUE;
}

#endif
