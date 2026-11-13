"""Build FINAL FANTASY XIV 1.23b for Android (ARM64) with the NDK: libmain.so, then an APK, and put it on
a device with adb. See docs/android-port.md.

  python3 tools/android_deps.py                       SDL3 and glslang for Android, once
  python3 tools/build_xiv_android.py lib --exe <ffxivgame.exe>
        the translation (generated/ffxivgame, as the Mac's), the runtime and tests/xivboot_android.c
        into build/android/libmain.so
  python3 tools/build_xiv_android.py apk               build/android/xiv.apk, signed with a key made once
  python3 tools/build_xiv_android.py install           adb install
  python3 tools/build_xiv_android.py push-game --game <FINAL FANTASY XIV folder>
        the game into the app's files folder on the device
  python3 tools/build_xiv_android.py run [--args <run.args file>]    start it (run.args sent first)
  python3 tools/build_xiv_android.py log               the app's xivboot.log

The renderer is gfx9_vulkan.c, its shaders from sm_glsl.c through glslang (docs/android-port.md,
milestone 2). XIV_GFX=null builds with gfx9_null.c instead, which draws nothing.
XIV_ANDROID_GUEST_BASE=<hex>|none: the guest window's fixed address (default build_xiv.py's); none
builds without RT_GUEST_FIXED.
"""
import argparse
import concurrent.futures
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_xiv  # noqa: E402
from android_toolchain import API, CACHE, EXE, SDK, SDL, TC  # noqa: E402

ROOT = build_xiv.ROOT
OUT = os.path.join(ROOT, 'build', 'android')
PACKAGE = 'local.xiv123b.anywhere'
CC = str(TC / ('aarch64-linux-android%d-clang%s' % (API, '.cmd' if EXE else '')))
CPU = '-march=armv8.2-a'  # every phone of the last years (the Pixel 6a's X1 and A76 cores)
NATIVE = ['-fPIC', '-ffixed-x18']  # Android keeps x18 (the shadow call stack)
SDL_INCLUDE = str(SDL / 'include')
SDL_LIB = str(CACHE / 'sdl-build')
GLSLANG = CACHE / 'glslang-install'  # android_deps.py
VULKAN = os.environ.get('XIV_GFX') != 'null'
BACKENDS = ('runtime/portable/gfx9_null.c', 'runtime/portable/gfx9_vulkan.c', 'runtime/portable/sm_glsl.c')
# glslang's static libraries, users before what they use
GLSLANG_LIBS = ['-lglslang', '-lglslang-default-resource-limits', '-lMachineIndependent', '-lGenericCodeGen',
                '-lOSDependent', '-lSPIRV', '-lSPIRV-Tools-opt', '-lSPIRV-Tools']


def run(cmd, **kw):
    print('> ' + ' '.join(map(str, cmd))[:200], flush=True)
    subprocess.run(list(map(str, cmd)), check=True, **kw)


def cflags():
    base = os.environ.get('XIV_ANDROID_GUEST_BASE')
    out = []
    for f in build_xiv.CFLAGS:
        if f.startswith('-mcpu='):
            f = CPU
        elif f.startswith('-DRT_GUEST_FIXED=') and base:
            if base == 'none':
                continue
            f = '-DRT_GUEST_FIXED=%sull' % base
        out.append(f)
    return out + NATIVE


def compile_all(sources, group, extra):
    """each source whose object is missing or was built from other bytes or flags, in parallel"""
    objdir = os.path.join(OUT, 'obj', group)
    os.makedirs(objdir, exist_ok=True)
    headers = build_xiv.newest_header()
    jobs, objs = [], []
    for s in sources:
        obj = os.path.join(objdir, s.replace('/', '_') + '.o')
        objs.append(obj)
        cmd = [CC, '-c'] + extra + [s, '-o', obj]
        key = hashlib.sha256(json.dumps(cmd).encode()).hexdigest()
        stamp = obj + '.key'
        src = os.path.join(ROOT, s)
        fresh = (os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key
                 and os.path.getmtime(obj) >= max(os.path.getmtime(src), headers))
        if not fresh:
            jobs.append((cmd, stamp, key))
    if jobs:
        print('%s: compiling %d of %d' % (group, len(jobs), len(sources)), flush=True)
        done = [0]

        def one(job):
            cmd, stamp, key = job
            r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            done[0] += 1
            if os.environ.get('XIV_PROGRESS'):
                print('@progress compile-%s %d %d' % (group, done[0], len(jobs)), flush=True)
            if r.returncode:
                return cmd[-3], r.stderr
            with open(stamp, 'w') as f:
                f.write(key)
            return None
        with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
            failed = [e for e in ex.map(one, jobs) if e]
        for name, text in failed[:12]:
            print('%s:\n%s' % (name, text[-4000:]), file=sys.stderr)
        if failed:
            raise SystemExit('%s: %d failed' % (group, len(failed)))
    return objs


