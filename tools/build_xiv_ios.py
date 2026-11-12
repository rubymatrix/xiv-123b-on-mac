"""Build FINAL FANTASY XIV 1.23b for iPad and iPhone (an .app with xivboot inside), with Xcode's clang.

  python3 tools/build_xiv_ios.py objs [--sdk iphonesimulator|iphoneos]
        compile the runtime, the host shims and SDL3 for iOS without the game: a check that the
        sources build for iOS, before a patched ffxivgame.exe is at hand
  python3 tools/build_xiv_ios.py app --exe <ffxivgame.exe> [--sdk ...] [--bundle-id id]
        [--sign-identity name] [--profile file.mobileprovision]
        build/ios-<sdk>/FINAL FANTASY XIV.app
  python3 tools/build_xiv_ios.py run --exe <ffxivgame.exe> [--device name] [-- XIV_X=value ...]
        the simulator build, installed on a simulator (booted, else the named or the first iPad) and
        started with its log on this terminal. The simulator reads the Mac's files, so the game
        folder is the exe's own (XIV_GAME); NAME=VALUE pairs after -- go to the app's environment.
  python3 tools/build_xiv_ios.py install --exe <ffxivgame.exe> [--device name]
        the device build, signed (identity and profile found in the keychain and Xcode's profiles
        when not given), installed on a connected iPad or iPhone

The same sources as build_xiv.py's xivboot (PORTABLE, HOST, the translation in generated/ffxivgame)
compiled for arm64 iOS into build/ios-<sdk>/ (the Mac's objects are not touched), with SDL3's UIKit
tree (third_party/sdl3-ios) and tests/xivboot_ios.c as the entry; xivboot.c's main is renamed
xivboot_main. The translation keeps ThinLTO. A simulator build is signed ad hoc. A device build needs
a provisioning profile for the bundle id that allows the entitlements below; the ones it does not
grant are dropped. The game folder goes in the app's Documents (UIFileSharingEnabled): see
docs/ios-port.md.

XIV_IOS_GUEST_BASE=<hex>|none: the guest window's fixed host address (default build_xiv.py's
0x20000000000); none builds without RT_GUEST_FIXED (runtime/guest.h).
"""
import argparse
import concurrent.futures
import glob
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_xiv  # noqa: E402

ROOT = build_xiv.ROOT
MIN_IOS = '18.0'  # MTLCompileOptions.mathMode (gfx9_metal.m); every M-chip iPad runs 18
APP_NAME = 'FINAL FANTASY XIV'
EXE_NAME = 'xivboot'
APP_ID = 'local.xiv123b.anywhere'
SDL = 'sdl3-ios'
# M1 is the A14's core: every iPad with an M chip, and iPhone 12 and later
CPU = '-mcpu=apple-a14'
FRAMEWORKS = ['Metal', 'QuartzCore', 'Foundation']
WEAK_FRAMEWORKS = ['MetalFX']  # device builds only: the simulator's SDK has no MetalFX (gfx9_upscale_none.h)
ENTITLEMENTS = {
    'com.apple.developer.kernel.extended-virtual-addressing': True,
    'com.apple.developer.kernel.increased-memory-limit': True,
}


def run(cmd, **kw):
    print('> ' + ' '.join(map(str, cmd))[:200], flush=True)
    subprocess.run(list(map(str, cmd)), cwd=ROOT, check=True, **kw)


def xcrun(sdk, *what):
    return subprocess.run(['xcrun', '--sdk', sdk] + list(what), capture_output=True, text=True,
                          check=True).stdout.strip()


def cflags():
    """build_xiv.py's CFLAGS for iOS: no -mcpu=apple-m1, the guest base from XIV_IOS_GUEST_BASE"""
    base = os.environ.get('XIV_IOS_GUEST_BASE')
    out = []
    for f in build_xiv.CFLAGS:
        if f.startswith('-mcpu='):
            f = CPU
        elif f.startswith('-DRT_GUEST_FIXED=') and base:
            if base == 'none':
                continue
            f = '-DRT_GUEST_FIXED=%sull' % base
        out.append(f)
    return out


def gen_flags():
    return [CPU if f.startswith('-mcpu=') else f for f in build_xiv.GEN_FLAGS]


