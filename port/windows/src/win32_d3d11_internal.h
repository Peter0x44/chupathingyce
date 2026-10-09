/*
WIN32_D3D11_INTERNAL.H

What the Direct3D 11 device's Windows half (win32_d3d11.c) and its texture
shim (win32_d3d11_textures.c) share; see port/windows/include/halo_d3d11.h.
*/

#ifndef __WIN32_D3D11_INTERNAL_H
#define __WIN32_D3D11_INTERNAL_H

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

extern ID3D11Device *win32_d3d11_device;
extern ID3D11DeviceContext *win32_d3d11_context;
extern D3D_FEATURE_LEVEL win32_d3d11_feature_level;

/* the GL texture's view, made from its pixels the first time it is drawn
with (or again once they change), or NULL */
ID3D11ShaderResourceView *win32_d3d11_texture_view(unsigned int name);

void platform_log(const char *format, ...);

#endif
