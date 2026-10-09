/*
D3D8_D3D11.C

The Xbox Direct3D 8 device, implemented with Direct3D 11 at feature level
9_3 (HALO_D3D11: Windows RT's GPUs). It is d3d8_gl.c's device with another
GPU interface under it: the game's calls and state are taken the same way,
the vertex programs and pixel shaders translated by the same translators
(into HLSL: nv2a_vsh.c, nv2a_psh.c), and the textures cached by the same
texture cache (xbox_textures.c, whose GL calls are the texture shim's,
win32_d3d11_textures.c). What draws is win32_d3d11.c, through
port/windows/include/halo_d3d11.h, which keeps the Windows SDK's names
apart from the Xbox SDK's here.

Unlike the GL device:
- vertices are converted to floats as each draw streams them (9_3 reads
  few of the Xbox's vertex formats: no NORMPACKED3, no 3-element shorts),
  and the attributes a draw's streams do not give are constants of the
  vertex shader, which is compiled for the attributes it is given;
- quads, fans, polygons and line loops are drawn as lists (Direct3D 11 has
  none of them);
- there is no anti-aliasing but supersampling, no per-pixel lighting and no
  point sizes (9_3 has none of the GPU features they use);
- the overlay (ui_overlay.c) is not drawn yet.
*/

#ifdef HALO_D3D11

#include "xgpu.h"
#include "sdl_platform.h"
#include "halo_ui_pointer.h"
#include "port_config.h"
#include "halo_d3d11.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef HALO_GAME_BROWSER
#include "browser.h"
#endif

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);
void d3d8_surface_resize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);
void memory_watch_initialize(void);

/* ---------- the screen's width (as d3d8_gl.c: see there) */

#define SCREEN_HEIGHT 480
#define SCREEN_MAXIMUM_WIDTH 1920

static long screen_width;
static float screen_scale[2] = { 1.0f, 1.0f };
static long ui_offset;
#define UI_OFFSET ((int)ui_offset)

/* ---------- anti-aliasing

display.anti_aliasing: of d3d8_gl.c's, supersampling only (the passes and
multisampling need more than 9_3 has); the others draw without */

enum
{
	_anti_aliasing_off,
	_anti_aliasing_ssaa,
};

static const struct
{
	const char *name;
	int mode;
} anti_aliasing_values[] =
{
	{ "off", _anti_aliasing_off },
	{ "fxaa", _anti_aliasing_off },
	{ "smaa", _anti_aliasing_off },
	{ "ssaa2x", _anti_aliasing_ssaa },
	{ "msaa2x", _anti_aliasing_off },
	{ "msaa4x", _anti_aliasing_off },
	{ "msaa8x", _anti_aliasing_off },
};

#define NUMBER_OF_ANTI_ALIASING_VALUES ((int)(sizeof(anti_aliasing_values) / sizeof(anti_aliasing_values[0])))

static int anti_aliasing_value = -1;
static int maximum_target_size;

static void anti_aliasing_read(void)
{
	const char *setting = config_string("display.anti_aliasing");
	int value;

	for (value = NUMBER_OF_ANTI_ALIASING_VALUES - 1;
		value > 0 && strcmp(setting, anti_aliasing_values[value].name);
		value--)
	{
	}
	if (value == anti_aliasing_value)
		return;
	anti_aliasing_value = value;
	if (strcmp(setting, anti_aliasing_values[value].name))
		platform_log("anti-aliasing: \"%s\" is unknown, so off", setting);
	else if (value && anti_aliasing_values[value].mode == _anti_aliasing_off)
		platform_log("anti-aliasing: %s is not drawn by Direct3D 11 at feature level 9_3, so off", setting);
	else
		platform_log("anti-aliasing: %s", setting);
}

static int anti_aliasing(void)
{
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
	return anti_aliasing_values[anti_aliasing_value].mode;
}

static void screen_mode_choose(long *width, float scale[2])
{
	long display_width, display_height;

	*width = 640;
	scale[0] = scale[1] = 1.0f;
	if (platform_screen_mode(&display_width, &display_height) && display_width > 0 && display_height > 0)
	{
		long wanted = (SCREEN_HEIGHT * display_width + display_height / 2) / display_height;

		*width = wanted < 640 ? 640 : wanted > SCREEN_MAXIMUM_WIDTH ? SCREEN_MAXIMUM_WIDTH : wanted & ~1L;
		scale[0] = (float)display_width / (float)*width;
		scale[1] = (float)display_height / (float)SCREEN_HEIGHT;
		if (*width != wanted && *width != (wanted & ~1L))
			scale[0] = scale[1] = scale[0] < scale[1] ? scale[0] : scale[1];
		/* the targets drawn no larger than the feature level's largest */
		if (maximum_target_size > 0)
		{
			float factor = anti_aliasing() == _anti_aliasing_ssaa ? 2.0f : 1.0f;

			if (*width * scale[0] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (*width * scale[0]);
			if (SCREEN_HEIGHT * scale[1] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (SCREEN_HEIGHT * scale[1]);
			scale[0] *= factor;
			scale[1] *= factor;
		}
	}
}

long halo_screen_width(void)
{
	if (!screen_width)
	{
		screen_mode_choose(&screen_width, screen_scale);
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", screen_width, SCREEN_HEIGHT,
			screen_width * screen_scale[0], SCREEN_HEIGHT * screen_scale[1]);
	}
	return screen_width;
}

float halo_screen_pixel_scale(void)
{
	halo_screen_width();
	return screen_scale[1];
}

float halo_screen_scale(void)
{
	halo_screen_width();
	return screen_scale[0] > screen_scale[1] ? screen_scale[0] : screen_scale[1];
}

/* display.shadow_resolution (d3d8_gl.c) */
#define SHADOW_MAP_SIZE 128
#define SHADOW_MAP_MAXIMUM_SCALE 8

static long shadow_scale;
static unsigned long shadow_scale_read_at;

static long shadow_scale_choose(void)
{
	long resolution = config_integer("display.shadow_resolution");
	long scale = 1;

	while (scale < SHADOW_MAP_MAXIMUM_SCALE && SHADOW_MAP_SIZE * scale * 2 <= resolution)
		scale *= 2;
	return scale;
}

long halo_shadow_map_scale(void)
{
	if (!shadow_scale)
	{
		shadow_scale_read_at = config_changes();
		shadow_scale = shadow_scale_choose();
		platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow_scale, SHADOW_MAP_SIZE * shadow_scale);
	}
	return shadow_scale;
}

void halo_screen_ui_offset(unsigned char centered)
{
	ui_offset = centered ? (halo_screen_width() - 640) / 2 : 0;
}

/* ---------- state the XDK header's inline functions read and write */

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;
BYTE D3D__StateBlockDirty[1024];

/* ---------- vertex shaders */

#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */
#define VERTEX_PROGRAM_SLOTS 136
/* the input masks a program is drawn with, each its own shader */
#define VERTEX_SHADER_VARIANTS 8

struct vertex_element
{
	unsigned char reg;
	unsigned char stream;
	unsigned char type;
	unsigned char bytes;
	unsigned short offset;
};

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long id;
	DWORD *instructions;
	unsigned long instruction_count;
	struct vertex_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long element_count;
	unsigned long packed_mask;
	struct nv2a_vertex_lighting lighting;
	/* the program's shaders by the input registers they read from streams
	(0 where one failed to compile) */
	struct
	{
		unsigned long input_mask;
		unsigned int shader;
		BOOL tried;
	} variants[VERTEX_SHADER_VARIANTS];
	unsigned int variant_count;
};

/* ---------- render targets */

struct render_target_entry
{
	struct render_target_entry *next;
	struct render_target_entry *next_in_bucket;
	/* (target.texture is the Direct3D 11 target's id) */
	struct xgpu_render_target target;
	unsigned long last_rendered;
};

#define RENDER_TARGET_BUCKET_COUNT 256

static struct render_target_entry *render_target_buckets[RENDER_TARGET_BUCKET_COUNT];
static struct render_target_entry *render_targets;

static struct render_target_entry **render_target_bucket(unsigned long data)
{
	return &render_target_buckets[((data >> 12) ^ (data >> 20)) % RENDER_TARGET_BUCKET_COUNT];
}

/* ---------- the device */

#define VISIBILITY_TEST_SLOTS 4096

struct d3d11_device
{
	D3DPRESENT_PARAMETERS presentation;
	D3DSurface back_buffer;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DVIEWPORT8 viewport;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];
	D3DSHADERCONSTANTMODE shader_constant_mode;

	struct vertex_shader_object *vertex_shader;
	struct vertex_shader_object *program_slots[VERTEX_PROGRAM_SLOTS];
	unsigned long program_address;
	float viewport_scale[4];
	float viewport_offset[4];

	struct
	{
		DWORD data;
		UINT stride;
	} streams[16];
	UINT base_vertex_index;

	float attributes[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	BOOL immediate_active;
	D3DPRIMITIVETYPE immediate_type;
	float *immediate_vertices;
	unsigned long immediate_count;
	unsigned long immediate_capacity;

	/* the vertex shader's constant buffer, whose c[] are the vertex
	constants themselves, and the pixel shader's; each as last sent, and
	the serial that names what was sent */
	struct xgpu_hlsl_vertex_constants vertex_constants, sent_vertex_constants;
	unsigned long vertex_constants_serial;
	struct xgpu_hlsl_pixel_constants pixel_constants, sent_pixel_constants;
	unsigned long pixel_constants_serial;

	/* visibility tests: each slot's query (win32_d3d11_query_end) and the
	latest count it gave */
	unsigned int queries[VISIBILITY_TEST_SLOTS];
	float query_area[VISIBILITY_TEST_SLOTS];
	unsigned long visibility_known[VISIBILITY_TEST_SLOTS];
	BOOL query_pending[VISIBILITY_TEST_SLOTS];
	BOOL visibility_test_active;

	/* the vertices and indices a draw converts */
	unsigned char *scratch_vertices;
	unsigned long scratch_vertices_size;
	WORD *scratch_indices;
	unsigned long scratch_indices_size;

	unsigned long frame;
	unsigned long next_vertex_shader_id;
	BOOL ready;
	BOOL created;
};

static struct d3d11_device device;