class Target:
    """One SDK's compilers and flags, and its output folder."""

    def __init__(self, sdk, bundle_id=APP_ID):
        self.sdk = sdk
        self.bundle_id = bundle_id
        self.out = os.path.join(ROOT, 'build', 'ios-' + sdk)
        triple = 'arm64-apple-ios' + MIN_IOS + ('-simulator' if sdk == 'iphonesimulator' else '')
        self.arch = ['-target', triple, '-isysroot', xcrun(sdk, '--show-sdk-path')]
        self.cc = [xcrun(sdk, '-f', 'clang')] + self.arch
        self.ar = xcrun(sdk, '-f', 'ar')

    def compile(self, sources, group, extra, our_headers=True):
        """Every source whose object is missing, older than its source or the newest runtime header,
        or built with other flags (a key per object), in parallel; returns the objects."""
        objdir = os.path.join(self.out, 'obj', group)
        os.makedirs(objdir, exist_ok=True)
        headers = build_xiv.newest_header() if our_headers else 0  # a library's objects do not see ours
        jobs, objs = [], []
        for s in sources:
            obj = os.path.join(objdir, s.replace('/', '_') + '.o')
            objs.append(obj)
            flags = list(extra)
            if s.endswith('.m'):  # ours count references by hand unless the group says ARC (SDL's)
                flags = [f for f in flags if f != '-std=c11'] + ([] if '-fobjc-arc' in flags else ['-fno-objc-arc'])
            cmd = self.cc + ['-c'] + flags + [s, '-o', obj]
            key = hashlib.sha256(json.dumps(cmd).encode()).hexdigest()
            stamp = obj + '.key'
            src = os.path.join(ROOT, s)
            fresh = (os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key
                     and os.path.getmtime(obj) >= max(os.path.getmtime(src), headers))
            if not fresh:
                jobs.append((cmd, stamp, key))
        if jobs:
            print('%s: compiling %d of %d' % (group, len(jobs), len(sources)), flush=True)

            def one(job):
                cmd, stamp, key = job
                r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
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

    def library(self, name):
        """third_party/<name> as an archive for this SDK: (archive, include flags, frameworks)"""
        lib = os.path.join(ROOT, 'third_party', name)
        with open(os.path.join(lib, 'manifest.json')) as f:
            m = json.load(f)
        archive = os.path.join(self.out, 'lib', 'lib%s.a' % name)
        os.makedirs(os.path.dirname(archive), exist_ok=True)
        objs = []
        rel = os.path.relpath(lib, ROOT)
        for n, g in enumerate(m['groups']):
            extra = (['-O2', '-DNDEBUG', '-w'] + g['flags']
                     + ['-I' + os.path.join(lib, d) for d in g['include']]
                     + sum((['-idirafter', os.path.join(lib, d)] for d in g['idirafter']), []))
            plain = [os.path.join(rel, s) for s in g['sources'] if not s.endswith('.m')]
            arc = [os.path.join(rel, s) for s in g['sources'] if s.endswith('.m')]
            objs += self.compile(plain, '%s-%d' % (name, n), extra, False)
            objs += self.compile(arc, '%s-%d-arc' % (name, n), extra + ['-fobjc-arc'], False)
        if not os.path.exists(archive) or any(os.path.getmtime(o) > os.path.getmtime(archive) for o in objs):
            if os.path.exists(archive):
                os.remove(archive)
            subprocess.run([self.ar, 'rcs', archive] + objs, check=True)
        frameworks = (sum((['-framework', f] for f in m.get('frameworks', [])), [])
                      + sum((['-weak_framework', f] for f in m.get('weak_frameworks', [])), []))
        return archive, ['-I' + os.path.join(lib, d) for d in m['public']], frameworks


def needs_translation(src):
    """whether a runtime source includes the translation's funcs.h (it builds only once there is one)"""
    with open(os.path.join(ROOT, src), errors='replace') as f:
        return '#include "funcs.h"' in f.read()


def runtime_objs(t, without_game=False):
    """the runtime, the host shims, xivboot.c (main renamed) and the iOS entry; and SDL.
    without_game: leave out the sources that need the translation's headers"""
    archive, inc, frameworks = t.library(SDL)
    ours = cflags() + inc
    sources = build_xiv.PORTABLE + build_xiv.HOST
    if without_game:
        later = [s for s in sources if needs_translation(s)]
        if later:
            print('left out until there is a translation: %s' % ', '.join(os.path.basename(s) for s in later))
        sources = [s for s in sources if s not in later]
    objs = t.compile(sources, 'runtime', ours)
    objs += t.compile(['tests/xivboot.c'], 'xivboot', ours + ['-Dmain=xivboot_main'])
    objs += t.compile(['tests/xivboot_ios.c'], 'entry', ours)
    return objs, archive, frameworks


