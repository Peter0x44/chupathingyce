"""Ninja rules for the native Windows builds (``ninja windows``, ``ninja
windows64``, ``ninja windows-arm64``, ``ninja windows-d3d11``).

Like the Linux build (tools/linux_build.py), it compiles the game sources
with clang for 32-bit x86 Windows (i686-pc-windows-msvc), adds the platform layer shared with
Linux (``port/linux/src``) and the Windows parts in ``port/windows``, and
links ``build/windows/halo.exe`` with lld. It is generated only when
configure.py runs on Windows. See port/windows/README.md for the design.

``ninja windows64`` compiles the same units for x64 Windows
(x86_64-pc-windows-msvc) into ``build/windows64/halo.exe``, with the 64-bit
builds' code paths (HALO_64BIT: source/cseries/xbox_address.h), as
``ninja linux64`` and ``ninja macos`` have them. Both play Halo PC's Custom
Edition maps (HALO_CUSTOM_EDITION).
``ninja windows-arm64`` is the same 64-bit build for 64-bit ARM Windows
(aarch64-pc-windows-msvc) into ``build/windows-arm64/halo.exe``; it
cross-compiles from an x64 machine as well, with Visual Studio's ARM64
libraries.
``ninja windows-d3d11`` is the 32-bit build drawn with Direct3D 11 at
feature level 9_3 rather than OpenGL (``build/windows-d3d11/halo.exe``): the
renderer of the Windows RT build, on a computer that runs the rest.
Windows keeps `long` 32 bits wide on x64 and ARM64 (LLP64), as the Xbox's compiler
did, so the sources need none of the LP64 builds' `long` rewrite
(tools/lp64_build.py).
"""

import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

from .version import VERSION_SOURCES, identity_defines, release_build, version
from .linux_build import (CUSTOM_EDITION_DEFINES, LINUX_PROFILE, MBEDTLS_DIR, MINIUPNPC_DIR, OPTIMISATION, STB_DIR, WINDOWS_PROFILE,
                          XDK_INCLUDE, game_browser_defines, lto_mode, march_flag, miniupnpc_sources, pgo_mode, compile_launcher, game_defines_and_includes,
                          game_sources, musl_math_cflags, musl_math_sources, pgo_profile, profile_use_flags,
                          xdk_headers)
from .lp64_build import lp64_excluded
from .embed_assets import hud_assets_build, hud_configure_inputs, ui_fonts_build
from .ninja_syntax import Writer

LINUX_DIR = Path("port/linux")
PORT_DIR = Path("port/windows")
PORT_CONFIG = PORT_DIR / "port.json"
BUILD = Path("build/windows")

SDL_VERSION = "3.4.16"
SDL_URL = (
    f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/"
    f"SDL3-devel-{SDL_VERSION}-VC.zip"
)
# (its SHA-256 when it was pinned: the DLL ships in the builds)
SDL_SHA256 = "1a784cb2a5c64d56fe7a62090fe9d242d9865f235e4ea9678f1a6ba4e693e7de"
THIRD_PARTY = BUILD / "third_party"
SDL_DIR = THIRD_PARTY / f"SDL3-{SDL_VERSION}"

# Flags shared by every unit. The Microsoft target gives the game the ABI it
# was written against natively: 16-bit wchar_t, MSVC structure layout,
# __declspec, calling conventions and COMDAT inline functions.
#  - 32-bit time_t, as in the Xbox's C runtime (the same on both sides of the
#    platform layer, which share struct timespec),
#  - tentative definitions shared between units (-fcommon),
#  - no optimisations that assume the absence of MSVC-tolerated UB,
#  - EBP frames (MSVC /Oy-): get_return_eip and the stack walker follow the
#    frame chain.
# the TOML parser the platform layer reads config.toml with (port_config.c)
TOML_DIR = Path("port/third_party/tomlc17")
EXPAT_DIR = Path("port/third_party/expat")
EXPAT_SOURCES = ("xmlparse.c", "xmlrole.c", "xmltok.c", "random_rand_s.c")
KCP_DIR = Path("port/third_party/kcp")
QRCODEGEN_DIR = Path("port/third_party/qrcodegen")
MONOCYPHER_DIR = Path("port/third_party/monocypher")
# the port's zlib (port/third_party/zlib/zlib_prefixed.h), which inflates
# the maps, the menus' and the HUD's PNGs and the updates
ZLIB_DIR = Path("port/third_party/zlib")
ZLIB_SOURCES = ("adler32.c", "crc32.c", "inffast.c", "inflate.c", "inftrees.c", "uncompr.c", "zutil.c")
# (its names prefixed, and the one Z_PREFIX leaves, its error messages, which
# the game's zlib names the same)
ZLIB_DEFINES = ("-DZ_PREFIX", "-Dz_errmsg=z_port_errmsg")


def updater_defines(release: bool) -> str:
    """the version's defines (port/linux/src/updater.c, the self-updater, has
    them, and gives the version to the rest): the version (tools/version.py),
    whether this build is a release's (only those look for updates), and its
    configuration; and the channel and commit (build_identity.c)"""
    flavor = "release" if release else "debug"
    return (f'-DHALO_VERSION=\\"{version()}\\" -DHALO_RELEASE_BUILD={int(release_build())} '
            f'-DHALO_BUILD_FLAVOR=\\"{flavor}\\" {identity_defines()}')