static struct
{
	unsigned long draws, immediate_draws, clears, presents;
	unsigned long skipped_no_program, skipped_no_target, skipped_shader;
	unsigned long target_changes;
	unsigned long vertex_bytes;
} stats;

static D3DDevice *device_pointer(void)
{
	return (D3DDevice *)&device;
}

static float dword_to_float(DWORD value)
{
	union { DWORD d; float f; } u;

	u.d = value;
	return u.f;
}

static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

static struct
{
	const char *skip_vertex_shaders;
	const char *dump_shaders;
	BOOL statistics;
} debug_settings;

/* nothing caches GL state in this device (xgpu.h) */
void xgpu_gl_state_invalidate(void)
{
}

/* ---------- vertical blank emulation (as d3d8_gl.c) */

#define VERTICAL_BLANK_NANOSECONDS (1000000000L / 60)

static pthread_mutex_t vertical_blank_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vertical_blank_condition = PTHREAD_COND_INITIALIZER;
static D3DCALLBACK vertical_blank_callback;
static unsigned long vertical_blank_count;
static volatile unsigned int flip_count;
static unsigned long pending_flips;
static BOOL vertical_blank_thread_started = FALSE;

static void *vertical_blank_thread(void *unused)
{
	struct timespec next;

	(void)unused;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;)
	{
		D3DCALLBACK callback;

		next.tv_nsec += VERTICAL_BLANK_NANOSECONDS;
		if (next.tv_nsec >= 1000000000L)
		{
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		platform_sleep_until(&next);

		pthread_mutex_lock(&vertical_blank_lock);
		vertical_blank_count++;
		if (pending_flips)
		{
			pending_flips--;
			flip_count++;
		}
		callback = vertical_blank_callback;
		pthread_cond_broadcast(&vertical_blank_condition);
		pthread_mutex_unlock(&vertical_blank_lock);

		if (callback)
			callback(0);
	}
	return NULL;
}

static void vertical_blank_start(void)
{
	pthread_mutex_lock(&vertical_blank_lock);
	if (!vertical_blank_thread_started)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, vertical_blank_thread, NULL) == 0)
		{
			pthread_detach(thread);
			vertical_blank_thread_started = TRUE;
		}
		else
		{
			platform_log("cannot start the vertical blank thread");
		}
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	pthread_mutex_lock(&vertical_blank_lock);
	vertical_blank_callback = callback;
	pthread_mutex_unlock(&vertical_blank_lock);
	vertical_blank_start();
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	unsigned long count;

	vertical_blank_start();
	pthread_mutex_lock(&vertical_blank_lock);
	count = vertical_blank_count;
	while (vertical_blank_count == count)
		pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
	pthread_mutex_unlock(&vertical_blank_lock);
}

/* ---------- render targets */

static void surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height, BOOL *depth)
{
	struct xgpu_texture_description description;
	DWORD format;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	*width = description.width;
	*height = description.height;
	format = description.format;
	*depth = format == D3DFMT_D24S8 || format == D3DFMT_F24S8 || format == D3DFMT_D16 || format == D3DFMT_F16 ||
		format == D3DFMT_LIN_D24S8 || format == D3DFMT_LIN_F24S8 || format == D3DFMT_LIN_D16 || format == D3DFMT_LIN_F16;
}

static BOOL surface_is_shadow_map(const D3DSurface *surface)
{
	struct xgpu_texture_description description;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	return description.format == D3DFMT_R5G6B5 && description.width == SHADOW_MAP_SIZE &&
		description.height == SHADOW_MAP_SIZE;
}

#define RECENT_RENDER_TARGET_COUNT 4

static struct
{
	DWORD data, format, size;
	long screen_width;
	float scale[2];
	struct render_target_entry *entry;
} recent_render_targets[RECENT_RENDER_TARGET_COUNT];
static unsigned long recent_render_target_next;

static struct render_target_entry *render_target_remember(const D3DSurface *surface, long screen,
	struct render_target_entry *entry)
{
	unsigned long slot = recent_render_target_next++ % RECENT_RENDER_TARGET_COUNT;

	recent_render_targets[slot].data = surface->Data;
	recent_render_targets[slot].format = surface->Format;
	recent_render_targets[slot].size = surface->Size;
	recent_render_targets[slot].screen_width = screen;
	recent_render_targets[slot].scale[0] = screen_scale[0];
	recent_render_targets[slot].scale[1] = screen_scale[1];
	recent_render_targets[slot].entry = entry;
	return entry;
}

static struct render_target_entry *render_target_get(const D3DSurface *surface)
{
	struct render_target_entry *entry;
	unsigned long width, height, slot;
	float scale[2] = { 1.0f, 1.0f };
	long screen;
	BOOL depth;

	if (!surface || !surface->Data || !device.ready)
		return NULL;
	screen = halo_screen_width();
	for (slot = 0; slot < RECENT_RENDER_TARGET_COUNT; slot++)
	{
		if (recent_render_targets[slot].entry && recent_render_targets[slot].data == surface->Data &&
			recent_render_targets[slot].format == surface->Format && recent_render_targets[slot].size == surface->Size &&
			recent_render_targets[slot].screen_width == screen && recent_render_targets[slot].scale[0] == screen_scale[0] &&
			recent_render_targets[slot].scale[1] == screen_scale[1])
		{
			return recent_render_targets[slot].entry;
		}
	}
	surface_dimensions(surface, &width, &height, &depth);
	if (width == (unsigned long)halo_screen_width() && height == SCREEN_HEIGHT)
	{
		scale[0] = screen_scale[0];
		scale[1] = screen_scale[1];
	}
	else if (!depth && surface_is_shadow_map(surface))
	{
		scale[0] = (float)halo_shadow_map_scale();
		scale[1] = scale[0];
	}
	for (entry = *render_target_bucket(surface->Data); entry; entry = entry->next_in_bucket)
	{
		if (entry->target.data == surface->Data && entry->target.width == width &&
			entry->target.height == height && entry->target.depth == depth &&
			entry->target.scale[0] == scale[0] && entry->target.scale[1] == scale[1])
		{
			return render_target_remember(surface, screen, entry);
		}
	}
	entry = calloc(1, sizeof(*entry));
	entry->target.data = surface->Data;
	entry->target.width = width;
	entry->target.height = height;
	entry->target.depth = depth;
	entry->target.scale[0] = scale[0];
	entry->target.scale[1] = scale[1];
	entry->target.gl_width = (unsigned long)(width * scale[0] + 0.5f);
	entry->target.gl_height = (unsigned long)(height * scale[1] + 0.5f);
	if (maximum_target_size > 0 && entry->target.gl_width > (unsigned long)maximum_target_size)
		entry->target.gl_width = (unsigned long)maximum_target_size;
	if (maximum_target_size > 0 && entry->target.gl_height > (unsigned long)maximum_target_size)
		entry->target.gl_height = (unsigned long)maximum_target_size;
	entry->target.texture = win32_d3d11_target_create((unsigned int)entry->target.gl_width,
		(unsigned int)entry->target.gl_height, depth);
	entry->next = render_targets;
	render_targets = entry;
	entry->next_in_bucket = *render_target_bucket(entry->target.data);
	*render_target_bucket(entry->target.data) = entry;
	return entry;
}

struct xgpu_render_target *xgpu_render_target_find(unsigned long data)
{
	struct render_target_entry *entry, *best = NULL;

	for (entry = *render_target_bucket(data); entry; entry = entry->next_in_bucket)
	{
		if (entry->target.data == data && !entry->target.depth && entry->target.texture &&
			(!best || entry->last_rendered > best->last_rendered))
			best = entry;
	}
	return best ? &best->target : NULL;
}

static unsigned long render_target_write_serial;
static float target_scale[2] = { 1.0f, 1.0f };

static int scaled_pixel(float coordinate, float scale)
{
	float value = coordinate * scale + 0.5f;
	int pixel = (int)value;

	if ((float)pixel > value)
		pixel--;
	return pixel;
}

static int target_pixel(float coordinate, int axis)
{
	return scaled_pixel(coordinate, target_scale[axis]);
}

/* the current targets' ids; FALSE if there is nothing to draw into */
static BOOL bind_targets(unsigned int *color_id, unsigned int *depth_id)
{
	struct render_target_entry *color = render_target_get(device.render_target);
	struct render_target_entry *depth = render_target_get(device.depth_stencil);

	if (depth && !depth->target.depth)
		depth = NULL;
	if (color && !color->target.texture)
		color = NULL;
	if (depth && !depth->target.texture)
		depth = NULL;
	if (!color && !depth)
		return FALSE;
	if (color)
	{
		color->last_rendered = device.frame + 1;
		color->target.written = ++render_target_write_serial;
	}
	target_scale[0] = color ? color->target.scale[0] : depth->target.scale[0];
	target_scale[1] = color ? color->target.scale[1] : depth->target.scale[1];
	*color_id = color ? (unsigned int)color->target.texture : 0;
	*depth_id = depth ? (unsigned int)depth->target.texture : 0;
	return TRUE;
}

/* ---------- device creation */

static void d3d11_initialize(void)
{
	int index;

	maximum_target_size = win32_d3d11_maximum_texture_size();
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
		device.attributes[index][3] = 1.0f;
	memory_watch_initialize();
	debug_settings.skip_vertex_shaders = config_string("debug.gpu_skip_vertex_shaders");
	debug_settings.dump_shaders = *config_string("debug.gpu_dump_shaders") ?
		config_string("debug.gpu_dump_shaders") : NULL;
	debug_settings.statistics = config_boolean("debug.gpu_stats");
	device.vertex_constants_serial = device.pixel_constants_serial = 1;
	device.ready = TRUE;
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
}

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	(void)sdk_version;
	return (Direct3D *)1;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

static void constants_store(unsigned long first, const void *data, unsigned long count)
{
	memcpy(device.vertex_constants.c[first], data, count * sizeof(device.vertex_constants.c[0]));
}

