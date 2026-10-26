# Multiplayer tests (VR + desktop, dedicated server)

The mod's multiplayer pieces:
- The VR channel (`src/se4/net.cpp`, R13 §3): a Hello/Ack handshake with version and build, and relayed VR poses.
- `ClientActionVR` from VR clients. It is automatic as a client of a remote server, so the server has head and hands
  for aiming and melee.
- Server-side melee sweeps for remote VR players.
- Guards on every peer for the engine's unguarded VR paths.

Desktop players install with `tools/install.py install <game> --mode desktop`, which writes `mp_guards=1`.

## 1. Local lab (one machine, isolated copies, no Steam)
`tools/lab/mp_lab.sh ss4|sm <out-dir> [dedicated|listen]` runs three gbe_fork lab copies over 127.0.0.1:
- instance 2, the server: flat, mod with desktop guards. It starts from its console at the main menu with
  `gamStartServer(level, "Cooperative", "Normal", 4)`, then the `gam_*` cvars, then `gamStart()`.
- instance 1, the VR client: lab Monado, `+gam_strConnectToIP 127.0.0.1`.
- instance 3, the desktop client: flat, mod with guards.

It prints each instance's `net:` lines and connection lines, captures both clients, and leaves everything running.
Stop it with `tools/lab/stop_lab.sh`.

Result, 2026-10-26, for SS4 Pompeii and SM Refinery co-op:
- All three joined.
- The VR client is marked as playing in VR. Its Hello was acknowledged, and its poses reached the server, which
  relayed them to the desktop client.
- Both clients see each other.
- There was no crash or kick.

Engine facts the script relies on:
- `gamStart()` alone starts single player. A command-line `+exec` runs before the game's shell functions exist.
- The main menu's "press any key" screen must be passed first.
- The console (F1) runs Lua in the shell. It is drawn in the flat window of a server or desktop instance.

## 2. Over the internet (to do; needs a second machine)
Goal: check that VR works against a remote server, not only on localhost.

1. **Server machine.** Use an isolated copy of the game with the gbe_fork emulator (`tools/lab/make_lab_tree.sh` on
   that machine, or the same layout made by hand). Start it as in §1, flat and dedicated.
2. **Network.** gbe_fork listens on UDP/TCP 47584 (`steam_settings/configs.main.ini`, `listen_port`). The game's own
   connection runs through the emulated Steam networking. Forward the gbe port at least, and check the game log
   for the port it actually opens. Sam4's `prj_uwPort` is not compiled in.
   - Discovery across the internet: list the server's public IP in `steam_settings/custom_broadcasts.txt` on each
     client.
   - Joining: the clients use `+gam_strConnectToIP <public IP>`.
3. **Clients.** One VR and one desktop, as in §1, connecting to the public IP. Check:
   - the `net:` handshake lines on both sides;
   - VR poses relayed with a sane rate (the server logs every 300th pose with `mp_log=1`);
   - hand-aimed shots hitting where the VR player aims, as seen on the server and the desktop client;
   - knife hits by the VR player counting on the server;
   - no desync, kick or crash for 10 minutes;
   - latency: compare the `seq` advance against time in the logs.
4. **Alternatives** if gbe_fork's internet path does not work: SmartSteamEmu
   (`/home/obesecatlord/Backup/Downloads/SmartSteamEmu147.7z`, in its own empty directory), or OnlineFix as a compromise.
   Never use the user's Steam install or saves.

## 3. Body, fingers and facing (lab, 2026-10-26)
`BODY=1 tools/lab/mp_lab.sh ss4|sm …` checks:
- **Facing.** The VR client turns about 180° with the right stick (native turning). The server's own view of the VR
  player's look changes accordingly: SS4 −2.36 → 0.79 rad, SM −2.95 → 0.11 rad. This is the mod's server log
  `net: server look of VR player`.
- **Arms and fingers.** The server keeps the desktop player 1.6 m in front of the VR player. The VR player holds its
  arms out with the left hand in a fist, and the desktop client's capture shows the VR player's arms following the
  controllers. The empty left hand closes with the grip and opens on release, in both games.
- **How:**
  - finger curls travel in the pose packet (`FingersValid`);
  - the third-person finger bones bend toward the palm, using a palm normal derived from the rest skeleton;
  - a hand holding a weapon keeps the native grip animation;
  - `body_ik` is now on by default, on desktop-guard peers too.

Note: the lab's "dedicated" server still spawns its own idle player ("playerid 16" in the logs; a third Sam stands at
the spawn). The engine's dedicated mode (`gam_bDedicatedServer`) does not stop that when the game is started through
`gamStartServer`/`gamStart`. It does not affect the tests.

## 4. Open
- The internet test (§2), which needs a second machine.