def xivboot(t, exe):
    """build/ios-<sdk>/xivboot: the translation (ThinLTO) and the runtime"""
    if not os.path.exists(os.path.join(ROOT, build_xiv.GEN, 'table.c')):
        build_xiv.translate(exe)
    gen = sorted(os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, build_xiv.GEN, '*.c')))
    objs = t.compile(gen, 'gen', cflags() + gen_flags())
    more, archive, frameworks = runtime_objs(t)
    out = os.path.join(t.out, EXE_NAME)
    cache = os.path.join(t.out, 'lto-cache')
    weak = WEAK_FRAMEWORKS if t.sdk == 'iphoneos' else []
    fw = sum((['-framework', f] for f in FRAMEWORKS), []) + sum((['-weak_framework', f] for f in weak), [])
    build_xiv.run_lto(t.cc + ['-o', out, '-flto=thin', '-O3', CPU, '-Wl,-U,___llvm_profile_write_file',
                              '-Wl,-cache_path_lto,' + cache, '-Wl,-mllvm,-threads=%d' % (os.cpu_count() or 4)]
                      + objs + more + [archive] + frameworks + fw + ['-lm', '-lpthread'],
                      os.path.relpath(cache, ROOT), len(gen))
    return out


def info_plist(t):
    return {
        'CFBundleExecutable': EXE_NAME,
        'CFBundleIdentifier': t.bundle_id,
        'CFBundleName': APP_NAME,
        'CFBundleDisplayName': 'FFXIV 1.23b',
        'CFBundlePackageType': 'APPL',
        'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleShortVersionString': '1.23b',
        'CFBundleVersion': '1',
        'CFBundleSupportedPlatforms': ['iPhoneSimulator' if t.sdk == 'iphonesimulator' else 'iPhoneOS'],
        'MinimumOSVersion': MIN_IOS,
        'UIDeviceFamily': [1, 2],
        'LSRequiresIPhoneOS': True,
        'UIRequiresFullScreen': True,
        'UIStatusBarHidden': True,
        'UILaunchScreen': {},
        'UISupportedInterfaceOrientations': ['UIInterfaceOrientationLandscapeLeft',
                                             'UIInterfaceOrientationLandscapeRight'],
        'UISupportedInterfaceOrientations~ipad': ['UIInterfaceOrientationLandscapeLeft',
                                                  'UIInterfaceOrientationLandscapeRight'],
        # the player copies the patched game folder in with Finder or the Files app
        'UIFileSharingEnabled': True,
        'LSSupportsOpeningDocumentsInPlace': True,
        'LSApplicationCategoryType': 'public.app-category.role-playing-games',
        'GCSupportsControllerUserInteraction': True,
        'UIApplicationSupportsIndirectInputEvents': True,  # a mouse or trackpad's pointer and clicks
        'GCSupportedGameControllers': [{'ProfileName': 'ExtendedGamepad'}],
    }


def app(t, a):
    exe = xivboot(t, a.exe)
    bundle = os.path.join(t.out, APP_NAME + '.app')
    shutil.rmtree(bundle, ignore_errors=True)
    os.makedirs(bundle)
    shutil.copy(exe, os.path.join(bundle, EXE_NAME))
    with open(os.path.join(bundle, 'Info.plist'), 'wb') as f:
        plistlib.dump(info_plist(t), f, fmt=plistlib.FMT_BINARY)
    with open(os.path.join(bundle, 'PkgInfo'), 'w') as f:
        f.write('APPL????')
    if t.sdk == 'iphonesimulator':
        run(['codesign', '--force', '--sign', '-', '--timestamp=none', bundle])
    else:
        a.profile = a.profile or find_profile(t.bundle_id)
        a.sign_identity = a.sign_identity or find_identity()
        if not a.sign_identity or not a.profile:
            raise SystemExit('a device build needs a signing identity and a provisioning profile for %s '
                             '(--sign-identity, --profile)' % t.bundle_id)
        shutil.copy(a.profile, os.path.join(bundle, 'embedded.mobileprovision'))
        prof = plistlib.loads(subprocess.run(['security', 'cms', '-D', '-i', a.profile], capture_output=True,
                                             check=True).stdout)
        allowed = prof.get('Entitlements', {})
        ent = dict(ENTITLEMENTS)
        ent['application-identifier'] = allowed['application-identifier']
        ent['com.apple.developer.team-identifier'] = allowed['com.apple.developer.team-identifier']
        if allowed.get('get-task-allow'):
            ent['get-task-allow'] = True
        dropped = [k for k in ent if k not in allowed]
        for k in dropped:
            print('entitlement not in the profile, left out: %s' % k)
        ent = {k: v for k, v in ent.items() if k in allowed}
        path = os.path.join(t.out, 'entitlements.plist')
        with open(path, 'wb') as f:
            plistlib.dump(ent, f)
        run(['codesign', '--force', '--sign', a.sign_identity, '--entitlements', path, bundle])
    print('built %s' % bundle)
    return bundle