WINDOWS_ABI_FLAGS = [
    "--target=i686-pc-windows-msvc",
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    # the same floating point results on every port (every machine in a
    # system link game simulates it from the same inputs): no
    # fused multiply-adds (port/include/halo_math.h)
    "-ffp-contract=off",
    OPTIMISATION,
    "-g",
    "-gcodeview",
    "-D_USE_32BIT_TIME_T",
    "-D_CRT_SECURE_NO_WARNINGS",
    "-D_CRT_NONSTDC_NO_WARNINGS",
    "-D_WINSOCK_DEPRECATED_NO_WARNINGS",
    # the MSVC 7 signatures the game uses: swprintf without a buffer size,
    # two-argument wcstok
    "-D_CRT_NON_CONFORMING_SWPRINTFS",
    "-D_CRT_NON_CONFORMING_WCSTOK",
]

GAME_FLAGS = [
    "-std=gnu89",
    "-w",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=int-conversion",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=return-type",
]

PLATFORM_FLAGS = [
    "-std=gnu11",
    "-DHALO_LINUX_PLATFORM_LAYER",
    "-Wall",
    "-Wno-unused-function",
    "-Wno-unknown-pragmas",
    "-Wno-microsoft-anon-tag",
    "-Wno-pragma-pack",
    "-Wno-ignored-attributes",
    "-Wno-duplicate-decl-specifier",
    "-Wno-missing-braces",
    "-Wno-unused-variable",
    "-Wno-ignored-pragmas",
    "-Wno-microsoft-enum-forward-reference",
    "-Wno-language-extension-token",
]

# port/windows/src/win32_*.c talk to Windows only: they see the Windows SDK
# instead of the Xbox SDK (the two define the same names differently).
WIN32_FLAGS = [
    "-std=gnu11",
    "-DWIN32_LEAN_AND_MEAN",
    "-DNOMINMAX",
    "-D_WIN32_WINNT=0x0A00",
    "-Wall",
]

# The conversions that truncate a 64-bit pointer, errors in the 64-bit
# build: each is a place that still treats an Xbox address as a pointer or
# the reverse (as in the LP64 builds, tools/lp64_build.py)
POINTER_TRUNCATION_ERRORS = [
    "-Werror=int-to-pointer-cast",
    "-Werror=pointer-to-int-cast",
    "-Werror=void-pointer-to-int-cast",
    "-Werror=int-conversion",
]


@dataclass
class WindowsTarget:
    """What the 32-bit and the 64-bit Windows builds compile differently."""
    # the build's name: its ninja target, its folder in build/, its rules' prefix
    name: str
    # the clang target
    triple: str
    # SDL's folder of libraries for it (lib/x86, lib/x64, lib/arm64)
    sdl_arch: str
    abi_flags: List[str]
    game_flags: List[str]
    platform_flags: List[str]
    win32_flags: List[str]
    # the link's, ahead of link-time optimisation's
    ldflags: List[str]
    # the comment above its rules
    comment: str
    # optimised with a profile (pgo/): the committed ones are the 32-bit builds'
    pgo: bool = True
    # leaves out what the 64-bit builds do (port/linux/port.json "lp64")
    lp64_exclusions: bool = False
    # what draws: "gl" (port/linux/src/d3d8_gl.c), or "d3d11", Direct3D 11
    # at feature level 9_3 (HALO_D3D11: port/windows/src/d3d8_d3d11.c)
    renderer: str = "gl"
    # "msvc": clang for the Microsoft ABI and Visual Studio's C runtime, with
    # SDL's Visual C++ package; or "mingw": llvm-mingw for msvcrt.dll (the
    # Windows RT build), with an SDL built here, and GNU ld's options
    toolchain: str = "msvc"

    @property
    def build(self) -> Path:
        return Path("build") / self.name

    def march(self, sln: Any) -> str:
        """the instruction set (march_flag): for ARM64 the baseline every
        64-bit ARM processor has with --portable, or when cross-compiling
        (this machine's processor is not the target's), else this machine's;
        for 32-bit ARM, Windows's own (ARMv7 Thumb-2 with NEON)"""
        if self.sdl_arch == "arm":
            return "-march=armv7-a"
        if self.sdl_arch != "arm64":
            return march_flag(sln)
        if getattr(sln, "port_portable", False) or platform.machine().lower() not in ("arm64", "aarch64"):
            return "-march=armv8-a"
        return "-mcpu=native"


WINDOWS32 = WindowsTarget(
    name="windows",
    triple="i686-pc-windows-msvc",
    sdl_arch="x86",
    # Halo PC's Custom Edition maps (linux_build.py, CUSTOM_EDITION_DEFINES),
    # as the 32-bit Linux build has them: their tag cache is mapped at the
    # host address they are linked to (port/linux/src/xbox_memory.c)
    abi_flags=[*WINDOWS_ABI_FLAGS, *CUSTOM_EDITION_DEFINES],
    game_flags=GAME_FLAGS,
    platform_flags=PLATFORM_FLAGS,
    win32_flags=WIN32_FLAGS,
    ldflags=[
        "--target=i686-pc-windows-msvc",
        "-fuse-ld=lld",
        "-g",
        # the Xbox memory window is at 0x80000000, in the upper half of the
        # 32-bit address space
        "-Wl,/LARGEADDRESSAWARE",
        "-Wl,/STACK:0x800000",
    ],
    comment="Native Windows build (ninja windows)",
)

