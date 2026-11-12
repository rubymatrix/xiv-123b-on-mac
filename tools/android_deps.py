"""Download and build the Android dependencies for ARM64: SDL3 3.4.16 (libSDL3.so and its Java). From
xi-on-anything's android_deps.py; glslang comes with the Vulkan back end.

  python3 tools/android_deps.py [--jobs N]

The source is pinned by SHA-256, the same release third_party/sdl3 vendors for the Mac. Downloads and
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    if not (NDK / 'build/cmake/android.toolchain.cmake').exists():
        raise SystemExit('the Android NDK %s is not there (ANDROID_NDK_HOME)' % NDK)
    build_sdl(a.jobs)
    print('Android dependencies ready in', CACHE)


if __name__ == '__main__':
    main()
