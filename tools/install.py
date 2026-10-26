#!/usr/bin/env python3
"""Install / uninstall the SS4+SM VR mod into a game directory, adding files only.

  install.py install   <game_dir> [--build build-win] [--mode vr|desktop]
  install.py uninstall <game_dir>
  install.py status    <game_dir>

Installs Bin/x64/msimg32.dll (proxy) and Bin/x64/ss4smvr/{ss4smvr.dll, libopenxr_loader.dll}, plus the mode's file:
--mode vr (default) creates the VR opt-in Bin/x64/ss4smvr/enable_vr; --mode desktop writes ss4smvr/options.txt with
mp_guards=1 for a flat player who joins VR players and desktop_upscaler=1, which adds "DLSS" / "FSR 3.1" to the game's
Options > Graphics > Rescaling Filter list (the choice is saved by the mod in ss4smvr/upscaler_menu.txt, a runtime file
that uninstall leaves like the log). Without either mode file the mod stays inert: flat play is unchanged.
Refuses to run
unless every game executable present in Bin/x64 matches a supported fingerprint, and refuses to overwrite
any file it did not install itself (receipt hash check). Uninstall removes only files whose current hash
equals the receipt. Never edits Steam settings: prints the Proton launch option instead.
"""
import argparse, hashlib, json, os, pathlib, shutil, sys, datetime

ROOT = pathlib.Path(__file__).resolve().parents[1]
RECEIPT = 'Bin/x64/ss4smvr/install-receipt.json'
FILES = {  # destination (relative to game dir) -> source (relative to build dir)
    'Bin/x64/msimg32.dll': 'msimg32.dll',
    'Bin/x64/ss4smvr/ss4smvr.dll': 'ss4smvr.dll',
    'Bin/x64/ss4smvr/libopenxr_loader.dll': 'openxr/src/loader/libopenxr_loader.dll',
}
DEPS = pathlib.Path.home() / 'LargeGames/Development/deps-src'
# Third-party runtime DLLs taken from the SDK checkouts (never committed to the repo):
#   nvngx_dlss.dll (NVIDIA DLSS SDK v310.9.16, redistributable with applications) next to the game exe, where
#   NGX looks for it; amd_fidelityfx_vk.dll (AMD FidelityFX SDK v1.1.4, MIT, FSR 3.1 Vulkan) next to the mod.
SDK_FILES = {
    'Bin/x64/nvngx_dlss.dll': DEPS / 'dlss-310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll',
    'Bin/x64/ss4smvr/amd_fidelityfx_vk.dll': DEPS / 'fidelityfx-sdk-1.1.4/PrebuiltSignedDLL/amd_fidelityfx_vk.dll',
}
EXES = ('Sam4.exe', 'Sam4_Unrestricted.exe', 'SamSM.exe', 'SamSM_Unrestricted.exe')


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def supported():
    fps = json.load(open(ROOT / 'docs/re/builds/fingerprints.json'))
    return {f['sha256']: f['file'] for f in fps}


def check_game(game):
    known = supported()
    found = []
    for exe in EXES:
        p = game / 'Bin/x64' / exe
        if p.exists():
            digest = sha256(p)
            if digest not in known:
                sys.exit(f'refusing: {p} is not a supported build (sha256 {digest[:16]}...). The mod targets '
                         'Serious Sam 4 build 616154 and Siberian Mayhem build 628492.')
            found.append(exe)
    if not found:
        sys.exit(f'refusing: no Serious Sam 4 / Siberian Mayhem executable in {game}/Bin/x64')
    return found


def load_receipt(game):
    p = game / RECEIPT
    return json.load(open(p)) if p.exists() else None


MODE_FILES = {  # generated per mode: destination -> content
    'vr': {'Bin/x64/ss4smvr/enable_vr': b''},
    'desktop': {'Bin/x64/ss4smvr/options.txt': b'mp_guards=1\ndesktop_upscaler=1\n'},
}


def install(game, build, mode='vr'):
    exes = check_game(game)
    receipt = load_receipt(game) or {'files': {}}
    sources = {dst: build / src for dst, src in FILES.items()}
    sources.update({dst: src for dst, src in SDK_FILES.items() if src.exists()})
    generated = MODE_FILES[mode]
    for dst, s in sources.items():
        if not s.exists():
            sys.exit(f'missing build output {s}; build with: cmake --build {build}')
    for dst in list(sources) + list(generated):
        d = game / dst
        if d.exists():
            mine = receipt['files'].get(dst)
            if not mine or mine != sha256(d):
                sys.exit(f'refusing: {d} exists and was not installed by this mod (or was modified since)')
    # The other mode's file goes (only if it is still ours, unmodified).
    for other, files in MODE_FILES.items():
        for dst in files if other != mode else ():
            d = game / dst
            if dst in receipt['files'] and d.exists() and sha256(d) == receipt['files'][dst]:
                d.unlink()
                del receipt['files'][dst]
    for dst, src in sources.items():
        d = game / dst
        d.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, d)
        receipt['files'][dst] = sha256(d)
    for dst, content in generated.items():
        d = game / dst
        d.write_bytes(content)
        receipt['files'][dst] = sha256(d)
    receipt['mode'] = mode
    receipt.update({'installed_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'executables': exes,
                    'source': str(ROOT)})
    (game / RECEIPT).write_text(json.dumps(receipt, indent=1))
    print(f'installed into {game} ({", ".join(exes)}), mode {mode}')
    print('Proton launch option (Steam > game > Properties > Launch Options):')
    print('  WINEDLLOVERRIDES="msimg32=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command% +gfx_strAPI Vulkan')
    print('Windows: no launch option needed (msimg32.dll next to the exe is loaded first).')


def uninstall(game):
    receipt = load_receipt(game)
    if not receipt:
        sys.exit('nothing to uninstall (no receipt)')
    kept = []
    for dst, digest in receipt['files'].items():
        d = game / dst
        if d.exists():
            if sha256(d) == digest:
                d.unlink()
            else:
                kept.append(dst)
    rp = game / RECEIPT
    if not kept:
        rp.unlink()
        try:
            (game / 'Bin/x64/ss4smvr').rmdir()
        except OSError:
            pass  # logs or user files remain
    else:
        receipt['files'] = {k: v for k, v in receipt['files'].items() if k in kept}
        rp.write_text(json.dumps(receipt, indent=1))
    print('uninstalled' + (f'; kept modified files: {kept}' if kept else ''))


def status(game):
    r = load_receipt(game)
    if not r:
        print('not installed')
        return
    for dst, digest in r['files'].items():
        d = game / dst
        state = 'ok' if d.exists() and sha256(d) == digest else ('modified' if d.exists() else 'missing')
        print(f'{state:8} {dst}')


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('action', choices=['install', 'uninstall', 'status'])
    ap.add_argument('game_dir')
    ap.add_argument('--build', default=str(ROOT / 'build-win'))
    ap.add_argument('--mode', choices=['vr', 'desktop'], default='vr')
    a = ap.parse_args()
    game = pathlib.Path(a.game_dir).expanduser().resolve()
    {'install': lambda: install(game, pathlib.Path(a.build), a.mode), 'uninstall': lambda: uninstall(game),
     'status': lambda: status(game)}[a.action]()
