#!/usr/bin/env python3
"""Summarise an ss4smvr.log as a pass/fail checklist of the VR pipeline stages.

  check_log.py [log]   (default: the Serious Sam 4 install's Bin/x64/ss4smvr/ss4smvr.log)

Each stage lists the log evidence it needs. "FAIL" means a stage reported an error, "missing" means its
evidence never appeared (the stage did not run or the run ended first). Exit status 1 if any stage failed.
"""
import pathlib, re, sys

DEFAULT = pathlib.Path.home() / 'FasterGames/SteamLibrary/steamapps/common/Serious Sam 4/Bin/x64/ss4smvr/ss4smvr.log'

# (stage, required pattern, failure patterns)
STAGES = [
    ('payload attached', r'ss4smvr payload attached', []),
    ('VR opt-in', r'attach: VR enabled', [r'attach: VR disabled']),
    ('build verified', r'build: .* verified', [r'build: unknown executable', r'crc .* VR stays inactive']),
    ('GetProcAddress hook', r'GetProcAddress hook installed', [r'GetProcAddress hook FAILED']),
    ('player_vr guards', r'vrd_init slot 0x208 calls at', [r'player_vr: .*FAILED', r'not patched', r'lacks VR player symbols']),
    ('OpenXR runtime', r'xr: runtime ', [r'XR_KHR_vulkan_enable2', r'no OpenXR system']),
    ('Vulkan via XR', r'xrCreateVulkanDeviceKHR -> 0 \(multiview on', [r'xrCreateVulkanDeviceKHR -> -', r'different physical device']),
    ('device installed', r'COpenXRDevice installed', [r'device global already set']),
    ('session focused', r'xr: session state 5', [r'XR session unavailable', r'instance loss']),
    ('controllers', r'input: (left|right) hand profile /interaction_profiles/', [r'input: attach failed']),
    ('virtual gamepad polled', r'gamepad: first game poll of user 0', []),
    ('eye render targets', r'vrdev: eye 2 render target', []),
    ('eye images captured', r'eyes: eye 2 image', [r'eyes: no image created']),
    ('stereo projection', r'stereo: submitting engine-rendered eyes', []),
    ('VR player enabled', r'marked as playing in VR', []),
    ('local CPlayerVRData', r'CPlayerVRData .* \(local 1\)', []),
]


def main():
    path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    text = path.read_text(errors='replace')
    lines = text.splitlines()
    bad = False
    print(f'{path} ({len(lines)} lines)')
    for name, need, fails in STAGES:
        hit = re.search(need, text)
        fail = next((m for f in fails if (m := re.search(f, text))), None)
        state = 'FAIL' if fail else ('ok' if hit else 'missing')
        bad |= state == 'FAIL'
        detail = (fail or hit).group(0) if (fail or hit) else ''
        print(f'  {state:8} {name:24} {detail[:90]}')
    fps = re.findall(r'mirror: ([0-9.]+) fps', text)
    if fps:
        vals = [float(v) for v in fps]
        print(f'  frame rate: last {vals[-1]:.1f} fps, min {min(vals):.1f}, max {max(vals):.1f} over {len(vals)} reports')
    slots = sorted(set(re.findall(r'first call slot (0x[0-9a-f]+)', text)))
    if slots:
        print('  device slots called:', ' '.join(slots))
    opts = re.findall(r'option (\S+) = (-?\d+)', text)
    if opts:
        print('  options:', ', '.join(f'{k}={v}' for k, v in opts))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