static void viewport_update_constants(void)
{
	float zscale = 16777215.0f;

	if (device.depth_stencil)
	{
		struct xgpu_texture_description description;

		xgpu_texture_describe(device.depth_stencil->Format, device.depth_stencil->Size, &description);
		if (description.format == D3DFMT_D16 || description.format == D3DFMT_LIN_D16 ||
			description.format == D3DFMT_F16 || description.format == D3DFMT_LIN_F16)
		{
			zscale = 65535.0f;
		}
	}
	device.viewport_scale[0] = device.viewport.Width * 0.5f;
	device.viewport_scale[1] = -(float)device.viewport.Height * 0.5f;
	device.viewport_scale[2] = zscale * (device.viewport.MaxZ - device.viewport.MinZ);
	device.viewport_scale[3] = 0.0f;
	device.viewport_offset[0] = device.viewport.X + device.viewport.Width * 0.5f;
	device.viewport_offset[1] = device.viewport.Y + device.viewport.Height * 0.5f;
	device.viewport_offset[2] = zscale * device.viewport.MinZ;
	device.viewport_offset[3] = 0.0f;
	if (!(device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
	{
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 38, device.viewport_scale, 1);
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 37, device.viewport_offset, 1);
	}
}

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	unsigned long width, height;
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!device.created)
	{
		memset(&device, 0, sizeof(device));
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		width = device.presentation.BackBufferWidth ? device.presentation.BackBufferWidth : 640;
		height = device.presentation.BackBufferHeight ? device.presentation.BackBufferHeight : 480;
		d3d8_surface_initialize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, width, height);
		d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, width, height);
		device.render_target = &device.back_buffer;
		device.depth_stencil = &device.depth_buffer;
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MaxZ = 1.0f;
		device.next_vertex_shader_id = 1;
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
		D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
		D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
		D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
		D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILWRITEMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
		{
			D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MINFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MAXANISOTROPY] = 1;
		}
		viewport_update_constants();

		/* (the dedicated server and a probe draw nothing: server/src;
		platform_video_initialize makes the Direct3D 11 device with the
		window, sdl_platform.c) */
		if (!config_boolean("debug.null_renderer") &&
#ifdef HALO_GAME_BROWSER
			!browser_headless() &&
#endif
			platform_video_initialize(width, height))
			d3d11_initialize();
		else
			platform_log("Direct3D: running without a window (nothing is displayed)");
		device.created = TRUE;
	}
	*returned_device = device_pointer();
	return S_OK;
}

/* ---------- the menus' pointer (as d3d8_gl.c) */

static void ui_point_from_window(float window_x, float window_y, short *x, short *y)
{
	struct render_target_entry *back_buffer = render_target_get(&device.back_buffer);
	int window_width, window_height, pixel_width, pixel_height, width, height, left, top;
	float screen_x, screen_y;

	*x = *y = -1;
	if (!back_buffer)
		return;
	platform_video_window_size(&window_width, &window_height);
	platform_video_drawable_size(&pixel_width, &pixel_height);
	if (window_width <= 0 || window_height <= 0)
		return;
	width = pixel_width;
	height = (int)((long)pixel_width * back_buffer->target.gl_height / back_buffer->target.gl_width);
	if (height > pixel_height)
	{
		height = pixel_height;
		width = (int)((long)pixel_height * back_buffer->target.gl_width / back_buffer->target.gl_height);
	}
	left = (pixel_width - width) / 2;
	top = (pixel_height - height) / 2;
	screen_x = (window_x * pixel_width / window_width - left) * (float)back_buffer->target.width / (float)width;
	screen_y = (window_y * pixel_height / window_height - top) * (float)back_buffer->target.height / (float)height;
	*x = (short)floorf(screen_x - (float)(halo_screen_width() - 640) / 2.0f);
	*y = (short)floorf(screen_y);
}

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	struct platform_ui_pointer state;

	platform_menus_set_active(menus_active != 0);
	platform_ui_pointer_set_active(menus_active != 0);
	if (!menus_active || !device.ready || !platform_ui_pointer_read(&state))
		return 0;
	memset(pointer, 0, sizeof(*pointer));
	ui_point_from_window(state.x, state.y, &pointer->x, &pointer->y);
	ui_point_from_window(state.click_x, state.click_y, &pointer->click_x, &pointer->click_y);
	pointer->moved = state.moved != FALSE;
	pointer->left_clicks = (unsigned char)(state.left_clicks < 255 ? state.left_clicks : 255);
	pointer->right_clicks = (unsigned char)(state.right_clicks < 255 ? state.right_clicks : 255);
	pointer->wheel_steps = (signed char)(state.wheel_steps < -8 ? -8 : state.wheel_steps > 8 ? 8 : state.wheel_steps);
	pointer->touch = state.touch != FALSE;
	pointer->downs = (unsigned char)(state.downs < 255 ? state.downs : 255);
	ui_point_from_window(state.down_x, state.down_y, &pointer->down_x, &pointer->down_y);
	return 1;
}

long halo_screen_commit(void)
{
	long width;
	float scale[2];

	anti_aliasing_read();
	if (shadow_scale && shadow_scale_read_at != config_changes())
	{
		long shadow = shadow_scale_choose();

		shadow_scale_read_at = config_changes();
		if (shadow != shadow_scale)
		{
			platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow, SHADOW_MAP_SIZE * shadow);
			shadow_scale = shadow;
			memset(recent_render_targets, 0, sizeof(recent_render_targets));
		}
	}
	if (!screen_width)
		return halo_screen_width();
	screen_mode_choose(&width, scale);
	if (width != screen_width || scale[0] != screen_scale[0] || scale[1] != screen_scale[1])
	{
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", width, SCREEN_HEIGHT,
			width * scale[0], SCREEN_HEIGHT * scale[1]);
		screen_width = width;
		screen_scale[0] = scale[0];
		screen_scale[1] = scale[1];
		if (device.created)
		{
			device.presentation.BackBufferWidth = (UINT)width;
			d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, (unsigned long)width, SCREEN_HEIGHT);
			d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, (unsigned long)width, SCREEN_HEIGHT);
		}
	}
	return screen_width;
}

ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)back_buffer;
	(void)type;
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

static BOOL trace_frame(void);

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (trace_frame())
		platform_log("set render target %08lx depth %08lx", render_target ? (unsigned long)render_target->Data : 0,
			depth_stencil ? (unsigned long)depth_stencil->Data : 0);
	stats.target_changes++;
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
	if (device.render_target)
	{
		unsigned long width, height;
		BOOL depth;

		surface_dimensions(device.render_target, &width, &height, &depth);
		device.viewport.X = 0;
		device.viewport.Y = 0;
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MinZ = 0.0f;
		device.viewport.MaxZ = 1.0f;
	}
	viewport_update_constants();
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
	viewport_update_constants();
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = device.transforms[state];
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	device.shader_constant_mode = mode;
	viewport_update_constants();
}

/* ---------- GPU synchronisation: Direct3D keeps its own ordering */

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	if (callback)
		callback(context);
}

/* ---------- visibility (occlusion) tests

As d3d8_gl.c's reading of its queries: a slot's result is the latest
count the GPU has finished, the game told it is complete; a count is of the
game's pixels (visibility_unscaled). */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
	if (!device.ready || device.visibility_test_active)
		return;
	device.visibility_test_active = TRUE;
	win32_d3d11_query_begin();
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	unsigned int query;

	if (!device.ready || !device.visibility_test_active)
		return S_OK;
	device.visibility_test_active = FALSE;
	index %= VISIBILITY_TEST_SLOTS;
	query = win32_d3d11_query_end();
	if (!query)
		return S_OK;
	/* (an earlier test of the slot not yet read: this one replaces it) */
	if (device.query_pending[index])
		win32_d3d11_query_release(device.queries[index]);
	device.queries[index] = query;
	device.query_pending[index] = TRUE;
	device.query_area[index] = target_scale[0] * target_scale[1];
	return S_OK;
}

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	unsigned long samples;

	if (time_stamp)
		*time_stamp = 0;
	index %= VISIBILITY_TEST_SLOTS;
	if (device.ready && device.query_pending[index] && win32_d3d11_query_result(device.queries[index], &samples))
	{
		float area = device.query_area[index];

		device.visibility_known[index] = area > 1.0f ? (unsigned long)(samples / area + 0.5f) : samples;
		device.query_pending[index] = FALSE;
	}
	if (result)
		*result = (UINT)device.visibility_known[index];
	return S_OK;
}

/* ---------- render and texture stage state (as d3d8_gl.c) */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	(void)method;
	(void)value;
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value);

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if (state == D3DRS_ZBIAS)
		D3DDevice_SetRenderState_ZBias(value);
	else if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float offset = -(float)value;
	float slope = offset * 0.25f;
	DWORD enable = value != 0;

	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZSLOPESCALE], &slope, sizeof(slope));
	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET], &offset, sizeof(offset));
	D3D__RenderState[D3DRS_POINTOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_WIREFRAMEOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_SOLIDOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_ZBIAS] = value;
}

#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3D__RenderState[state] = value; }

COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(ZEnable, D3DRS_ZENABLE)
COMPLEX_RENDER_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_RENDER_STATE(StencilFail, D3DRS_STENCILFAIL)
COMPLEX_RENDER_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_RENDER_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_RENDER_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_RENDER_STATE(LogicOp, D3DRS_LOGICOP)
COMPLEX_RENDER_STATE(EdgeAntiAlias, D3DRS_EDGEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleAntiAlias, D3DRS_MULTISAMPLEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleMask, D3DRS_MULTISAMPLEMASK)
COMPLEX_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
COMPLEX_RENDER_STATE(ShadowFunc, D3DRS_SHADOWFUNC)
COMPLEX_RENDER_STATE(LineWidth, D3DRS_LINEWIDTH)
COMPLEX_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
COMPLEX_RENDER_STATE(YuvEnable, D3DRS_YUVENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.textures[stage] = texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.palettes[stage] = palette;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	if (!definition)
		return;
	memcpy(&D3D__RenderState[D3DRS_PSALPHAINPUTS0], definition->PSAlphaInputs, sizeof(definition->PSAlphaInputs));
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD] = definition->PSFinalCombinerInputsABCD;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG] = definition->PSFinalCombinerInputsEFG;
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT0_0], definition->PSConstant0, sizeof(definition->PSConstant0));
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT1_0], definition->PSConstant1, sizeof(definition->PSConstant1));
	memcpy(&D3D__RenderState[D3DRS_PSALPHAOUTPUTS0], definition->PSAlphaOutputs, sizeof(definition->PSAlphaOutputs));
	memcpy(&D3D__RenderState[D3DRS_PSRGBINPUTS0], definition->PSRGBInputs, sizeof(definition->PSRGBInputs));
	D3D__RenderState[D3DRS_PSCOMPAREMODE] = definition->PSCompareMode;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0] = definition->PSFinalCombinerConstant0;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1] = definition->PSFinalCombinerConstant1;
	memcpy(&D3D__RenderState[D3DRS_PSRGBOUTPUTS0], definition->PSRGBOutputs, sizeof(definition->PSRGBOutputs));
	D3D__RenderState[D3DRS_PSCOMBINERCOUNT] = definition->PSCombinerCount;
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = definition->PSTextureModes;
	D3D__RenderState[D3DRS_PSDOTMAPPING] = definition->PSDotMapping;
	D3D__RenderState[D3DRS_PSINPUTTEXTURE] = definition->PSInputTexture;
}

