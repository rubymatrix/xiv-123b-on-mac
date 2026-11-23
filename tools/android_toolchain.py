"""Paths shared by the Android tools (android_deps.py, build_xiv_android.py). From xi-on-anything's.

ANDROID_HOME is the SDK (default: the usual place on each system), ANDROID_NDK_HOME the NDK (default
the SDK's ndk/27.2.12479018), XIV_ANDROID_DEPS the dependency cache (default build/android-deps).
"""
import os
import platform
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _default_sdk():
    if sys.platform == 'darwin':
        return Path.home() / 'Library/Android/sdk'
    if sys.platform.startswith('win'):
        return Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'Android/Sdk'
    return Path.home() / 'Android/Sdk'


SDK = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or str(_default_sdk()))
NDK_VERSION = '27.2.12479018'
NDK = Path(os.environ.get('ANDROID_NDK_HOME', str(SDK / 'ndk' / NDK_VERSION)))
HOST = ('darwin-x86_64' if sys.platform == 'darwin' else  # one prebuilt for both Mac chips
        'windows-x86_64' if sys.platform.startswith('win') else 'linux-x86_64')
TC = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
EXE = '.exe' if sys.platform.startswith('win') else ''
CACHE = Path(os.environ.get('XIV_ANDROID_DEPS', str(ROOT / 'build/android-deps'))).resolve()
SDL = CACHE / 'SDL3-3.4.16'  # the source tree android_deps.py unpacks; its Java goes into the APK
API = 29  # ELF TLS (the runtime's thread-locals); API 28 emulates it


def cmake():
    """the SDK's CMake (it comes with Ninja), else one on the PATH"""
    for v in sorted((SDK / 'cmake').glob('*'), reverse=True):
        if (v / 'bin' / ('cmake' + EXE)).exists():
            return v / 'bin' / ('cmake' + EXE), v / 'bin' / ('ninja' + EXE)
    return Path(shutil.which('cmake') or 'cmake'), Path(shutil.which('ninja') or 'ninja')