def lib(exe):
    if not os.path.exists(os.path.join(SDL_LIB, 'libSDL3.so')):
        raise SystemExit('SDL3 for Android is not built: python3 tools/android_deps.py')
    if not os.path.exists(os.path.join(ROOT, build_xiv.GEN, 'table.c')):
        build_xiv.translate(exe)
    gen = sorted(os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, build_xiv.GEN, '*.c')))
    gen_flags = [CPU if f.startswith('-mcpu=') else f for f in build_xiv.GEN_FLAGS]
    objs = compile_all(gen, 'gen', cflags() + gen_flags)
    if VULKAN and not (GLSLANG / 'lib' / 'libglslang.a').exists():
        raise SystemExit('glslang for Android is not built: python3 tools/android_deps.py')
    ours = cflags() + ['-I', SDL_INCLUDE, '-I', 'tests']
    portable = [s for s in build_xiv.PORTABLE if not s.endswith('.m')]  # app_icon.m: the Mac's Dock
    host = [s for s in build_xiv.HOST if not s.endswith('.m') and s not in BACKENDS]
    if VULKAN:
        host += ['runtime/portable/sm_glsl.c', 'runtime/portable/gfx9_vulkan.c']
        ours += ['-I', str(GLSLANG / 'include')]
    else:
        host += ['runtime/portable/gfx9_null.c']
    objs += compile_all(portable + host, 'runtime', ours)
    objs += compile_all(['tests/xivboot.c'], 'xivboot', ours + ['-Dmain=xivboot_main'])
    objs += compile_all(['tests/xivboot_android.c', 'tests/ios_touch.c'], 'entry', ours)
    out = os.path.join(OUT, 'libmain.so')
    cache = os.path.join(OUT, 'lto-cache')
    build_xiv.run_lto([CC, '-shared', '-o', out, '-flto=thin', '-O3', CPU, '-fPIC', '-Wl,-soname,libmain.so',
                       '-Wl,-z,max-page-size=16384', '-Wl,--no-undefined', '-Wl,--thinlto-cache-dir=' + cache,
                       '-Wl,--thinlto-jobs=%d' % (os.cpu_count() or 4)]
                      + objs + ['-L' + SDL_LIB, '-lSDL3']
                      + (['-L' + str(GLSLANG / 'lib')] + GLSLANG_LIBS + ['-lvulkan', '-l:libc++_static.a', '-l:libc++abi.a'] if VULKAN else [])
                      + ['-llog', '-landroid', '-lm', '-ldl'],
                      os.path.relpath(cache, ROOT), len(gen))
    print('built %s' % out)


# --- the APK ---------------------------------------------------------------------------------------
def build_tools():
    for v in sorted(glob.glob(str(SDK / 'build-tools' / '*')), reverse=True):
        if os.path.exists(os.path.join(v, 'd8' + ('.bat' if EXE else ''))):
            return v
    raise SystemExit('no Android build tools in %s' % SDK)


def platform_jar():
    for v in sorted(glob.glob(str(SDK / 'platforms' / 'android-*' / 'android.jar')), reverse=True):
        return v
    raise SystemExit('no Android platform in %s' % SDK)


def java_home():
    home = os.environ.get('JAVA_HOME')
    if home:
        return home
    for c in ('/Applications/Android Studio.app/Contents/jbr/Contents/Home',
              os.path.expanduser('~/android-studio/jbr'), r'C:\Program Files\Android\Android Studio\jbr'):
        if os.path.exists(os.path.join(c, 'bin', 'javac' + EXE)):
            return c
    javac = shutil.which('javac')
    if javac:
        return os.path.dirname(os.path.dirname(os.path.realpath(javac)))
    raise SystemExit('no Java (JAVA_HOME): Android Studio brings one')