/* ---------- vertex shaders (as d3d8_gl.c) */

static unsigned long vertex_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: return 2;
	case D3DVSDT_SHORT2: return 4;
	case D3DVSDT_SHORT3: return 6;
	case D3DVSDT_SHORT4: return 8;
	case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	case D3DVSDT_FLOAT2H: return 12;
	default: return 0;
	}
}

static void parse_declaration(struct vertex_shader_object *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offsets[16] = { 0 };

	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;
		unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		switch (token_type)
		{
		case D3DVSD_TOKEN_STREAM:
			stream = token & D3DVSD_STREAMNUMBERMASK;
			break;
		case D3DVSD_TOKEN_STREAMDATA:
			if (token & D3DVSD_DATALOADTYPEMASK)
			{
				unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

				offsets[stream] += (token & 0x08000000) ? count : count * 4;
			}
			else if (object->element_count < XGPU_VERTEX_ATTRIBUTE_COUNT)
			{
				struct vertex_element *element = &object->elements[object->element_count++];

				element->reg = (unsigned char)(token & D3DVSD_VERTEXREGMASK);
				element->stream = (unsigned char)stream;
				element->type = (unsigned char)((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				element->bytes = (unsigned char)vertex_type_bytes(element->type);
				element->offset = (unsigned short)offsets[stream];
				offsets[stream] += element->bytes;
				if (element->type == D3DVSDT_NORMPACKED3)
					object->packed_mask |= 1UL << element->reg;
			}
			break;
		case D3DVSD_TOKEN_CONSTMEM:
			declaration += ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT) * 4;
			break;
		case D3DVSD_TOKEN_EXT:
			declaration += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
			break;
		default:
			break;
		}
	}
}

#ifdef HALO_64BIT
static struct vertex_shader_object **vertex_shader_handles;
static unsigned int vertex_shader_handle_count;

static DWORD vertex_shader_handle_new(struct vertex_shader_object *object)
{
	struct vertex_shader_object **handles = realloc(vertex_shader_handles,
		(vertex_shader_handle_count + 1) * sizeof(*handles));

	if (!handles)
		return 0;
	vertex_shader_handles = handles;
	vertex_shader_handles[vertex_shader_handle_count++] = object;
	return vertex_shader_handle_count * 2;
}

static struct vertex_shader_object *vertex_shader_handle_object(DWORD handle)
{
	if (!handle || (handle & 1) || handle / 2 > vertex_shader_handle_count)
		return NULL;
	return vertex_shader_handles[handle / 2 - 1];
}
#else
#define vertex_shader_handle_new(object) ((DWORD)(object))
#define vertex_shader_handle_object(handle) ((struct vertex_shader_object *)(handle))
#endif

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	object->id = device.next_vertex_shader_id++;
	if (function)
	{
		object->instruction_count = function[0] >> 16;
		object->instructions = malloc(object->instruction_count * 4 * sizeof(DWORD));
		memcpy(object->instructions, function + 1, object->instruction_count * 4 * sizeof(DWORD));
	}
	parse_declaration(object, declaration);
	*handle = vertex_shader_handle_new(object);
#ifdef HALO_64BIT
	if (!*handle)
		return E_OUTOFMEMORY;
#endif
	return S_OK;
}

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_handle_object(handle);

	if (!handle || (handle & 1) || !object || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

/* (per-pixel lighting is not drawn here: the programs are lit as they
light themselves) */
void halo_vertex_shader_lighting(unsigned long handle)
{
	(void)handle;
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
	{
		device.vertex_shader = object;
		device.program_address = 0;
		device.program_slots[0] = object;
	}
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_slots[address] = vertex_shader_from_handle(handle);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
		device.vertex_shader = object;
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_address = address;
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	*size = object ? object->instruction_count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + XGPU_VERTEX_CONSTANT_BIAS;

	if (first < 0 || first >= XGPU_VERTEX_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > XGPU_VERTEX_CONSTANT_COUNT)
		constant_count = XGPU_VERTEX_CONSTANT_COUNT - first;
	constants_store((unsigned long)first, constant_data, constant_count);
}

static struct vertex_shader_object *current_program(void)
{
	struct vertex_shader_object *program = device.program_slots[device.program_address];

	return program ? program : device.vertex_shader;
}

/* ---------- shaders */

static unsigned long hash_words(const void *data, unsigned long size)
{
	const DWORD *words = data;
	unsigned long hash = 2166136261UL;

	for (size /= 4; size; size--)
		hash = (hash ^ *words++) * 16777619UL;
	return hash;
}

static void shader_dump(const char *name, const char *text)
{
	char path[512];
	FILE *file;

	if (!debug_settings.dump_shaders)
		return;
	snprintf(path, sizeof(path), "%s/%s.hlsl", debug_settings.dump_shaders, name);
	if ((file = fopen(path, "w")) != NULL)
	{
		fputs(text, file);
		fclose(file);
	}
}

/* the program's shader for the input registers the draw's streams give
(0 if it does not compile) */
static unsigned int vertex_shader_get(struct vertex_shader_object *program, unsigned long input_mask)
{
	unsigned int index;
	char *text;
	char name[64];

	for (index = 0; index < program->variant_count; index++)
	{
		if (program->variants[index].input_mask == input_mask)
			return program->variants[index].shader;
	}
	/* (more masks than kept: the oldest made again) */
	if (program->variant_count == VERTEX_SHADER_VARIANTS)
	{
		memmove(&program->variants[0], &program->variants[1], (VERTEX_SHADER_VARIANTS - 1) * sizeof(program->variants[0]));
		program->variant_count--;
	}
	text = nv2a_vertex_shader_to_hlsl(program->instructions, program->instruction_count, input_mask);
	snprintf(name, sizeof(name), "vs%03lu_%04lx", program->id, input_mask);
	shader_dump(name, text);
	index = program->variant_count++;
	program->variants[index].input_mask = input_mask;
	program->variants[index].shader = win32_d3d11_vertex_shader(text, input_mask);
	program->variants[index].tried = TRUE;
	free(text);
	return program->variants[index].shader;
}

struct pixel_entry
{
	struct pixel_entry *next;
	unsigned long hash;
	struct nv2a_pixel_shader_key key;
	unsigned int shader;
};

#define PIXEL_BUCKETS 1024
static struct pixel_entry *pixel_buckets[PIXEL_BUCKETS];

typedef char pixel_shader_key_size_assert[sizeof(struct nv2a_pixel_shader_key) % 4 == 0 ? 1 : -1];

static unsigned int pixel_shader_get(const struct nv2a_pixel_shader_key *key)
{
#define RECENT_PIXEL_COUNT 4
	static struct pixel_entry *recent[RECENT_PIXEL_COUNT];
	static unsigned long recent_next;
	unsigned long hash, index;
	struct pixel_entry **bucket, *entry;
	char *text;

	for (index = 0; index < RECENT_PIXEL_COUNT; index++)
	{
		if (recent[index] && !memcmp(&recent[index]->key, key, sizeof(*key)))
			return recent[index]->shader;
	}
	hash = hash_words(key, sizeof(*key));
	bucket = &pixel_buckets[hash % PIXEL_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && !memcmp(&entry->key, key, sizeof(*key)))
		{
			recent[recent_next++ % RECENT_PIXEL_COUNT] = entry;
			return entry->shader;
		}
	}
	entry = calloc(1, sizeof(*entry));
	entry->hash = hash;
	entry->key = *key;
	text = nv2a_pixel_shader_to_hlsl(key);
	{
		char name[32];

		snprintf(name, sizeof(name), "ps_%08lx", hash);
		shader_dump(name, text);
	}
	entry->shader = win32_d3d11_pixel_shader(text);
	free(text);
	entry->next = *bucket;
	*bucket = entry;
	recent[recent_next++ % RECENT_PIXEL_COUNT] = entry;
	return entry->shader;
}

/* ---------- per-draw state */

