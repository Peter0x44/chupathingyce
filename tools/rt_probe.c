/*
RT_PROBE.C

What a Windows RT device offers the Windows RT build (ninja windows-rt),
for running on the device before the game: the Direct3D 11 feature level
of its GPU, whether shaders compile for it (d3dcompiler_47.dll), and
whether a 32-bit program there can have the address space the 32-bit game
needs, the Xbox memory window at 0x80000000 and Custom Edition's tag cache
at 0x40440000 (port/linux/src/platform.h; above 2 GB only with
/LARGEADDRESSAWARE and a 3 GB user address space: bcdedit /set
increaseuserva 3072).

Built by CI with the build (tools/ci_build.py windows-rt), or by hand:

    armv7-w64-mingw32-clang -O1 tools/rt_probe.c -o rt_probe.exe
        -Wl,--large-address-aware -ld3d11 -ldxgi -ldxguid

It prints its findings, and waits for Enter so that its window stays open.
*/

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

static const char *level_name(D3D_FEATURE_LEVEL level)
{
	switch (level)
	{
	case D3D_FEATURE_LEVEL_9_1: return "9_1";
	case D3D_FEATURE_LEVEL_9_2: return "9_2";
	case D3D_FEATURE_LEVEL_9_3: return "9_3";
	case D3D_FEATURE_LEVEL_10_0: return "10_0";
	case D3D_FEATURE_LEVEL_10_1: return "10_1";
	case D3D_FEATURE_LEVEL_11_0: return "11_0";
	case D3D_FEATURE_LEVEL_11_1: return "11_1";
	default: return "unknown";
	}
}

static void probe_direct3d(void)
{
	static const D3D_FEATURE_LEVEL levels[] = {
		D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1,
	};
	static const char shader[] =
		"Texture2D t : register(t0); SamplerState s : register(s0);\n"
		"float4 main(float4 p : SV_Position, float2 c : TEXCOORD0) : SV_Target { return t.Sample(s, c); }\n";
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	D3D_FEATURE_LEVEL level = 0;
	IDXGIDevice *dxgi = NULL;
	IDXGIAdapter *adapter = NULL;
	DXGI_ADAPTER_DESC description;
	HMODULE compiler;
	pD3DCompile compile;
	HRESULT result;

	result = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, sizeof(levels) / sizeof(levels[0]),
		D3D11_SDK_VERSION, &device, &level, &context);
	if (FAILED(result))
	{
		printf("Direct3D 11: no hardware device (%08lx)\n", (unsigned long)result);
		return;
	}
	memset(&description, 0, sizeof(description));
	if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi)) &&
		SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)))
		IDXGIAdapter_GetDesc(adapter, &description);
	printf("Direct3D 11: feature level %s on %ls (%lu MB of video memory)\n", level_name(level),
		description.Description, (unsigned long)(description.DedicatedVideoMemory >> 20));
	printf("  the game draws at 9_3: %s\n", level >= D3D_FEATURE_LEVEL_9_3 ? "yes" :
		"NO (set display.d3d11_feature_level = \"9_1\" in config.toml; untested)");

	compiler = LoadLibraryW(L"d3dcompiler_47.dll");
	compile = compiler ? (pD3DCompile)(void *)GetProcAddress(compiler, "D3DCompile") : NULL;
	if (!compile)
	{
		printf("Shader compiler: d3dcompiler_47.dll is MISSING (the game cannot draw)\n");
	}
	else
	{
		ID3DBlob *code = NULL, *errors = NULL;
		const char *profile = level >= D3D_FEATURE_LEVEL_10_0 ? "ps_4_0" :
			level >= D3D_FEATURE_LEVEL_9_3 ? "ps_4_0_level_9_3" : "ps_4_0_level_9_1";
		DWORD start = GetTickCount();

		result = compile(shader, sizeof(shader) - 1, "probe", NULL, NULL, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
			0, &code, &errors);
		printf("Shader compiler: %s for %s (%lu ms)\n", SUCCEEDED(result) ? "compiles" : "FAILS", profile,
			(unsigned long)(GetTickCount() - start));
		if (code)
			ID3D10Blob_Release(code);
		if (errors)
			ID3D10Blob_Release(errors);
	}
	if (adapter)
		IDXGIAdapter_Release(adapter);
	if (dxgi)
		IDXGIDevice_Release(dxgi);
	ID3D11DeviceContext_Release(context);
	ID3D11Device_Release(device);
}

/* reserves [base, base + size) at that address; whether it could */
static BOOL reserve_at(unsigned long base, unsigned long size)
{
	void *reserved = VirtualAlloc((void *)base, size, MEM_RESERVE, PAGE_NOACCESS);

	if (!reserved)
		return FALSE;
	VirtualFree(reserved, 0, MEM_RELEASE);
	return reserved == (void *)base;
}

static void probe_memory(void)
{
	SYSTEM_INFO system;
	MEMORYSTATUSEX memory;
	BOOL window, tag_cache;

	GetSystemInfo(&system);
	memory.dwLength = sizeof(memory);
	GlobalMemoryStatusEx(&memory);
	printf("Memory: %lu MB, %lu MB free; this program's addresses %p to %p\n",
		(unsigned long)(memory.ullTotalPhys >> 20), (unsigned long)(memory.ullAvailPhys >> 20),
		system.lpMinimumApplicationAddress, system.lpMaximumApplicationAddress);
	window = reserve_at(0x80000000UL, 0x20000000UL);
	tag_cache = reserve_at(0x40440000UL, 0x01700000UL);
	printf("  the Xbox memory window, 0x80000000 to 0xa0000000: %s\n", window ? "can be had" :
		"CANNOT be had (the game cannot start: try bcdedit /set increaseuserva 3072, then restart)");
	printf("  Custom Edition's tag cache, 0x40440000: %s\n", tag_cache ? "can be had" : "CANNOT be had");
}

int main(void)
{
	OSVERSIONINFOW version;

	memset(&version, 0, sizeof(version));
	version.dwOSVersionInfoSize = sizeof(version);
	GetVersionExW(&version);
	printf("ChupathingyCE Windows RT probe: Windows %lu.%lu build %lu\n\n", (unsigned long)version.dwMajorVersion,
		(unsigned long)version.dwMinorVersion, (unsigned long)version.dwBuildNumber);
	probe_direct3d();
	printf("\n");
	probe_memory();
	printf("\nPress Enter to close.\n");
	getchar();
	return 0;
}