def apk():
    lib_so = os.path.join(OUT, 'libmain.so')
    if not os.path.exists(lib_so):
        raise SystemExit('build the library first: build_xiv_android.py lib')
    bt, jar, jdk = build_tools(), platform_jar(), java_home()
    # d8 and apksigner are scripts that run the java on the PATH: this one
    os.environ['JAVA_HOME'] = jdk
    os.environ['PATH'] = os.path.join(jdk, 'bin') + os.pathsep + os.environ.get('PATH', '')
    with tempfile.TemporaryDirectory(prefix='apk-') as work:
        java = sorted(glob.glob(str(SDL / 'android-project/app/src/main/java/**/*.java'), recursive=True)
                      + glob.glob(os.path.join(ROOT, 'android/xiv/java/**/*.java'), recursive=True))
        classes = os.path.join(work, 'classes')
        os.makedirs(classes)
        run([os.path.join(jdk, 'bin', 'javac' + EXE), '-nowarn', '-source', '8', '-target', '8', '-bootclasspath', jar,
             '-d', classes] + java)
        dex = os.path.join(work, 'dex')
        os.makedirs(dex)
        run([os.path.join(bt, 'd8' + ('.bat' if EXE else '')), '--min-api', str(API), '--lib', jar, '--output', dex]
            + sorted(glob.glob(os.path.join(classes, '**', '*.class'), recursive=True)))
        unsigned = os.path.join(work, 'unsigned.apk')
        run([os.path.join(bt, 'aapt' + EXE), 'package', '-f', '-M', os.path.join(ROOT, 'android/xiv/AndroidManifest.xml'),
             '-I', jar, '-F', unsigned])
        stripped = os.path.join(work, 'libmain.so')
        shutil.copyfile(lib_so, stripped)
        run([str(TC / ('llvm-strip' + EXE)), '--strip-debug', stripped])
        with zipfile.ZipFile(unsigned, 'a', compression=zipfile.ZIP_DEFLATED) as z:
            z.write(os.path.join(dex, 'classes.dex'), 'classes.dex')
            z.write(os.path.join(SDL_LIB, 'libSDL3.so'), 'lib/arm64-v8a/libSDL3.so')
            z.write(stripped, 'lib/arm64-v8a/libmain.so')
        keystore = os.path.join(str(CACHE), 'xiv-signing.p12')
        if not os.path.exists(keystore):
            run([os.path.join(jdk, 'bin', 'keytool' + EXE), '-genkeypair', '-keystore', keystore, '-storepass', 'android',
                 '-keypass', 'android', '-alias', 'xiv', '-dname', 'CN=FFXIV 1.23b (this computer)', '-keyalg', 'RSA',
                 '-validity', '10000'])
        aligned = os.path.join(work, 'aligned.apk')
        run([os.path.join(bt, 'zipalign' + EXE), '-f', '-P', '16', '4', unsigned, aligned])
        out = os.path.join(OUT, 'xiv.apk')
        run([os.path.join(bt, 'apksigner' + ('.bat' if EXE else '')), 'sign', '--ks', keystore, '--ks-pass', 'pass:android',
             '--ks-key-alias', 'xiv', '--out', out, aligned])
    print('built %s' % out)


# --- the device --------------------------------------------------------------------------------------
FILES = '/sdcard/Android/data/%s/files' % PACKAGE


def adb(*args, check=True, capture=False):
    tool = shutil.which('adb') or str(SDK / 'platform-tools' / ('adb' + EXE))
    return subprocess.run([tool] + list(args), check=check, capture_output=capture, text=True)


def push_game(game):
    """the game folder into the app's files, as one tar stream unpacked by the device's tar (adb
    exec-in): adb push of the whole folder hung before sending anything (215,000 files). Run the app
    once first, so the folder is its own. Progress: @progress push <files> <total>."""
    import tarfile
    names = []
    for d, dirs, files in os.walk(game):
        dirs.sort()
        for f in sorted(files):
            if f != '.DS_Store':
                names.append(os.path.join(d, f))
    total = len(names)
    tool = shutil.which('adb') or str(SDK / 'platform-tools' / ('adb' + EXE))
    p = subprocess.Popen([tool, 'exec-in', 'sh', '-c', 'mkdir -p "%s" && cd "%s" && tar -xf -' % (FILES, FILES)],
                         stdin=subprocess.PIPE)
    base = os.path.dirname(game)
    with tarfile.open(fileobj=p.stdin, mode='w|', format=tarfile.PAX_FORMAT) as tar:
        for i, path in enumerate(names):
            tar.add(path, arcname=os.path.relpath(path, base), recursive=False)
            if i % 2000 == 0:
                print('@progress push %d %d' % (i, total), flush=True)
    p.stdin.close()
    if p.wait():
        raise SystemExit('the device could not unpack the game (adb exec-in tar)')
    print('@progress push %d %d' % (total, total), flush=True)
    print('pushed %d files' % total)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('target', choices=['lib', 'apk', 'install', 'push-game', 'run', 'log'])
    ap.add_argument('--exe')
    ap.add_argument('--game')
    ap.add_argument('--args', help='a run.args file sent before starting')
    a = ap.parse_args()
    if a.target == 'lib':
        if not a.exe:
            raise SystemExit('lib needs --exe <the patched ffxivgame.exe>')
        lib(os.path.abspath(a.exe))
    elif a.target == 'apk':
        apk()
    elif a.target == 'install':
        adb('install', '-r', os.path.join(OUT, 'xiv.apk'))
    elif a.target == 'push-game':
        if not a.game:
            raise SystemExit('push-game needs --game <FINAL FANTASY XIV folder>')
        push_game(os.path.abspath(a.game))
    elif a.target == 'run':
        if a.args:
            adb('push', a.args, FILES + '/run.args')
        adb('shell', 'am', 'start', '-S', '-n', '%s/.MainActivity' % PACKAGE)
    elif a.target == 'log':
        print(adb('shell', 'cat', FILES + '/xivboot.log', check=False, capture=True).stdout)


if __name__ == '__main__':
    main()