static unsigned long stage_texture_mode(int stage)
{
	return (D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f;
}

static unsigned int address_mode(DWORD mode)
{
	switch (mode)
	{
	case D3DTADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
	case D3DTADDRESS_CLAMP: return GL_CLAMP_TO_EDGE;
	case D3DTADDRESS_BORDER: return GL_CLAMP_TO_BORDER;
	case D3DTADDRESS_CLAMPTOEDGE: return GL_CLAMP_TO_EDGE;
	default: return GL_REPEAT;
	}
}

/* a stage's sampler from its texture stage state (d3d8_gl.c
configure_sampler); hires: a high-res HUD texture, filtered from its levels */
static void sampler_describe(int stage, BOOL mipmapped, BOOL hires, struct win32_d3d11_sampler *sampler)
{
	DWORD *state = D3D__TextureState[stage];
	DWORD min_filter = hires ? D3DTEXF_LINEAR : state[D3DTSS_MINFILTER];
	DWORD mip_filter = hires ? D3DTEXF_LINEAR : mipmapped ? state[D3DTSS_MIPFILTER] : D3DTEXF_NONE;
	DWORD mag_filter = hires ? D3DTEXF_LINEAR : state[D3DTSS_MAGFILTER];

	sampler->minification = min_filter == D3DTEXF_POINT ? GL_NEAREST : GL_LINEAR;
	sampler->magnification = mag_filter == D3DTEXF_POINT ? GL_NEAREST : GL_LINEAR;
	sampler->mip = mip_filter == D3DTEXF_NONE ? 0 : mip_filter == D3DTEXF_POINT ? GL_NEAREST : GL_LINEAR;
	sampler->address[0] = address_mode(state[D3DTSS_ADDRESSU]);
	sampler->address[1] = address_mode(state[D3DTSS_ADDRESSV]);
	sampler->address[2] = address_mode(state[D3DTSS_ADDRESSW]);
	sampler->lod_bias = hires ? 0.0f : dword_to_float(state[D3DTSS_MIPMAPLODBIAS]);
	sampler->minimum_lod = hires ? 0.0f : (float)state[D3DTSS_MAXMIPLEVEL];
	sampler->anisotropy = min_filter == D3DTEXF_ANISOTROPIC && state[D3DTSS_MAXANISOTROPY] > 1 ?
		state[D3DTSS_MAXANISOTROPY] : 1;
	color_to_vec4(state[D3DTSS_BORDERCOLOR], sampler->border);
}

/* ---------- render targets sampled with their mip chain (as d3d8_gl.c's
mip_composite: the levels' targets copied into one texture, again only
once a level has been drawn into since) */

#define MIP_COMPOSITE_LEVELS 16

struct mip_composite
{
	struct mip_composite *next;
	unsigned long data, width, height, levels;
	unsigned int id;
	unsigned long rendered_levels;
	unsigned long level_sources[MIP_COMPOSITE_LEVELS];
	unsigned long level_written[MIP_COMPOSITE_LEVELS];
};

static struct mip_composite *mip_composites;

static unsigned int mip_composite_get(const struct xgpu_texture_description *description, unsigned long data)
{
	struct mip_composite *composite;
	struct xgpu_render_target *targets[MIP_COMPOSITE_LEVELS];
	unsigned int ids[MIP_COMPOSITE_LEVELS];
	unsigned long level, rendered_levels = 0;
	BOOL changed;

	for (composite = mip_composites; composite; composite = composite->next)
	{
		if (composite->data == data && composite->width == description->width &&
			composite->height == description->height && composite->levels == description->levels)
			break;
	}
	if (!composite)
	{
		composite = calloc(1, sizeof(*composite));
		composite->data = data;
		composite->width = description->width;
		composite->height = description->height;
		composite->levels = description->levels;
		composite->rendered_levels = ~0UL;
		composite->next = mip_composites;
		mip_composites = composite;
	}
	for (level = 0; level < description->levels && level < MIP_COMPOSITE_LEVELS; level++)
	{
		unsigned long width = description->width >> level ? description->width >> level : 1;
		unsigned long height = description->height >> level ? description->height >> level : 1;
		struct xgpu_render_target *target =
			xgpu_render_target_find(data + xgpu_texture_level_offset(description, level));

		if (!target || target->width != width || target->height != height ||
			target->gl_width != width || target->gl_height != height)
			break;
		targets[level] = target;
		ids[level] = (unsigned int)target->texture;
		rendered_levels++;
	}
	changed = rendered_levels != composite->rendered_levels;
	for (level = 0; level < rendered_levels && !changed; level++)
	{
		changed = targets[level]->texture != composite->level_sources[level] ||
			targets[level]->written != composite->level_written[level];
	}
	if (!changed)
		return composite->id;
	composite->rendered_levels = rendered_levels;
	for (level = 0; level < rendered_levels; level++)
	{
		composite->level_sources[level] = targets[level]->texture;
		composite->level_written[level] = targets[level]->written;
	}
	composite->id = win32_d3d11_target_mipmaps(data, ids, (int)rendered_levels, (int)description->levels);
	return composite->id;
}

/* each stage's texture into the draw, and what the pixel shader needs to
know of it into the key */
static void bind_textures(struct nv2a_pixel_shader_key *key, struct win32_d3d11_draw *draw, float texture_scale[4][4])
{
	int stage;

	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		D3DBaseTexture *texture = device.textures[stage];
		unsigned long mode = stage_texture_mode(stage);
		struct xgpu_render_target *target;
		struct xgpu_texture_description description;

		texture_scale[stage][0] = texture_scale[stage][1] = 1.0f;
		texture_scale[stage][2] = texture_scale[stage][3] = 1.0f;
		draw->texture_kind[stage] = _win32_d3d11_texture_none;
		draw->texture[stage] = 0;
		if (!texture || !texture->Data || mode == 0 || mode == 0x04 || mode == 0x05 || mode == 0x11)
		{
			key->sampler_type[stage] = mode == 0x11 ? _xgpu_sampler_2d : _xgpu_sampler_none;
			continue;
		}
		target = xgpu_render_target_find(texture->Data);
		if (target)
		{
			xgpu_texture_describe(texture->Format, texture->Size, &description);
			draw->texture_kind[stage] = _win32_d3d11_texture_target;
			draw->texture[stage] = (unsigned int)target->texture;
			if (description.linear)
			{
				texture_scale[stage][0] = 1.0f / (float)target->width;
				texture_scale[stage][1] = 1.0f / (float)target->height;
			}
			if (!description.linear && !description.cube_map && description.levels > 1 &&
				target->width == description.width && target->height == description.height)
			{
				unsigned int id = mip_composite_get(&description, texture->Data);

				if (id)
				{
					draw->texture_kind[stage] = _win32_d3d11_texture_mipmaps;
					draw->texture[stage] = id;
				}
				else
				{
					description.levels = 1;
				}
			}
			else
			{
				description.levels = 1;
			}
			key->sampler_type[stage] = _xgpu_sampler_2d;
		}
		else
		{
			const D3DCOLOR *palette = device.palettes[stage] && device.palettes[stage]->Data ?
				(const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(device.palettes[stage]->Data) : NULL;
			GLenum gl_target;
			unsigned int name = xgpu_texture_get((const DWORD *)texture, palette, &gl_target, &description);
			int channels = 0, levels = 0, kind = name ? win32_d3d11_texture_describe(name, &channels, &levels) : 0;

			if (!kind)
			{
				key->sampler_type[stage] = _xgpu_sampler_none;
				continue;
			}
			draw->texture_kind[stage] = _win32_d3d11_texture_gl;
			draw->texture[stage] = name;
			key->channels[stage] = (unsigned char)channels;
			key->sampler_type[stage] = (unsigned char)kind;
			if (description.linear)
			{
				texture_scale[stage][0] = 1.0f / (float)description.width;
				texture_scale[stage][1] = 1.0f / (float)description.height;
			}
		}
		sampler_describe(stage, description.levels > 1, description.hires, &draw->samplers[stage]);
		if (stage == 0)
			key->coverage_alpha = description.hires_coverage != FALSE;
	}
}

static unsigned int blend_equation(DWORD operation)
{
	switch (operation)
	{
	case D3DBLENDOP_SUBTRACT: return GL_FUNC_SUBTRACT;
	case D3DBLENDOP_REVSUBTRACT:
	case D3DBLENDOP_REVSUBTRACTSIGNED: return GL_FUNC_REVERSE_SUBTRACT;
	case D3DBLENDOP_MIN: return GL_MIN;
	case D3DBLENDOP_MAX: return GL_MAX;
	default: return GL_FUNC_ADD;
	}
}

/* the render states (d3d8_gl.c apply_raster_state) into the draw */
static void raster_state(BOOL has_depth, struct win32_d3d11_draw *draw)
{
	DWORD *rs = D3D__RenderState;
	DWORD write = rs[D3DRS_COLORWRITEENABLE];
	int x0 = target_pixel((float)device.viewport.X, 0);
	int y0 = target_pixel((float)device.viewport.Y, 1);
	int x1 = target_pixel((float)(device.viewport.X + device.viewport.Width), 0);
	int y1 = target_pixel((float)(device.viewport.Y + device.viewport.Height), 1);

	draw->viewport[0] = (float)x0;
	draw->viewport[1] = (float)y0;
	draw->viewport[2] = (float)(x1 > x0 ? x1 - x0 : 0);
	draw->viewport[3] = (float)(y1 > y0 ? y1 - y0 : 0);
	/* (the NV2A's scissor follows the viewport: split-screen windows) */
	draw->scissor[0] = x0;
	draw->scissor[1] = y0;
	draw->scissor[2] = x1;
	draw->scissor[3] = y1;
	draw->depth_range[0] = device.viewport.MinZ;
	draw->depth_range[1] = device.viewport.MaxZ;

	draw->depth_function = has_depth && rs[D3DRS_ZENABLE] ? (rs[D3DRS_ZFUNC] ? rs[D3DRS_ZFUNC] : GL_NEVER) : 0;
	draw->depth_write = draw->depth_function && rs[D3DRS_ZWRITEENABLE];
	draw->stencil_function = has_depth && rs[D3DRS_STENCILENABLE] ?
		(rs[D3DRS_STENCILFUNC] ? rs[D3DRS_STENCILFUNC] : GL_NEVER) : 0;
	draw->stencil_reference = rs[D3DRS_STENCILREF];
	draw->stencil_read_mask = rs[D3DRS_STENCILMASK];
	draw->stencil_write_mask = rs[D3DRS_STENCILWRITEMASK];
	/* (the Xbox's stencil operations are GL's, with 0 for zero) */
	draw->stencil_fail = rs[D3DRS_STENCILFAIL];
	draw->stencil_depth_fail = rs[D3DRS_STENCILZFAIL];
	draw->stencil_pass = rs[D3DRS_STENCILPASS];

	if (rs[D3DRS_ALPHABLENDENABLE])
	{
		draw->blend_source = rs[D3DRS_SRCBLEND];
		draw->blend_destination = rs[D3DRS_DESTBLEND];
		draw->blend_equation = blend_equation(rs[D3DRS_BLENDOP]);
		color_to_vec4(rs[D3DRS_BLENDCOLOR], draw->blend_factor);
	}
	else
	{
		draw->blend_equation = 0;
	}
	draw->color_mask = ((write & D3DCOLORWRITEENABLE_RED) ? 1 : 0) | ((write & D3DCOLORWRITEENABLE_GREEN) ? 2 : 0) |
		((write & D3DCOLORWRITEENABLE_BLUE) ? 4 : 0) | ((write & D3DCOLORWRITEENABLE_ALPHA) ? 8 : 0);

	/* the cull mode names the winding to discard, FRONTFACE the front one,
	both on the screen as Direct3D 11 has them */
	if (rs[D3DRS_CULLMODE] != D3DCULL_NONE)
	{
		draw->front_counter_clockwise = rs[D3DRS_FRONTFACE] == D3DFRONT_CCW;
		draw->cull = rs[D3DRS_CULLMODE] == rs[D3DRS_FRONTFACE] ? GL_FRONT : GL_BACK;
	}
	else
	{
		draw->cull = 0;
		draw->front_counter_clockwise = FALSE;
	}
	draw->wireframe = rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME || rs[D3DRS_FILLMODE] == D3DFILL_POINT;
	if (rs[D3DRS_SOLIDOFFSETENABLE])
	{
		draw->slope_scaled_depth_bias = dword_to_float(rs[D3DRS_POLYGONOFFSETZSLOPESCALE]);
		draw->depth_bias = dword_to_float(rs[D3DRS_POLYGONOFFSETZOFFSET]);
	}
	else
	{
		draw->slope_scaled_depth_bias = draw->depth_bias = 0.0f;
	}
}

/* the vertex shader's constants beyond c[] (the attributes the streams do
not give: their current values), sent with a new serial when they change */
static void vertex_constants_update(struct win32_d3d11_draw *draw)
{
	struct xgpu_hlsl_vertex_constants *constants = &device.vertex_constants;

	memcpy(constants->viewport_scale, device.viewport_scale, sizeof(constants->viewport_scale));
	memcpy(constants->viewport_offset, device.viewport_offset, sizeof(constants->viewport_offset));
	memcpy(constants->attributes, device.attributes, sizeof(constants->attributes));
	constants->point_size = D3D__RenderState[D3DRS_POINTSIZE] ? dword_to_float(D3D__RenderState[D3DRS_POINTSIZE]) : 1.0f;
	constants->screen_offset = (float)UI_OFFSET;
	constants->zero = 0.0f;
	constants->padding = 0.0f;
	if (memcmp(constants, &device.sent_vertex_constants, sizeof(*constants)))
	{
		device.sent_vertex_constants = *constants;
		device.vertex_constants_serial++;
	}
	draw->vertex_constants = &device.sent_vertex_constants;
	draw->vertex_constants_size = sizeof(device.sent_vertex_constants);
	draw->vertex_constants_serial = device.vertex_constants_serial;
}

static void pixel_constants_update(struct win32_d3d11_draw *draw, float texture_scale[4][4])
{
	struct xgpu_hlsl_pixel_constants *constants = &device.pixel_constants;
	int stage;

	memset(constants, 0, sizeof(*constants));
	for (stage = 0; stage < 8; stage++)
	{
		color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage], constants->ps_c0[stage]);
		color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage], constants->ps_c1[stage]);
	}
	color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0], constants->ps_final_c0);
	color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1], constants->ps_final_c1);
	color_to_vec4(D3D__RenderState[D3DRS_FOGCOLOR], constants->fog_color);
	constants->fog_parameters[0] = dword_to_float(D3D__RenderState[D3DRS_FOGSTART]);
	constants->fog_parameters[1] = dword_to_float(D3D__RenderState[D3DRS_FOGEND]);
	constants->fog_parameters[2] = dword_to_float(D3D__RenderState[D3DRS_FOGDENSITY]);
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		DWORD *state = D3D__TextureState[stage];

		constants->bump_matrix[stage][0] = dword_to_float(state[D3DTSS_BUMPENVMAT00]);
		constants->bump_matrix[stage][1] = dword_to_float(state[D3DTSS_BUMPENVMAT01]);
		constants->bump_matrix[stage][2] = dword_to_float(state[D3DTSS_BUMPENVMAT10]);
		constants->bump_matrix[stage][3] = dword_to_float(state[D3DTSS_BUMPENVMAT11]);
		constants->bump_luminance[stage][0] = dword_to_float(state[D3DTSS_BUMPENVLSCALE]);
		constants->bump_luminance[stage][1] = dword_to_float(state[D3DTSS_BUMPENVLOFFSET]);
	}
	memcpy(constants->texture_scale, texture_scale, sizeof(constants->texture_scale));
	constants->alpha_reference = (float)(D3D__RenderState[D3DRS_ALPHAREF] & 0xff);
	if (memcmp(constants, &device.sent_pixel_constants, sizeof(*constants)))
	{
		device.sent_pixel_constants = *constants;
		device.pixel_constants_serial++;
	}
	draw->pixel_constants = &device.sent_pixel_constants;
	draw->pixel_constants_size = sizeof(device.sent_pixel_constants);
	draw->pixel_constants_serial = device.pixel_constants_serial;
}