WINDOWS64 = WindowsTarget(
    name="windows64",
    triple="x86_64-pc-windows-msvc",
    sdl_arch="x64",
    abi_flags=[
        "--target=x86_64-pc-windows-msvc",
        # (time_t is 64-bit on x64: the C runtime has no 32-bit one there)
        *(flag for flag in WINDOWS_ABI_FLAGS
          if not flag.startswith("--target=") and flag != "-D_USE_32BIT_TIME_T"),
        # the 64-bit builds' code paths (source/cseries/xbox_address.h)
        "-DHALO_64BIT",
        # Halo PC's Custom Edition maps (linux_build.py, CUSTOM_EDITION_DEFINES)
        *CUSTOM_EDITION_DEFINES,
    ],
    game_flags=[
        *(flag for flag in GAME_FLAGS if flag not in ("-w", "-Wno-error=int-conversion",
                                                      "-Wno-error=implicit-function-declaration")),
        "-ferror-limit=0",
        # -w would also hide the errors below
        "-Wno-everything",
        *POINTER_TRUNCATION_ERRORS,
        "-Werror=pointer-integer-compare",
        # an undeclared function returns int, truncating a returned pointer
        "-Werror=implicit-function-declaration",
        # (not -Werror=format, as the LP64 builds have: where they spell
        # `long` int, a %d given a long is the same size here, and the
        # sizes that differ between 32 and 64 bits are the same as theirs)
    ],
    platform_flags=[*PLATFORM_FLAGS, "-Werror=incompatible-pointer-types", *POINTER_TRUNCATION_ERRORS],
    win32_flags=[*WIN32_FLAGS, *POINTER_TRUNCATION_ERRORS],
    ldflags=[
        "--target=x86_64-pc-windows-msvc",
        "-fuse-ld=lld",
        "-g",
        # the Xbox address space is 4 GB at 1 TB (xbox_address.h), reserved
        # at start-up (port/linux/src/xbox_memory.c). High-entropy
        # randomization would spread the first allocations over the bottom
        # terabyte, up to that window; without it they stay low
        "-Wl,/HIGHENTROPYVA:NO",
        "-Wl,/STACK:0x800000",
    ],
    comment="Native 64-bit Windows build (ninja windows64)",
    pgo=False,
    lp64_exclusions=True,
)


# the 64-bit build for 64-bit ARM Windows: the x64 build's flags for the
# other processor
WINDOWS_ARM64 = WindowsTarget(
    name="windows-arm64",
    triple="aarch64-pc-windows-msvc",
    sdl_arch="arm64",
    abi_flags=[
        "--target=aarch64-pc-windows-msvc",
        *(flag for flag in WINDOWS64.abi_flags if not flag.startswith("--target=")),
        # (char is signed on Windows ARM64, as on the Xbox; said all the
        # same, as the arm64 server says it)
        "-fsigned-char",
    ],
    game_flags=WINDOWS64.game_flags,
    platform_flags=WINDOWS64.platform_flags,
    win32_flags=WINDOWS64.win32_flags,
    ldflags=[
        "--target=aarch64-pc-windows-msvc",
        *(flag for flag in WINDOWS64.ldflags if not flag.startswith("--target=")),
    ],
    comment="Native 64-bit ARM Windows build (ninja windows-arm64)",
    pgo=False,
    lp64_exclusions=True,
)


# The 32-bit build drawn with Direct3D 11 at feature level 9_3, as the
# Windows RT build is (port/windows/include/halo_d3d11.h): that renderer,
# on a computer that also runs the 32-bit game (feature level 9_3 asked of
# its GPU, or of Direct3D's software renderer).
WINDOWS_D3D11 = WindowsTarget(
    name="windows-d3d11",
    triple=WINDOWS32.triple,
    sdl_arch=WINDOWS32.sdl_arch,
    abi_flags=[*WINDOWS32.abi_flags, "-DHALO_D3D11"],
    game_flags=WINDOWS32.game_flags,
    platform_flags=WINDOWS32.platform_flags,
    win32_flags=WINDOWS32.win32_flags,
    ldflags=WINDOWS32.ldflags,
    comment="Native Windows build drawn with Direct3D 11 at feature level 9_3 (ninja windows-d3d11)",
    pgo=False,
    renderer="d3d11",
)

# The Windows RT build (Windows RT 8.1 on 32-bit ARM: Surface 2, Surface
# RT): the 32-bit game for ARMv7, built with llvm-mingw against msvcrt.dll
# (Windows RT 8.1 has no Universal C Runtime), for Windows 8.1's API, drawn
# with Direct3D 11 at feature level 9_3. Its SDL is built here
# (sdl_arm_build): SDL publishes none for 32-bit ARM.
WINDOWS_RT = WindowsTarget(
    name="windows-rt",
    triple="armv7-w64-windows-gnu",
    sdl_arch="arm",
    abi_flags=[
        "--target=armv7-w64-windows-gnu",
        *(flag for flag in WINDOWS_ABI_FLAGS if not flag.startswith("--target=")),
        *CUSTOM_EDITION_DEFINES,
        "-DHALO_D3D11",
        # (msvcrt.dll's printf has no %lld or %zu; mingw's own does)
        "-D__USE_MINGW_ANSI_STDIO=1",
        # (as the Xbox's: char is signed on Windows on ARM too)
        "-fsigned-char",
    ],
    game_flags=WINDOWS32.game_flags,
    platform_flags=WINDOWS32.platform_flags,
    win32_flags=[*(flag for flag in WIN32_FLAGS if not flag.startswith("-D_WIN32_WINNT=")), "-D_WIN32_WINNT=0x0603"],
    ldflags=[
        "--target=armv7-w64-windows-gnu",
        "-fuse-ld=lld",
        "-g",
        # the Xbox memory window is at 0x80000000 (as the 32-bit x86 build)
        "-Wl,--large-address-aware",
        "-Wl,--stack,0x800000",
    ],
    comment="Windows RT build: 32-bit ARM, llvm-mingw and msvcrt.dll, Direct3D 11 at 9_3 (ninja windows-rt)",
    pgo=False,
    renderer="d3d11",
    toolchain="mingw",
)