def find_profile(bundle_id):
    """an unexpired provisioning profile Xcode made for bundle_id, newest first"""
    import datetime
    found = []
    for d in ('~/Library/Developer/Xcode/UserData/Provisioning Profiles', '~/Library/MobileDevice/Provisioning Profiles'):
        d = os.path.expanduser(d)
        for f in os.listdir(d) if os.path.isdir(d) else []:
            path = os.path.join(d, f)
            r = subprocess.run(['security', 'cms', '-D', '-i', path], capture_output=True)
            if r.returncode:
                continue
            prof = plistlib.loads(r.stdout)
            app_id = prof.get('Entitlements', {}).get('application-identifier', '')
            expires = prof.get('ExpirationDate')
            if app_id.split('.', 1)[-1] == bundle_id and expires and expires > datetime.datetime.now():
                found.append((expires, path))
    return max(found)[1] if found else None


def find_identity():
    out = subprocess.run(['security', 'find-identity', '-v', '-p', 'codesigning'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if '"Apple Development:' in line:
            return line.split('"')[1]
    return None


def device(name):
    """the identifier of a connected, paired iPad or iPhone (the named one, else the first)"""
    out = os.path.join(ROOT, 'build', 'devices.json')
    subprocess.run(['xcrun', 'devicectl', 'list', 'devices', '--json-output', out], capture_output=True, check=True)
    with open(out) as f:
        devices = json.load(f)['result']['devices']
    for d in devices:
        props = d.get('deviceProperties', {})
        if d.get('hardwareProperties', {}).get('reality') != 'physical':
            continue
        if not name or name in (props.get('name'), d.get('identifier'), d.get('hardwareProperties', {}).get('udid')):
            return d['identifier']
    raise SystemExit('no device connected%s (cable or Wi-Fi, paired, Developer Mode on)' % (' named ' + name if name else ''))


def simulator(name):
    """the UDID of a booted simulator, else of the named (or the first) iPad, booted"""
    devs = json.loads(subprocess.run(['xcrun', 'simctl', 'list', 'devices', 'available', '-j'],
                                     capture_output=True, text=True, check=True).stdout)['devices']
    sims = [d for rt, ds in devs.items() if 'iOS' in rt for d in ds]
    booted = [d for d in sims if d['state'] == 'Booted' and (not name or d['name'] == name)]
    if booted:
        return booted[0]['udid']
    pick = [d for d in sims if (d['name'] == name if name else d['name'].startswith('iPad'))]
    if not pick:
        raise SystemExit('no simulator %s' % (name or 'iPad'))
    run(['xcrun', 'simctl', 'boot', pick[-1]['udid']])
    return pick[-1]['udid']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('target', choices=['objs', 'app', 'run', 'install'])
    ap.add_argument('--exe', help='the patched 1.23b ffxivgame.exe (its folder is the game folder for run)')
    ap.add_argument('--sdk', choices=['iphonesimulator', 'iphoneos'], default='iphonesimulator')
    ap.add_argument('--sign-identity')
    ap.add_argument('--profile')
    ap.add_argument('--device', help='simulator name for run, device name for install')
    ap.add_argument('--bundle-id', default=APP_ID)
    argv = sys.argv[1:]
    env_args = argv[argv.index('--') + 1:] if '--' in argv else []
    a = ap.parse_args(argv[:argv.index('--')] if '--' in argv else argv)
    if a.target == 'run':
        a.sdk = 'iphonesimulator'
    if a.target == 'install':
        a.sdk = 'iphoneos'
    t = Target(a.sdk, a.bundle_id)
    if a.target == 'objs':
        objs, archive, _ = runtime_objs(t, without_game=True)
        print('%d objects and %s for %s' % (len(objs), os.path.relpath(archive, ROOT), a.sdk))
        return
    if not a.exe:
        raise SystemExit('--exe <the patched ffxivgame.exe> is needed for %s' % a.target)
    a.exe = os.path.abspath(a.exe)
    bundle = app(t, a)
    if a.target == 'install':
        run(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', device(a.device), bundle])
    if a.target == 'run':
        udid = simulator(a.device)
        run(['xcrun', 'simctl', 'install', udid, bundle])
        env = dict(os.environ, SIMCTL_CHILD_XIV_GAME=os.path.dirname(a.exe))
        for kv in env_args:
            k, _, v = kv.partition('=')
            env['SIMCTL_CHILD_' + k] = v
        run(['xcrun', 'simctl', 'launch', '--console-pty', '--terminate-running-process', udid, t.bundle_id], env=env)


if __name__ == '__main__':
    main()