/* everything of the draw but its vertices: FALSE if it is not drawn */
static BOOL prepare_draw(unsigned long input_mask, struct win32_d3d11_draw *draw)
{
	struct vertex_shader_object *program = current_program();
	struct nv2a_pixel_shader_key key;
	float texture_scale[4][4];
	int stage;

	if (!device.ready || !program || !device.vertex_shader || !program->instructions)
	{
		stats.skipped_no_program++;
		return FALSE;
	}
	{
		const char *skip = debug_settings.skip_vertex_shaders;

		while (skip && *skip)
		{
			if ((unsigned long)atol(skip) == program->id)
				return FALSE;
			skip = strchr(skip, ',');
			if (skip)
				skip++;
		}
	}
	memset(draw, 0, sizeof(*draw));
	memset(&key, 0, sizeof(key));
	memcpy(key.combiner_state, D3D__RenderState, sizeof(key.combiner_state));
	memset(&key.combiner_state[D3DRS_PSCONSTANT0_0], 0, 16 * sizeof(DWORD));
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0;
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
	key.texture_modes = D3D__RenderState[D3DRS_PSTEXTUREMODES];
	bind_textures(&key, draw, texture_scale);
	if (!bind_targets(&draw->color_target, &draw->depth_target))
	{
		stats.skipped_no_target++;
		return FALSE;
	}
	raster_state(draw->depth_target != 0, draw);
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		key.alpha_kill[stage] = D3D__TextureState[stage][D3DTSS_ALPHAKILL] == D3DTALPHAKILL_ENABLE;
		key.color_sign[stage] = (unsigned char)((D3D__TextureState[stage][D3DTSS_COLORSIGN] >> 28) & 0xf);
	}
	key.coverage_alpha = key.coverage_alpha && D3D__RenderState[D3DRS_ALPHABLENDENABLE] &&
		D3D__RenderState[D3DRS_SRCBLEND] == D3DBLEND_CONSTANTCOLOR &&
		D3D__RenderState[D3DRS_DESTBLEND] == D3DBLEND_SRCALPHA;
	key.alpha_test_function = D3D__RenderState[D3DRS_ALPHATESTENABLE] ? D3D__RenderState[D3DRS_ALPHAFUNC] : 0;
	key.fog_enable = D3D__RenderState[D3DRS_FOGENABLE] != 0;
	key.fog_table_mode = (unsigned char)D3D__RenderState[D3DRS_FOGTABLEMODE];

	draw->vertex_shader = vertex_shader_get(program, input_mask);
	draw->pixel_shader = pixel_shader_get(&key);
	if (!draw->vertex_shader || !draw->pixel_shader)
	{
		stats.skipped_shader++;
		return FALSE;
	}
	vertex_constants_update(draw);
	pixel_constants_update(draw, texture_scale);
	return TRUE;
}

/* ---------- tracing (debug.gpu_trace_frame) */

static BOOL trace_frame(void)
{
	static long frame = -2;

	if (frame == -2)
		frame = config_integer("debug.gpu_trace_frame");
	return frame >= 0 && device.frame == (unsigned long)frame;
}

static void trace_draw(const char *kind, D3DPRIMITIVETYPE type, unsigned long count)
{
	struct vertex_shader_object *program = current_program();
	DWORD *rs = D3D__RenderState;

	if (!trace_frame())
		return;
	platform_log("%s type %d count %lu vs %lu (decl %lu) vp %lu,%lu %lux%lu z%.2f-%.2f zen %lu zw %lu zf %lx blend %lu %lx/%lx cull %lx cw %08lx tm %05lx cc %lx fin %08lx/%08lx at %lu/%lx",
		kind, type, count, program ? program->id : 0, device.vertex_shader ? device.vertex_shader->id : 0,
		device.viewport.X, device.viewport.Y, device.viewport.Width, device.viewport.Height,
		device.viewport.MinZ, device.viewport.MaxZ, rs[D3DRS_ZENABLE], rs[D3DRS_ZWRITEENABLE], rs[D3DRS_ZFUNC],
		rs[D3DRS_ALPHABLENDENABLE], rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], rs[D3DRS_CULLMODE],
		rs[D3DRS_COLORWRITEENABLE], rs[D3DRS_PSTEXTUREMODES], rs[D3DRS_PSCOMBINERCOUNT],
		rs[D3DRS_PSFINALCOMBINERINPUTSABCD], rs[D3DRS_PSFINALCOMBINERINPUTSEFG],
		rs[D3DRS_ALPHATESTENABLE], rs[D3DRS_ALPHAFUNC]);
}

/* ---------- vertex data

Each draw's vertices are read from guest memory and converted: every input
register a stream gives, four floats, one after another in register order
(the vertex shader's input: nv2a_vertex_shader_to_hlsl). Each element as
OpenGL reads it (d3d8_gl.c attribute_format): missing components 0, 0, 1;
normalized types to [0, 1] or [-1, 1]. */

static void *scratch_vertices(unsigned long size)
{
	if (device.scratch_vertices_size < size)
	{
		free(device.scratch_vertices);
		device.scratch_vertices_size = size + 65536;
		device.scratch_vertices = malloc(device.scratch_vertices_size);
		if (!device.scratch_vertices)
			device.scratch_vertices_size = 0;
	}
	return device.scratch_vertices;
}

static WORD *scratch_indices(unsigned long count)
{
	if (device.scratch_indices_size < count)
	{
		free(device.scratch_indices);
		device.scratch_indices_size = count + 4096;
		device.scratch_indices = malloc(device.scratch_indices_size * sizeof(WORD));
		if (!device.scratch_indices)
			device.scratch_indices_size = 0;
	}
	return device.scratch_indices;
}