# the platform layer's GL renderer, which the Direct3D 11 builds draw without
# (their overlay is d3d8_d3d11.c's for now)
GL_RENDERER_SOURCES = {"d3d8_gl.c", "gl_functions.c", "xgpu_post.c", "xgpu_shader_cache.c", "ui_overlay.c"}
# and what Direct3D 11 needs (d3dcompiler_47.dll is loaded when the device is made)
D3D11_LIBRARIES = ["d3d11", "dxgi", "dxguid"]


def _load_config() -> Dict[str, Any]:
    with open(PORT_CONFIG, "r", encoding="utf-8") as f:
        return json.load(f)


def windows_configure_inputs() -> List[Path]:
    """Files whose change must re-run configure.py."""
    return [Path(__file__), PORT_CONFIG, PORT_DIR / "src", LINUX_DIR / "src", LINUX_DIR / "game", *hud_configure_inputs()]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def fetch_sdl() -> None:
    """Downloads SDL3's Visual C++ development package (headers, import
    library and DLL) once."""
    if all((SDL_DIR / "lib" / arch / "SDL3.lib").is_file() for arch in ("x86", "x64", "arm64")):
        return
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    archive = THIRD_PARTY / f"SDL3-devel-{SDL_VERSION}-VC.zip"
    print(f"Downloading {SDL_URL}")
    with urllib.request.urlopen(SDL_URL) as response, open(archive, "wb") as f:
        shutil.copyfileobj(response, f)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SDL_SHA256:
        archive.unlink()
        raise SystemExit(f"{SDL_URL}: SHA-256 {digest}, not the pinned {SDL_SHA256}")
    with zipfile.ZipFile(archive) as z:
        z.extractall(THIRD_PARTY)
    archive.unlink()


# The runtime of instrumented builds (-fprofile-generate), which LLVM for
# Windows ships only for x86-64: its C sources, from the LLVM release of the
# compiler that uses them, built for the 32-bit game.
PROFILE_RUNTIME_SOURCES = [
    "InstrProfiling.c", "InstrProfilingBuffer.c", "InstrProfilingFile.c", "InstrProfilingInternal.c",
    "InstrProfilingMerge.c", "InstrProfilingMergeFile.c", "InstrProfilingNameVar.c",
    "InstrProfilingPlatformWindows.c", "InstrProfilingUtil.c", "InstrProfilingValue.c",
    "InstrProfilingVersionVar.c", "InstrProfilingWriter.c", "WindowsMMap.c",
]
PROFILE_RUNTIME_HEADERS = [
    "lib/profile/InstrProfiling.h", "lib/profile/InstrProfilingInternal.h", "lib/profile/InstrProfilingPort.h",
    "lib/profile/InstrProfilingUtil.h", "lib/profile/WindowsMMap.h", "include/profile/InstrProfData.inc",
    "include/profile/instr_prof_interface.h", "include/profile/MIBEntryDef.inc", "include/profile/MemProfData.inc",
]


def clang_release(cc: str) -> Optional[str]:
    """the release (22.1.0) of the clang named cc, or None"""
    try:
        output = subprocess.run([cc, "--version"], capture_output=True, text=True, check=False).stdout
    except OSError:
        return None
    match = re.search(r"clang version (\d+\.\d+\.\d+)", output)
    return match.group(1) if match else None


def fetch_profile_runtime(release: str) -> Path:
    """Downloads compiler-rt's profile runtime sources for this release once;
    returns their directory (with lib/profile and include/profile)."""
    root = THIRD_PARTY / f"compiler-rt-profile-{release}"
    files = [f"lib/profile/{name}" for name in PROFILE_RUNTIME_SOURCES] + PROFILE_RUNTIME_HEADERS
    for name in files:
        target = root / name
        if target.is_file():
            continue
        url = f"https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-{release}/compiler-rt/{name}"
        print(f"Downloading {url}")
        target.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(url) as response:
            data = response.read()
        target.write_bytes(data)
    return root


