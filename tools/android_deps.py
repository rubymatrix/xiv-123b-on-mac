"""Download and build the Android dependencies for ARM64: SDL3 3.4.16 (libSDL3.so and its Java), and
glslang 16.6.0 with its SPIR-V optimizer (SPIRV-Tools and SPIRV-Headers at the commits its
known_good.json names) as static libraries for the Vulkan back end. SDL's part is from xi-on-anything's
android_deps.py.

  python3 tools/android_deps.py [--jobs N]

Each source is pinned by SHA-256. SDL is the release third_party/sdl3 vendors for the Mac, and glslang
the release Homebrew gives the Mac's Vulkan build. Downloads and
builds stay in the dependency cache (XIV_ANDROID_DEPS, default build/android-deps).
"""
import argparse
import hashlib
import os
import subprocess
import tarfile
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from android_toolchain import API, CACHE, NDK, SDL, cmake

SDL_SHA256 = '7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68'  # release-3.4.16
GLSLANG = ('glslang', '16.6.0', 'https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz',
           '9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2')
SPIRV_TOOLS = ('SPIRV-Tools', 'ef96ed763b43b59b33b31b362f09a02b729fa1c9',
               'https://github.com/KhronosGroup/SPIRV-Tools/archive/ef96ed763b43b59b33b31b362f09a02b729fa1c9.tar.gz',
               '82c62146083fd558735a3171cf97cfc47903ca7d368482e87f94bd44883c0f00')
SPIRV_HEADERS = ('SPIRV-Headers', '496543121ce6419f23d6fa5d7194ba66c36212d2',
                 'https://github.com/KhronosGroup/SPIRV-Headers/archive/496543121ce6419f23d6fa5d7194ba66c36212d2.tar.gz',
                 'a9bb9c48713245eacf97cc539b6f1d45405a92d8813f8b82e635f0a085ee9898')
GLSLANG_OUT = CACHE / 'glslang-install'  # include/glslang, lib/*.a


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def download(url, archive, expected):
    if not archive.exists():
        partial = archive.with_suffix('.partial')
        urllib.request.urlretrieve(url, partial)
        if sha(partial) != expected:
            raise SystemExit('the download of %s does not match its pinned hash' % archive.name)
        partial.rename(archive)
    if sha(archive) != expected:
        raise SystemExit('the cached %s does not match its pinned hash' % archive.name)


def build_sdl(jobs):
    CACHE.mkdir(parents=True, exist_ok=True)
    archive = CACHE / (SDL.name + '.tar.gz')
    download('https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/' + archive.name, archive, SDL_SHA256)
    if not SDL.exists():
        with tarfile.open(archive) as data:
            data.extractall(CACHE, filter='data')
    out = CACHE / 'sdl-build'
    if (out / 'libSDL3.so').exists():
        print('SDL3 for Android: ready')
        return
    cm, ninja = cmake()
    subprocess.run([str(cm), '-S', str(SDL), '-B', str(out), '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + str(ninja),
                    '-DCMAKE_TOOLCHAIN_FILE=' + str(NDK / 'build/cmake/android.toolchain.cmake'),
                    '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-%d' % API,
                    '-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON', '-DCMAKE_BUILD_TYPE=Release',
                    '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TESTS=OFF', '-DSDL_TEST_LIBRARY=OFF'], check=True)
    subprocess.run([str(cm), '--build', str(out), '--parallel', str(jobs)], check=True)


def fetch(name, version, url, expected):
    """A pinned source tarball unpacked in the cache; returns its folder."""
    archive = CACHE / ('%s-%s.tar.gz' % (name, version))
    download(url, archive, expected)
    folder = CACHE / ('%s-%s' % (name, version))
    if not folder.exists():
        with tarfile.open(archive) as data:
            data.extractall(CACHE, filter='data')
    return folder


def build_glslang(jobs):
    if (GLSLANG_OUT / 'lib' / 'libglslang.a').exists():
        print('glslang for Android: ready')
        return
    CACHE.mkdir(parents=True, exist_ok=True)
    src = fetch(*GLSLANG)
    tools, headers = fetch(*SPIRV_TOOLS), fetch(*SPIRV_HEADERS)
    # where glslang's CMake looks for them (update_glslang_sources.py would clone them there)
    ext = src / 'External'
    ext.mkdir(exist_ok=True)
    if not (ext / 'spirv-tools').exists():
        (ext / 'spirv-tools').symlink_to(tools, target_is_directory=True)
    if not (tools / 'external' / 'spirv-headers').exists():
        (tools / 'external').mkdir(exist_ok=True)
        (tools / 'external' / 'spirv-headers').symlink_to(headers, target_is_directory=True)
    out = CACHE / 'glslang-build'
    cm, ninja = cmake()
    subprocess.run([str(cm), '-S', str(src), '-B', str(out), '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + str(ninja),
                    '-DCMAKE_TOOLCHAIN_FILE=' + str(NDK / 'build/cmake/android.toolchain.cmake'),
                    '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-%d' % API,
                    '-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON', '-DCMAKE_BUILD_TYPE=Release',
                    '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DCMAKE_INSTALL_PREFIX=' + str(GLSLANG_OUT),
                    '-DBUILD_SHARED_LIBS=OFF', '-DENABLE_OPT=ON', '-DENABLE_HLSL=OFF', '-DGLSLANG_TESTS=OFF',
                    '-DGLSLANG_ENABLE_INSTALL=ON', '-DENABLE_GLSLANG_BINARIES=OFF', '-DSPIRV_SKIP_TESTS=ON',
                    '-DSPIRV_SKIP_EXECUTABLES=ON', '-DSPIRV_TOOLS_BUILD_STATIC=ON'], check=True)
    subprocess.run([str(cm), '--build', str(out), '--parallel', str(jobs)], check=True)
    subprocess.run([str(cm), '--install', str(out)], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    if not (NDK / 'build/cmake/android.toolchain.cmake').exists():
        raise SystemExit('the Android NDK %s is not there (ANDROID_NDK_HOME)' % NDK)
    build_sdl(a.jobs)
    build_glslang(a.jobs)
    print('Android dependencies ready in', CACHE)


if __name__ == '__main__':
    main()