static void element_convert(unsigned long type, const unsigned char *source, float *out)
{
	const short *shorts = (const short *)source;
	int index;

	out[0] = out[1] = out[2] = 0.0f;
	out[3] = 1.0f;
	switch (type)
	{
	case D3DVSDT_FLOAT1: memcpy(out, source, 4); break;
	case D3DVSDT_FLOAT2: memcpy(out, source, 8); break;
	case D3DVSDT_FLOAT3:
	case D3DVSDT_FLOAT2H: memcpy(out, source, 12); break;
	case D3DVSDT_FLOAT4: memcpy(out, source, 16); break;
	case D3DVSDT_D3DCOLOR:
		/* (B, G, R, A in memory) */
		out[0] = source[2] / 255.0f;
		out[1] = source[1] / 255.0f;
		out[2] = source[0] / 255.0f;
		out[3] = source[3] / 255.0f;
		break;
	case D3DVSDT_SHORT1: case D3DVSDT_SHORT2: case D3DVSDT_SHORT3: case D3DVSDT_SHORT4:
		for (index = 0; index <= (int)(type == D3DVSDT_SHORT1 ? 0 : type == D3DVSDT_SHORT2 ? 1 :
			type == D3DVSDT_SHORT3 ? 2 : 3); index++)
		{
			out[index] = (float)shorts[index];
		}
		break;
	case D3DVSDT_NORMSHORT1: case D3DVSDT_NORMSHORT2: case D3DVSDT_NORMSHORT3: case D3DVSDT_NORMSHORT4:
		for (index = 0; index <= (int)(type == D3DVSDT_NORMSHORT1 ? 0 : type == D3DVSDT_NORMSHORT2 ? 1 :
			type == D3DVSDT_NORMSHORT3 ? 2 : 3); index++)
		{
			float value = shorts[index] / 32767.0f;

			out[index] = value < -1.0f ? -1.0f : value;
		}
		break;
	case D3DVSDT_NORMPACKED3:
	{
		/* 11, 11 and 10 signed bits (nv2a_vsh.c unpack_normpacked3) */
		unsigned long packed = (unsigned long)source[0] | ((unsigned long)source[1] << 8) |
			((unsigned long)source[2] << 16) | ((unsigned long)source[3] << 24);
		long x = (long)(packed & 0x7ff), y = (long)((packed >> 11) & 0x7ff), z = (long)(packed >> 22);

		if (x & 0x400)
			x -= 0x800;
		if (y & 0x400)
			y -= 0x800;
		if (z & 0x200)
			z -= 0x400;
		out[0] = x / 1023.0f;
		out[1] = y / 1023.0f;
		out[2] = z / 511.0f;
		break;
	}
	case D3DVSDT_PBYTE1: case D3DVSDT_PBYTE2: case D3DVSDT_PBYTE3: case D3DVSDT_PBYTE4:
		for (index = 0; index <= (int)(type == D3DVSDT_PBYTE1 ? 0 : type == D3DVSDT_PBYTE2 ? 1 :
			type == D3DVSDT_PBYTE3 ? 2 : 3); index++)
		{
			out[index] = source[index] / 255.0f;
		}
		break;
	default:
		break;
	}
}

/* the input registers the declaration's streams give */
static unsigned long stream_input_mask(void)
{
	const struct vertex_shader_object *declaration = device.vertex_shader;
	unsigned long index, mask = 0;

	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];

		if (device.streams[element->stream].data && element->type != D3DVSDT_NONE)
			mask |= 1UL << element->reg;
	}
	return mask;
}

/* vertices [first, first + count) of the streams, converted, into the draw */
static BOOL setup_streams(unsigned long input_mask, unsigned long first, unsigned long count,
	struct win32_d3d11_draw *draw)
{
	const struct vertex_shader_object *declaration = device.vertex_shader;
	unsigned long slots[XGPU_VERTEX_ATTRIBUTE_COUNT], slot_count = 0, index, vertex, stride;
	float *out;

	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (input_mask & (1UL << index))
			slots[index] = slot_count++;
	}
	if (!slot_count)
		return FALSE;
	stride = slot_count * 4 * sizeof(float);
	out = scratch_vertices(count * stride);
	if (!out)
		return FALSE;
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];
		unsigned long source_stride = device.streams[element->stream].stride;
		const unsigned char *base;

		if (!device.streams[element->stream].data || element->type == D3DVSDT_NONE)
			continue;
		base = (const unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[element->stream].data) + element->offset;
		for (vertex = 0; vertex < count; vertex++)
		{
			element_convert(element->type, base + (first + vertex) * source_stride,
				out + vertex * slot_count * 4 + slots[element->reg] * 4);
		}
	}
	draw->vertices = out;
	draw->vertex_count = (unsigned int)count;
	draw->stride = (unsigned int)stride;
	stats.vertex_bytes += count * stride;
	return TRUE;
}

/* the draw's primitives as Direct3D 11 has them: the indices of quads,
fans, polygons and line loops (in the vertices' own numbering, or count
vertices from 0 where indices is NULL) as lists; the rest as they are */
static void primitives(D3DPRIMITIVETYPE type, const WORD *indices, unsigned long count, struct win32_d3d11_draw *draw)
{
	unsigned long index, out_count = 0;
	WORD *out = NULL;

#define VERTEX(n) (indices ? indices[n] : (WORD)(n))
	switch (type)
	{
	case D3DPT_QUADLIST:
		out = scratch_indices(count / 4 * 6);
		for (index = 0; out && index + 3 < count; index += 4)
		{
			out[out_count++] = VERTEX(index);
			out[out_count++] = VERTEX(index + 1);
			out[out_count++] = VERTEX(index + 2);
			out[out_count++] = VERTEX(index);
			out[out_count++] = VERTEX(index + 2);
			out[out_count++] = VERTEX(index + 3);
		}
		draw->topology = _win32_d3d11_triangles;
		break;
	case D3DPT_TRIANGLEFAN:
	case D3DPT_POLYGON:
		out = scratch_indices(count > 2 ? (count - 2) * 3 : 0);
		for (index = 1; out && index + 1 < count; index++)
		{
			out[out_count++] = VERTEX(0);
			out[out_count++] = VERTEX(index);
			out[out_count++] = VERTEX(index + 1);
		}
		draw->topology = _win32_d3d11_triangles;
		break;
	case D3DPT_LINELOOP:
		out = scratch_indices(count + 1);
		for (index = 0; out && index < count; index++)
			out[out_count++] = VERTEX(index);
		if (out && count)
			out[out_count++] = VERTEX(0);
		draw->topology = _win32_d3d11_line_strip;
		break;
	default:
		draw->topology = type == D3DPT_POINTLIST ? _win32_d3d11_points : type == D3DPT_LINELIST ? _win32_d3d11_lines :
			type == D3DPT_LINESTRIP ? _win32_d3d11_line_strip :
			type == D3DPT_TRIANGLESTRIP || type == D3DPT_QUADSTRIP ? _win32_d3d11_triangle_strip : _win32_d3d11_triangles;
		if (indices)
		{
			draw->indices = indices;
			draw->index_count = (unsigned int)count;
		}
		return;
	}
#undef VERTEX
	draw->indices = out;
	draw->index_count = (unsigned int)out_count;
}

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number >= 16)
		return;
	device.streams[stream_number].data = stream_data ? stream_data->Data : 0;
	device.streams[stream_number].stride = stride;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	device.base_vertex_index = base_vertex_index;
	D3D__IndexData = index_data ? (WORD *)xbox_pointer(index_data->Data) : NULL;
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT vertex_count)
{
	struct win32_d3d11_draw draw;
	unsigned long input_mask;

	if (!vertex_count || !device.ready || !device.vertex_shader)
		return;
	input_mask = stream_input_mask();
	if (!prepare_draw(input_mask, &draw))
		return;
	trace_draw("draw", primitive_type, vertex_count);
	if (!setup_streams(input_mask, start_vertex, vertex_count, &draw))
		return;
	primitives(primitive_type, NULL, vertex_count, &draw);
	if ((draw.indices && !draw.index_count) || (!draw.indices && primitive_type == D3DPT_QUADLIST))
		return;
	stats.draws++;
	win32_d3d11_draw(&draw);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	struct win32_d3d11_draw draw;
	unsigned long minimum = 0xffff, maximum = 0, index, input_mask;

	if (!vertex_count || !index_data || !device.ready || !device.vertex_shader)
		return;
	input_mask = stream_input_mask();
	if (!prepare_draw(input_mask, &draw))
		return;
	trace_draw("indexed", primitive_type, vertex_count);
	for (index = 0; index < vertex_count; index++)
	{
		if (index_data[index] < minimum)
			minimum = index_data[index];
		if (index_data[index] > maximum)
			maximum = index_data[index];
	}
	/* (the streams from the base vertex on: index i is vertex base + i;
	the converted vertices start at the smallest index) */
	if (!setup_streams(input_mask, device.base_vertex_index + minimum, maximum - minimum + 1, &draw))
		return;
	primitives(primitive_type, index_data, vertex_count, &draw);
	if (!draw.index_count)
		return;
	draw.base_vertex = -(int)minimum;
	stats.draws++;
	win32_d3d11_draw(&draw);
}

/* ---------- immediate mode */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	device.immediate_active = TRUE;
	device.immediate_type = primitive_type;
	device.immediate_count = 0;
}

static void immediate_emit(void)
{
	unsigned long floats = XGPU_VERTEX_ATTRIBUTE_COUNT * 4;

	if (device.immediate_count == device.immediate_capacity)
	{
		device.immediate_capacity = device.immediate_capacity ? device.immediate_capacity * 2 : 256;
		device.immediate_vertices = realloc(device.immediate_vertices,
			device.immediate_capacity * floats * sizeof(float));
	}
	memcpy(device.immediate_vertices + device.immediate_count * floats, device.attributes, floats * sizeof(float));
	device.immediate_count++;
}