# a C __inline function with external linkage, defined in a .c file
EXPORTED_INLINE = re.compile(
    r"^(?!\s*static\b)[^;{}()\n]*\b(?:__inline|_inline|__forceinline)\b[^;{}()]*?"
    r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{",
    re.M,
)
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def inline_export_wrapper(source: Path, build: Path = BUILD) -> Path:
    """MSVC emits a C __inline function with external linkage wherever a
    call to it is not inlined, and other units may call it through a
    prototype; clang can inline every call in the defining unit, leaving no
    copy. A unit that defines such a function is compiled through a
    generated wrapper that includes it and takes the functions' addresses,
    which makes clang emit them (as COMDATs). Returns the file to compile."""
    if source.suffix != ".c" or not str(source).replace(os.sep, "/").startswith("source/"):
        return source
    names = EXPORTED_INLINE.findall(COMMENT.sub("", source.read_text(encoding="utf-8", errors="replace")))
    if not names:
        return source
    wrapper = build / "inline_exports" / source
    text = (
        "/* generated by tools/windows_build.py: see inline_export_wrapper */\n"
        f'#include "{source.resolve().as_posix()}"\n'
        "static void *const halo_windows_inline_exports[] __attribute__((used)) = {\n"
        + "".join(f"\t(void *){name},\n" for name in names)
        + "};\n"
    )
    wrapper.parent.mkdir(parents=True, exist_ok=True)
    if not wrapper.is_file() or wrapper.read_text(encoding="utf-8") != text:
        wrapper.write_text(text, encoding="utf-8")
    return wrapper


def miniupnpc_arm64_wrapper(source: Path, build: Path) -> Path:
    """ARM64's winnt.h includes <stdint.h>, whose uint32_t clashes with the
    one addr_is_reserved.c declares itself under _MSC_VER (unsigned long,
    the same width on Windows); that file is compiled through a generated
    wrapper that gives its own another name, so that miniupnpc stays as it
    is (port/third_party/miniupnpc/README.md). Returns the file to compile."""
    if source.name != "addr_is_reserved.c":
        return source
    wrapper = build / "miniupnpc" / source.name
    text = (
        "/* generated by tools/windows_build.py: see miniupnpc_arm64_wrapper */\n"
        "#include <winsock2.h>\n"
        "#include <ws2tcpip.h>\n"
        "#define uint32_t miniupnpc_uint32_t\n"
        f'#include "{source.resolve().as_posix()}"\n'
    )
    wrapper.parent.mkdir(parents=True, exist_ok=True)
    if not wrapper.is_file() or wrapper.read_text(encoding="utf-8") != text:
        wrapper.write_text(text, encoding="utf-8")
    return wrapper


def generate_windows_build(n: Writer, sln: Any) -> None:
    if sys.platform != "win32" or not PORT_CONFIG.is_file():
        return
    try:
        fetch_sdl()
    except OSError as error:
        print(f"Windows build disabled: cannot fetch SDL3 ({error})", file=sys.stderr)
        return
    for target in (WINDOWS32, WINDOWS64, WINDOWS_ARM64, WINDOWS_D3D11, WINDOWS_RT):
        # (the Windows RT build only where its SDL's sources can be had)
        if target.toolchain == "mingw" and not fetch_sdl_source():
            print("Windows RT build disabled: cannot clone SDL3's sources", file=sys.stderr)
            continue
        generate_windows_target(n, sln, target)


# the Windows RT build's SDL: the pinned sources (android_build.py's),
# built for 32-bit ARM by llvm-mingw
SDL_ARM_SOURCE = THIRD_PARTY / "SDL3-source"
SDL_ARM_BUILD = Path("build/windows-rt/third_party/SDL3-build")


def fetch_sdl_source() -> bool:
    """SDL3's sources (once), checked against the pinned commit"""
    from .android_build import SDL_TAG, SDL_URL, check_sdl_commit

    if (SDL_ARM_SOURCE / "CMakeLists.txt").is_file():
        return True
    SDL_ARM_SOURCE.parent.mkdir(parents=True, exist_ok=True)
    print(f"Cloning SDL3 {SDL_TAG} (the Windows RT build's)")
    if subprocess.run(["git", "clone", "-q", "-c", "advice.detachedHead=false", "--depth", "1", "--branch", SDL_TAG,
                       SDL_URL, str(SDL_ARM_SOURCE)]).returncode != 0:
        return False
    check_sdl_commit(SDL_ARM_SOURCE)
    return True


def sdl_arm_build(n: Writer, prefix: str, cc: str) -> Path:
    """the rule that builds SDL3.dll for 32-bit ARM with cc's CMake
    toolchain; returns its import library"""
    import_library = SDL_ARM_BUILD / "libSDL3.dll.a"
    # (its C++ compiler and resource compiler beside it: armv7-w64-mingw32-clang++, -windres)
    suffix = cc[-len(".exe"):] if cc.lower().endswith(".exe") else ""
    base = cc[: len(cc) - len(suffix)]
    tool_prefix = base[: -len("clang")] if base.endswith("clang") else "armv7-w64-mingw32-"
    n.rule(
        name=f"{prefix}_sdl3",
        # (through cmd: ninja runs a command without a shell on Windows)
        command=(f"cmd /c cmake -S {SDL_ARM_SOURCE} -B {SDL_ARM_BUILD} -G Ninja -DCMAKE_BUILD_TYPE=Release "
                 "-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=ARM "
                 f"-DCMAKE_C_COMPILER={_quote(cc)} -DCMAKE_CXX_COMPILER={_quote(base + '++' + suffix)} "
                 f"-DCMAKE_RC_COMPILER={_quote(tool_prefix + 'windres' + suffix)} "
                 "-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "
                 f"> {SDL_ARM_BUILD.parent / 'sdl3-configure.log'} && ninja -C {SDL_ARM_BUILD} "
                 f"> {SDL_ARM_BUILD.parent / 'sdl3-build.log'}"),
        description=f"{prefix.upper()} SDL3 (32-bit ARM)",
    )
    n.build(outputs=[import_library, SDL_ARM_BUILD / "SDL3.dll"], rule=f"{prefix}_sdl3",
            implicit=[SDL_ARM_SOURCE / "CMakeLists.txt"])
    return import_library


def generate_windows_target(n: Writer, sln: Any, target: WindowsTarget) -> None:
    """one Windows build's rules: ``ninja windows``, ``ninja windows64``,
    ``ninja windows-arm64``, ``ninja windows-d3d11`` or ``ninja windows-rt``"""
    linux_config: Dict[str, Any] = json.loads((LINUX_DIR / "port.json").read_text(encoding="utf-8"))
    config = _load_config()

    prefix = target.name
    label = prefix.upper()
    build = target.build
    tags_header = build / "halo_msvc_tags.h"
    obj_dir = build / "obj"
    output = build / "halo.exe"
    sdl_dll = build / "SDL3.dll"
    sdl_lib = SDL_DIR / "lib" / target.sdl_arch
    mingw = target.toolchain == "mingw"
    cc = (getattr(sln, "windows_rt_cc", None) or "armv7-w64-mingw32-clang") if mingw else \
        (getattr(sln, "windows_cc", None) or "clang")
    prefix_header = PORT_DIR / "include" / "halo_windows_prefix.h"
    crt_include = PORT_DIR / "include" / "crt"
    posix_include = PORT_DIR / "include" / "posix"
    excluded = set(lp64_excluded()) if target.lp64_exclusions else set()

    n.comment(target.comment)
    n.variable(f"{prefix}_cc", cc)
    # MSVC gives struct tags first named in a prototype file scope; clang
    # does not (the Linux build's generator also writes these declarations)
    n.rule(
        name=f"{prefix}_msvc_tags",
        command="$python tools/linux_msvc_semantics.py --output $out $scan",
        description=f"{label} MSVC TAGS $out",
        restat=True,
    )
    game_headers = sorted(p for p in Path("source").rglob("*") if p.suffix in (".c", ".h"))
    # (mingw's GNU target has no COMDAT C inlines either: as the Linux
    # build, the header also makes each inline function weak, and the
    # platform layer has one of its own for the SDK's inlines,
    # halo_windows_prefix.h)
    n.build(outputs=tags_header, rule=f"{prefix}_msvc_tags",
            implicit=[Path("tools/linux_msvc_semantics.py"), *game_headers, *(xdk_headers() if mingw else [])],
            variables={"scan": f"--all-inlines --tags source --inlines source --inlines {XDK_INCLUDE}" if mingw
                       else "--tags source"})
    platform_semantics_header = build / "platform_msvc_semantics.h"
    if mingw:
        n.build(outputs=platform_semantics_header, rule=f"{prefix}_msvc_tags",
                implicit=[Path("tools/linux_msvc_semantics.py"), *xdk_headers()],
                variables={"scan": f"--inlines {XDK_INCLUDE}"})
    n.rule(
        name=f"{prefix}_cc",
        command=f"{compile_launcher(sln)}${prefix}_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description=f"{label} CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name=f"{prefix}_link",
        command=f"${prefix}_cc $ldflags -o $out --rsp-quoting=windows @$out.rsp $libs",
        description=f"{label} LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(
        name=f"{prefix}_copy",
        command="$python -c \"import shutil,sys; shutil.copyfile(sys.argv[1], sys.argv[2])\" $in $out",
        description=f"{label} COPY $out",
    )

    # the high-res HUD's textures (port/assets/hud; port/linux/src/hud_hires.c)
    embedded_assets = (hud_assets_build(n, prefix, build / "generated" / "hud_hires_assets.c")
                       + ui_fonts_build(n, prefix, build / "generated" / "ui_fonts.c", sln))

    # (the game browser, the game list and dedicated servers, as every
    # desktop build has them: HALO_GAME_BROWSER, configure.py; a debug build
    # checks its stack frames (/GS), and stops at the first one overrun, as
    # at the first failed assertion; a release build does not, so that an
    # overrun nobody has met cannot end a game)
    abi = " ".join(target.abi_flags + [target.march(sln)] + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False)
                                                           else ["-fstack-protector-strong"])
                   + game_browser_defines(sln))
    sdl_include = SDL_DIR / "include"
    sdl_import = sdl_arm_build(n, prefix, cc) if mingw else sdl_lib / "SDL3.lib"
    if mingw:
        sdl_lib = sdl_import.parent
    libs = " ".join(
        [_quote(sdl_import)]
        + [f"-l{lib}" for lib in config.get("libraries", [])]
        + ([f"-l{lib}" for lib in D3D11_LIBRARIES] if target.renderer == "d3d11" else [])
    )
    # (the linker's map beside the program, halo.map: its functions'
    # addresses, kept with the symbols, tools/ci_build.py)
    if mingw:
        # (GNU ld's spellings of the same, and the symbols' PDB beside it)
        base_ldflags = list(target.ldflags) + [f"-Wl,-Map={_quote(build / 'halo.map')}",
                                               f"-Wl,--pdb={_quote(build / 'halo.pdb')}"]
        if getattr(sln, "port_release", False):
            base_ldflags += ["-Wl,--subsystem,windows", "-Wl,-e,mainCRTStartup"]
        else:
            base_ldflags += ["-Wl,--subsystem,console"]
    else:
        base_ldflags = list(target.ldflags) + ["-Wl,/MAP"]
    if mingw:
        pass
    elif getattr(sln, "port_release", False):
        # no console window (the port's log goes to halo.log instead,
        # win32_posix.c): under Wine (Proton, gamescope) the console window
        # can hide the game's window
        base_ldflags += ["-Wl,/SUBSYSTEM:WINDOWS", "-Wl,/ENTRY:mainCRTStartup"]
    else:
        base_ldflags += ["-Wl,/SUBSYSTEM:CONSOLE"]

    def emit(obj_dir: Path, output: Path, extra_cflags: List[str], extra_ldflags: List[str],
             extra_objects: List[Path], implicit_inputs: List[Path]) -> None:
        """the objects and the executable, with the given extra flags"""
        extra = " ".join(extra_cflags)
        objects: List[Path] = []

        def add_object(source: Path, cflags: str) -> None:
            obj = obj_dir / source.with_suffix(".o")
            objects.append(obj)
            compiled = inline_export_wrapper(source, build)
            if target.sdl_arch == "arm64" and source.parent == MINIUPNPC_DIR / "src":
                compiled = miniupnpc_arm64_wrapper(source, build)
            # (each unit's own first symbol: halo_windows_prefix.h HALO_UNIT)
            unit = " -DHALO_UNIT=halo_unit_" + re.sub(r"\W", "_", source.as_posix()) if mingw else ""
            n.build(
                outputs=obj,
                rule=f"{prefix}_cc",
                inputs=compiled,
                implicit=[*xdk_headers(), prefix_header, tags_header, source, *implicit_inputs,
                          *([platform_semantics_header] if mingw else [])],
                variables={"cflags": f"{cflags} {extra}{unit}"},
            )

        game_cflags = " ".join([
            abi,
            " ".join(target.game_flags),
            f"-include {prefix_header}",
            f"-include {tags_header}",
            f"-I{crt_include}",
            f"-I{PORT_DIR / 'include'}",
            # the port's headers the game's units include (halo_keyboard.h,
            # halo_menus.h), but not the Linux build's C runtime wrappers
            # next to them, which no game unit includes in quotes
            f"-iquote {LINUX_DIR / 'include'}",
            # the headers of the port's own game units (port/linux/game), for
            # the game sources that call them
            f"-iquote {Path(linux_config['game_sources'])}",
            game_defines_and_includes(linux_config),
            # the Xbox SDK declarations (port/include/xdk) come before the
            # Windows SDK, which has headers of the same names
            f"-I{XDK_INCLUDE}",
        ])
        for source in game_sources(linux_config):
            if source.as_posix() not in excluded:
                add_object(source, game_cflags)
        for source in sorted(Path(linux_config["game_sources"]).glob("*.c")):
            add_object(source, game_cflags)
        # the dedicated server's director, with the game browser (server/)
        if getattr(sln, "game_browser", False):
            for source in sorted(Path("server/src").glob("*.c")):
                add_object(source, game_cflags)

        linux_platform = Path(linux_config["platform_sources"])
        platform_cflags = " ".join([
            abi,
            " ".join(target.platform_flags),
            f"-include {prefix_header}",
            *([f"-include {platform_semantics_header}"] if mingw else []),
            f"-I{posix_include}",
            f"-I{crt_include}",
            f"-I{linux_platform}",
            f"-I{PORT_DIR / 'include'}",
            f"-I{TOML_DIR}",
            f"-I{EXPAT_DIR}",
            f"-I{KCP_DIR}",
            f"-I{QRCODEGEN_DIR}",
            f"-I{MONOCYPHER_DIR}",
            f"-I{ZLIB_DIR}",
            # halo_linux_winsock_names.h, but not the Linux build's C runtime
            # wrappers next to it
            f"-iquote {LINUX_DIR / 'include'}",
            "-Isource -Isource/cseries",
            f"-I{_quote(sdl_include)}",
            f"-I{XDK_INCLUDE}",
        ])
        win32_cflags = " ".join([
            abi,
            " ".join(target.win32_flags),
            f"-I{posix_include}",
            f"-I{linux_platform}",
            f"-I{_quote(sdl_include)}",
        ])
        replaced = set(config.get("replaced_platform_sources", []))
        if target.renderer == "d3d11":
            replaced |= GL_RENDERER_SOURCES
        for source in sorted(linux_platform.glob("*.c")):
            if source.name in replaced:
                continue
            if source.name in VERSION_SOURCES:
                add_object(source, f"{platform_cflags} {updater_defines(getattr(sln, 'port_release', False))}")
            elif source.name == "posix_browser.c":
                # (the game list's requests: on Winsock, with Mbed TLS, as
                # on Linux)
                add_object(source, f"{win32_cflags} -I{MBEDTLS_DIR / 'include'}")
            elif source.name == "posix_ui_font.c":
                # (the overlay's fonts: stb_truetype; their data, tools/embed_assets.py --fonts)
                add_object(source, f"{platform_cflags} -I{STB_DIR}")
            else:
                add_object(source, platform_cflags)
        # the game list's TLS (port/third_party/mbedtls; posix_browser.c), on
        # Winsock
        if getattr(sln, "game_browser", False):
            for source in sorted((MBEDTLS_DIR / "library").glob("*.c")):
                add_object(source, " ".join([abi, *WIN32_FLAGS, f"-I{MBEDTLS_DIR / 'include'}",
                                             f"-I{MBEDTLS_DIR / 'library'}", "-D_CRT_SECURE_NO_WARNINGS", "-w"]))
        miniupnpc_include = f"-I{MINIUPNPC_DIR / 'include'} -DMINIUPNP_STATICLIB"
        for source in sorted((PORT_DIR / "src").glob("*.c")):
            if source.name == "win32_upnp.c":
                add_object(source, f"{win32_cflags} {miniupnpc_include}")
            else:
                add_object(source, win32_cflags if source.name.startswith("win32_") else platform_cflags)
        # internet play's UPnP (port/third_party/miniupnpc), on Winsock, as
        # its own build has it
        for source in miniupnpc_sources():
            add_object(source, " ".join([abi, *WIN32_FLAGS, miniupnpc_include, f"-I{MINIUPNPC_DIR / 'src'}",
                                         "-D_CRT_SECURE_NO_WARNINGS", "-D_WINSOCK_DEPRECATED_NO_WARNINGS", "-w"]))
        for source in embedded_assets:
            add_object(source, platform_cflags)
        # the settings file's parser (port/third_party/tomlc17), with the
        # platform layer's ABI and nothing else
        add_object(TOML_DIR / "tomlc17.c", " ".join([abi, "-std=gnu11", "-w"]))
        # the menus' XML parser (port/third_party/expat; menu_files.c), with
        # its hash salt from rand_s
        for name in EXPAT_SOURCES:
            add_object(EXPAT_DIR / name, " ".join([abi, "-std=gnu11", f"-I{EXPAT_DIR}", "-w"]))
        # internet play's reliable streams (port/third_party/kcp; p2p.c)
        add_object(KCP_DIR / "ikcp.c", " ".join([abi, "-std=gnu11", "-w"]))
        # Link Profile's QR code (port/third_party/qrcodegen; browser.c)
        add_object(QRCODEGEN_DIR / "qrcodegen.c", " ".join([abi, "-std=gnu11", "-w"]))
        # internet play's signatures, for public games' listings
        # (port/third_party/monocypher; p2p_crypto.c)
        for name in ("monocypher.c", "monocypher-ed25519.c"):
            add_object(MONOCYPHER_DIR / name, " ".join([abi, "-std=gnu11", "-w"]))
        # the port's zlib
        for name in ZLIB_SOURCES:
            add_object(ZLIB_DIR / name, " ".join([abi, "-std=gnu11", *ZLIB_DEFINES, "-w"]))
        # the game's sin, pow and the rest, the same on every port
        # (port/include/halo_math.h)
        for source in musl_math_sources():
            add_object(source, musl_math_cflags(abi))

        n.build(
            outputs=output,
            rule=f"{prefix}_link",
            inputs=objects + extra_objects,
            # (an SDL built here: before the link that names it)
            implicit=[sdl_import] if mingw else [],
            variables={"ldflags": " ".join(base_ldflags + extra_ldflags), "libs": libs},
        )

    # Profile-guided optimisation: with the committed Windows profile (or
    # the Linux one, which matches most of the game), or with --pgo=train
    # one that an instrumented build records while playing
    # (tools/pgo_train.py). A profile is trained once: code changed since
    # simply goes without, and deleting it trains a new one.
    profile = pgo_profile(sln, WINDOWS_PROFILE, [LINUX_PROFILE], cc) if target.pgo else None
    if pgo_mode(sln) == "train" and profile == WINDOWS_PROFILE:
        release = clang_release(cc)
        if not release:
            sys.exit(f"cannot tell the release of {cc}, whose profile runtime the instrumented build needs")
        runtime = fetch_profile_runtime(release)
        runtime_cflags = " ".join([
            "--target=i686-pc-windows-msvc", OPTIMISATION, "-g", "-gcodeview", "-w",
            "-D_CRT_SECURE_NO_WARNINGS", "-DCOMPILER_RT_HAS_ATOMICS=1",
            f"-I{_quote(runtime / 'include')}", f"-I{_quote(runtime / 'lib' / 'profile')}",
        ])
        generate_dir = BUILD / "pgo-generate"
        runtime_objects: List[Path] = []
        for name in PROFILE_RUNTIME_SOURCES:
            obj = generate_dir / "profile_runtime" / name.replace(".c", ".o")
            n.build(outputs=obj, rule="windows_cc", inputs=runtime / "lib" / "profile" / name,
                    variables={"cflags": runtime_cflags})
            runtime_objects.append(obj)
        # the runtime's start-up, which LLVM writes in C++
        obj = generate_dir / "profile_runtime" / "halo_profile_runtime.o"
        n.build(outputs=obj, rule="windows_cc", inputs=PORT_DIR / "pgo" / "halo_profile_runtime.c",
                variables={"cflags": runtime_cflags})
        runtime_objects.append(obj)
        instrumented = generate_dir / "halo.exe"
        instrumented_dll = generate_dir / "SDL3.dll"
        # instrumented objects ask for LLVM's own runtime library, which
        # these objects replace
        emit(generate_dir / "obj", instrumented, ["-fprofile-generate"],
             ["-Wl,/NODEFAULTLIB:clang_rt.profile.lib"], runtime_objects, [])
        n.build(outputs=instrumented_dll, rule="windows_copy", inputs=SDL_DIR / "lib" / "x86" / "SDL3.dll")
        n.rule(
            name="windows_pgo_train",
            command="$python tools/pgo_train.py --binary $binary --work $work --output $out",
            description="WINDOWS PGO TRAINING: playing levels in the instrumented build",
            pool="console",
        )
        n.build(
            outputs=profile,
            rule="windows_pgo_train",
            order_only=[instrumented, instrumented_dll],
            variables={"binary": str(instrumented), "work": str(sln.build_dir / "pgo" / "windows")},
        )

    # link-time optimisation (configure.py --lto)
    lto = lto_mode(sln)
    lto_cflags = [] if lto == "off" else ["-flto=thin" if lto == "thin" else "-flto=full"]
    emit(obj_dir, output, lto_cflags + profile_use_flags(profile),
         lto_cflags + [OPTIMISATION] if lto_cflags else [], [], [profile] if profile else [])
    n.build(outputs=sdl_dll, rule=f"{prefix}_copy", inputs=sdl_lib / "SDL3.dll")
    # internet play's MQTT brokers, a file beside the game (network.brokers_file)
    brokers = build / "brokers.txt"
    n.build(outputs=brokers, rule=f"{prefix}_copy", inputs=Path("port/assets/network/brokers.txt"))
    n.build(outputs=prefix, rule="phony", inputs=[output, sdl_dll, brokers])
    n.newline()