void WINAPI D3DDevice_End(void)
{
	struct win32_d3d11_draw draw;
	unsigned long count = device.immediate_count;

	device.immediate_active = FALSE;
	/* (every register, four floats: the vertices are already as the shader
	reads them) */
	if (!count || !prepare_draw(0xffff, &draw))
		return;
	trace_draw("immediate", device.immediate_type, count);
	draw.vertices = device.immediate_vertices;
	draw.vertex_count = (unsigned int)count;
	draw.stride = XGPU_VERTEX_ATTRIBUTE_COUNT * 4 * sizeof(float);
	primitives(device.immediate_type, NULL, count, &draw);
	if (draw.indices && !draw.index_count)
		return;
	stats.immediate_draws++;
	win32_d3d11_draw(&draw);
}

static void set_attribute(INT reg, float a, float b, float c, float d)
{
	BOOL emit = FALSE;

	if (reg == D3DVSDE_VERTEX)
	{
		reg = 0;
		emit = TRUE;
	}
	if (reg < 0 || reg >= XGPU_VERTEX_ATTRIBUTE_COUNT)
		return;
	device.attributes[reg][0] = a;
	device.attributes[reg][1] = b;
	device.attributes[reg][2] = c;
	device.attributes[reg][3] = d;
	if (device.immediate_active && (emit || reg == 0))
		immediate_emit();
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	set_attribute(reg, a, b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	set_attribute(reg, a, b, c, d);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	set_attribute(reg, (float)a, (float)b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	set_attribute(reg, a / 255.0f, b / 255.0f, c / 255.0f, d / 255.0f);
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	float value[4];

	color_to_vec4(color, value);
	set_attribute(reg, value[0], value[1], value[2], value[3]);
}

/* ---------- clearing (as d3d8_gl.c: the viewport's rectangle, or each
rectangle within it, and only the channels named) */

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	unsigned int color_target = 0, depth_target = 0, color_mask;
	float rgba[4];
	DWORD index;

	if (!device.ready || !bind_targets(&color_target, &depth_target))
		return;
	if (trace_frame())
		platform_log("clear flags %lx color %08lx z %g count %lu target %08lx depth %08lx", (unsigned long)flags,
			(unsigned long)color, z, (unsigned long)count,
			device.render_target ? (unsigned long)device.render_target->Data : 0,
			device.depth_stencil ? (unsigned long)device.depth_stencil->Data : 0);
	stats.clears++;
	color_to_vec4(color, rgba);
	color_mask = ((flags & D3DCLEAR_TARGET_R) ? 1 : 0) | ((flags & D3DCLEAR_TARGET_G) ? 2 : 0) |
		((flags & D3DCLEAR_TARGET_B) ? 4 : 0) | ((flags & D3DCLEAR_TARGET_A) ? 8 : 0);
	if (!count || !rectangles)
	{
		win32_d3d11_clear(color_target, depth_target,
			target_pixel((float)device.viewport.X, 0), target_pixel((float)device.viewport.Y, 1),
			target_pixel((float)(device.viewport.X + device.viewport.Width), 0),
			target_pixel((float)(device.viewport.Y + device.viewport.Height), 1),
			(flags & D3DCLEAR_TARGET) != 0, color_mask, rgba, (flags & D3DCLEAR_ZBUFFER) != 0, z,
			(flags & D3DCLEAR_STENCIL) != 0, stencil);
		return;
	}
	for (index = 0; index < count; index++)
	{
		INT left = rectangles[index].x1 > device.viewport.X ? rectangles[index].x1 : device.viewport.X;
		INT top = rectangles[index].y1 > device.viewport.Y ? rectangles[index].y1 : device.viewport.Y;
		INT right = rectangles[index].x2 < device.viewport.X + device.viewport.Width ?
			rectangles[index].x2 : device.viewport.X + device.viewport.Width;
		INT bottom = rectangles[index].y2 < device.viewport.Y + device.viewport.Height ?
			rectangles[index].y2 : device.viewport.Y + device.viewport.Height;

		if (left >= right || top >= bottom)
			continue;
		win32_d3d11_clear(color_target, depth_target,
			target_pixel((float)(left + UI_OFFSET), 0), target_pixel((float)top, 1),
			target_pixel((float)(right + UI_OFFSET), 0), target_pixel((float)bottom, 1),
			(flags & D3DCLEAR_TARGET) != 0, color_mask, rgba, (flags & D3DCLEAR_ZBUFFER) != 0, z,
			(flags & D3DCLEAR_STENCIL) != 0, stencil);
	}
}

/* ---------- the anti-aliasing passes: none here */

void halo_screen_anti_alias(short x0, short y0, short x1, short y1)
{
	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
}

/* ---------- presentation */

/* debug.screenshot_every's frames into debug.screenshot_directory, as
d3d8_gl.c writes them */
static void write_screenshot(struct render_target_entry *target)
{
	const char *directory = *config_string("debug.screenshot_directory") ?
		config_string("debug.screenshot_directory") : NULL;
	unsigned long width = target->target.gl_width, height = target->target.gl_height;
	unsigned long image_size = width * height * 4, pixel;
	unsigned char header[54] = { 'B', 'M' };
	unsigned char *pixels;
	char path[512];
	FILE *file;

	if (!directory || !(pixels = malloc(image_size)))
		return;
	if (win32_d3d11_target_read((unsigned int)target->target.texture, pixels, (unsigned int)width, (unsigned int)height))
	{
		/* (destination alpha is the game's scratch) */
		for (pixel = 0; pixel < width * height; pixel++)
			pixels[pixel * 4 + 3] = 0xff;
		snprintf(path, sizeof(path), "%s/frame%05lu.bmp", directory, device.frame);
		if ((file = fopen(path, "wb")) != NULL)
		{
			*(unsigned int *)(header + 2) = (unsigned int)(54 + image_size);
			*(unsigned int *)(header + 10) = 54;
			*(unsigned int *)(header + 14) = 40;
			*(int *)(header + 18) = (int)width;
			*(int *)(header + 22) = -(int)height;
			*(unsigned short *)(header + 26) = 1;
			*(unsigned short *)(header + 28) = 32;
			*(unsigned int *)(header + 34) = (unsigned int)image_size;
			fwrite(header, 1, sizeof(header), file);
			fwrite(pixels, 1, image_size, file);
			fclose(file);
		}
	}
	free(pixels);
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle,
	void *unused, void *unused2)
{
	static long screenshot_every = -1;

	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	if (screenshot_every < 0)
		screenshot_every = config_integer("debug.screenshot_every");
	if (device.ready)
	{
		struct render_target_entry *back_buffer = render_target_get(&device.back_buffer);
		int window_width, window_height, width, height, x, y;

		if (back_buffer && back_buffer->target.texture && screenshot_every > 0 &&
			device.frame % (unsigned long)screenshot_every == 0)
			write_screenshot(back_buffer);
		platform_video_drawable_size(&window_width, &window_height);
		if (back_buffer && back_buffer->target.texture && window_width > 0 && window_height > 0)
		{
			/* letterboxed to the back buffer's shape */
			width = window_width;
			height = (int)((long)window_width * back_buffer->target.gl_height / back_buffer->target.gl_width);
			if (height > window_height)
			{
				height = window_height;
				width = (int)((long)window_height * back_buffer->target.gl_width / back_buffer->target.gl_height);
			}
			x = (window_width - width) / 2;
			y = (window_height - height) / 2;
			win32_d3d11_present((unsigned int)back_buffer->target.texture, x, y, width, height, window_width,
				window_height, config_boolean("display.vsync"));
		}
		/* (the frame rate limit: sdl_platform.c) */
		platform_video_swap();
		xgpu_texture_cache_begin_frame();
	}
	device.frame++;
	stats.presents++;
	if (debug_settings.statistics && device.frame % 60 == 0)
	{
		platform_log("frame %lu: %lu draws, %lu immediate, %lu clears, %lu target changes; skipped %lu no program, "
			"%lu no target, %lu shader; %lu KB of vertices",
			device.frame, stats.draws / stats.presents, stats.immediate_draws / stats.presents,
			stats.clears / stats.presents, stats.target_changes / stats.presents, stats.skipped_no_program,
			stats.skipped_no_target, stats.skipped_shader, stats.vertex_bytes / stats.presents / 1024);
		memset(&stats, 0, sizeof(stats));
	}
	platform_pump_events();

	pthread_mutex_lock(&vertical_blank_lock);
	if (halo_interpolation_enabled())
	{
		flip_count++;
	}
	else
	{
		while (pending_flips >= 2)
			pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
		pending_flips++;
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}

/* ---------- the overlay (ui_overlay.h), not drawn yet: its screens are
laid out, and measure their text as nothing */

#ifdef HALO_GAME_BROWSER
#include "ui_overlay.h"

void ui_overlay_rect(float x, float y, float width, float height, float radius, unsigned int color)
{
	(void)x; (void)y; (void)width; (void)height; (void)radius; (void)color;
}

void ui_overlay_gradient(float x, float y, float width, float height, float radius, unsigned int top,
	unsigned int bottom)
{
	(void)x; (void)y; (void)width; (void)height; (void)radius; (void)top; (void)bottom;
}

void ui_overlay_outline(float x, float y, float width, float height, float radius, float thickness,
	unsigned int color)
{
	(void)x; (void)y; (void)width; (void)height; (void)radius; (void)thickness; (void)color;
}

float ui_overlay_text(int font, float size, float x, float y, int align, unsigned int color, const char *text)
{
	(void)font; (void)size; (void)y; (void)align; (void)color; (void)text;
	return x;
}

float ui_overlay_text_width(int font, float size, const char *text)
{
	(void)font; (void)size; (void)text;
	return 0.0f;
}

float ui_overlay_button(int button, float size, float x, float y, unsigned int color)
{
	(void)button; (void)size; (void)y; (void)color;
	return x;
}

float ui_overlay_button_width(int button, float size)
{
	(void)button; (void)size;
	return 0.0f;
}

void ui_overlay_cutout(float x, float y, float width, float height)
{
	(void)x; (void)y; (void)width; (void)height;
}

int ui_overlay_available(void)
{
	return 0;
}

void ui_overlay_present(int x, int y, int width, int height, int window_width, int window_height)
{
	(void)x; (void)y; (void)width; (void)height; (void)window_width; (void)window_height;
}
#endif

#endif
