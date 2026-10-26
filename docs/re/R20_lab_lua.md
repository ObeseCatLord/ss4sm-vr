# Gate R20: running a Lua snippet in the running world, for the lab (Sam4.exe 616154, SamSM.exe 628492)

Static analysis only. Sources:
- A private scratch copy of the `lab/ghidra_r19` Sam4.exe project (`tools/ghidra/DecompileList.java`).
- capstone (`tools/disasm.py`), `callers.py`, `riprefs.py`, `strrefs.py`, `substrrefs.py`, `cvar_readers.py`,
  `vtable_slot.py` and the harvest JSONs.
- SM and Unrestricted addresses come from `tools/port_sites.py` (a unique wildcarded signature), `tools/sig_at.py`
  (full masked match) or the call order inside an already-ported function. The method is stated per row.
- The game's own Lua in `lab/lua` (private), used only to see how content reaches `worldInfo` and players.

Nothing was run and nothing was committed. No game file, setting or save was touched. Addresses are Sam4.exe PE VAs
unless marked **SM**. Confidence tags follow R17:
- **[D]**: read from a decompile or disassembly.
- **[M]**: from reflection, harvest or content.
- **[I]**: inference that needs a runtime check.

Abbreviations:
- "lsi": `CLuaScriptInterface` (R13 §1.3).
- "ctx": the per-thread engine context. `ctx+0xa8` is the lsi currently executing.
- "shell": the global, non-world lsi.
- "world lsi": the lsi of a world, kind 1.

## 0. Answers

1. **The in-game console exists in both final exes and is not disabled.** **F1** (key id 0x51) or **Tilde**
   (key id 0x61, the `` ` ``/`~` key) toggles it. The toggle is hardcoded in the project instance's key handler
   (pi vt+0x70). The default system bindings also map both keys to `syscmdActivateConsole`. **[D]**
   - Alt held suppresses the toggle **[I]**.
   - The only gate is a `.bss` global that has no writer (it is always 0). **[D]**
   - `Sam4_Unrestricted.exe` and `SamSM_Unrestricted.exe` contain the identical handler and gate. "Unrestricted" is
     the moddable/Workshop variant (`_unrestricted` save/session suffixes). It is not a console switch. **[D][M]**
2. **Console input is Lua**, but it runs in the **shell lsi, not the world**.
   - Each line is first tried as `return <line>` (so the value is printed), then run as a statement. **[D]**
   - The shell has full read permission (level 4). Its globals hold cvars and engine functions only. **There is no
     `worldGlobals`, no `worldInfo` and no way to obtain an entity handle.** So the `prjStartGenericWeaponAnimation`
     example cannot be typed into the console. **[D]**
   - A console line `exec <file>` runs a Lua file, also in the shell. **[D]**
3. **Startup scripts exist, but all of them run in the shell.** None runs code in a world, and there is no
   per-level script cvar. **[D][M]**
   - `UserCfg.lua`: `scr_strUserCfg`, enabled by default with `scr_bUseUserCfg = 1`.
   - `prj_strExecCfg`: its record carries the short name `exec`. Set it with `+prj_strExecCfg <file>`.
   - The developer "dev scripts" path *does* run a file in the world lsi. In both final configurations its entry
     points (`AddDevScriptBinding`, `scrExecuteDevScriptIfExists`) are compiled to stubs, so it cannot be reached
     without native code. **[D]**
4. **Mod-side bridge (recommended).** The engine has a ready "run this Lua source string in that lsi" function,
   **`lua_exec_string` 0x140b748a0** (SM **0x140b74e10**). It is what the dev-script path, the console and the
   engine's own one-liners use.
   - Signature: `int exec(lsi*, ctx*, const char* src, uint flags)`.
   - It brackets `ctx+0xa8` and calls lsi vt+0x98.
   - In a world lsi the chunk becomes a **scheduler coroutine**. It runs in the content scripts' protected
     environment (`worldGlobals`, `Wait`, `RunAsync`, `import`, `Depfile`, all engine functions), under the engine's
     instruction-count timeout and error logging.
   - The world lsi is `world_to_lsi(world)`: 0x140b79820, SM 0x140b79e70, `*(*(world+0x130)+0x58)`.
   - The world is `*(player+0x30)`.
   - The context comes from `script_ctx()`: 0x1411223e0, SM 0x141122ba0.
   - Run it once on the simulation thread from the existing local-player tick hook (§5). The raw Lua API (R13) is a
     fallback, but it gives no `Wait()`.

## 1. The in-game console

### 1.1 How it opens [D]
- **Default system bindings.** S4 0x140154cf0, SM 0x140155990 (port_sites). They bind `"Tilde"` and `"F1"`
  (`Keyboard+Mouse`) to the IDENT `syscmdActivateConsole` (global 0x142275b60), `F10` to `syscmdInvokeMenu`, and
  `Escape` to `InvokeMenuExit`.
- **Command registry.** 0x1401575b0 registers "ETRSPlayerCommand.ActivateConsole=Activate console".
- **The actual toggle is hardcoded.** It is the project-instance key handler **pi vt+0x70 = 0x140053030** (SM
  **0x140053090**, sig_at full match):

  ```text
  if ev.type == 0x13 (key down)
     && !(windowSystem->vt[0x218]() & (1<<29))        // 0x14239c1c0 = window system ("WindowSystem.Standard")
     && (ev.key == 0x51 || ev.key == 0x61)            // (key-0x51) & ~0x10 == 0
     && g_consoleBlock == 0:                          // S4 0x142273a34, SM 0x142250b18
        console->SetActive(!console->IsActive())      // console = *(pi+0x70)
  else if console active: key goes to the console (Escape with ev[1]!=0 excepted)
  ```

- **Key ids** come from the key-name table builder 0x141193010. `F1` stores 0x51 (F2..F4 = 0x52..0x54), `Tilde`
  stores 0x61 (characters `` ` `` and `~`), and `Escape` stores 0x60. **[D]**
- **Bit 29** of window-system vt+0x218 matches Win32 lParam bit 29 (Alt) **[I]**. In practice, do not hold Alt.
- **`g_consoleBlock`** (S4 0x142273a34 / SM 0x142250b18 / Sam4_Unrestricted 0x142273a34 / SamSM_Unrestricted
  0x14224fb18):
  - It is in `.bss` (zero-initialised).
  - Its **only** reference in each exe is the read at 0x140053077 (SM 0x1400530d7).
  - It has no RIP-relative store, no absolute or RVA pointer, no lea base nearby and no cvar record.
  - It is therefore always 0, and the console toggles. **[D]**
- **Console object** (the pi member at +0x70, created in the pi ctor 0x14004b5a0, SM 0x14004b5f0, as the `"local"`
  `"GameConsole"` client):
  - IsActive 0x140aaee70 (SM 0x140aaf5e0) = `+0x108`;
  - SetActive 0x140ab0640 (SM 0x140ab0da0);
  - render 0x140aaf480 (SM 0x140aafbf0). Its cvars are `con_fHeightRatio` and `con_iLastLines`.
- **Pause.** The game loop (0x140058ab9 at 0x140059a17) pauses while the console is active unless
  `prj_bDisableConsolePause` (S4 0x1422736a0, SM 0x142250740) ≠ 0. **For the lab: close the console before
  expecting simulation, or pass `+prj_bDisableConsolePause 1`.** **[D]**

### 1.2 Console input is Lua in the shell [D]
- The console is a client of the RCon command server (`"RConServer"` timer, init 0x140e62eb0, SM 0x140e63d30).
  Its per-frame processor **0x140e63da0** (SM **0x140e64c30**) handles a typed line:
  1. It logs `"> %1"` and skips lines that look like `#…(` tags.
  2. It offers the line to registered command filters. There is one: **0x140e62840** (SM 0x140e636c0), which
     parses `exec %1` and runs that file with `shell_exec_file(path, 8)`.
  3. Otherwise it runs `lua_exec_string_cur(shell, "return " .. line .. ";", 3)` (0x140b75130, SM 0x140b756a0). If
     that fails, it runs `lua_exec_string_cur(shell, line, 1)`.
  4. History and favourites are stored in `temp:/RCon/History|Favorites/%1.txt`.
- **Shell lsi.** It is a global: **S4 0x1422ab068, SM 0x142288228**. The getter is 0x140b75370 (SM 0x140b758e0).
  - It is created by 0x140b74e90 (SM 0x140b75400) with `Init(0)`, so kind 0.
  - It is stepped by the `"StepScriptInterface"` timer 0x140b75580, which uses the main-thread ctx global
    0x14239c4e0.
  - The same init registers **`"remotecall "`** window-system messages (handler 0x140b74e10, SM 0x140b75380, message
    type 0x14/0x17). Their text is also executed in the shell. The transport behind this is unknown.
- **Permissions** (globals `__index` 0x140fe2c10, SM 0x140fe3760). The level comes from the lsi kind through table
  0x141f9c060 = `{kind0: 4, kind1 (world): 2, kind2: 4, kind3: 0}`.
  - Entries that are neither world (type flags 0xc000) nor script-visible (0x10002) read as nil.
  - World data needs level ≥2. Plain cvars need ≥3, other data ≥4.
  - A global at 0x1422ab020 (no writer found, no cvar record) bypasses all of this.
  - So the shell (4) may *name* world functions. The world lsi (2) cannot read most cvars.
- **What the shell sees.** Globals setup is 0x140e79200 (SM via its lsi). For kind ≠ 1 it creates only `globals`. The
  kind-1 path that creates `worldGlobals`, the protected environment and the persisted tables is skipped.
  - The shell therefore has **no `worldGlobals` and no `worldInfo`**.
  - No global (non-method) script function that returns an entity or world was found by name: none of the 666
    harvested globals is a `GetWorldInfo` or `GetLocalPlayer`, and `GetWorldInfo` exists only as the method
    `CBaseEntity::GetWorldInfo` **[M]**.
  - Shell-level lab use is limited to cvars and console commands such as `cht_bEnableCheats = 1`, `chtGiveWeapon`,
    `gamListPlayers` (flags 0x10002), `prjRemoteSamCommand` (below) and `ExecuteCommands`. The last runs in the
    *current* lsi, which is the shell here.
- **`prjRemoteSamCommand(str)`** (0x140062140, SM 0x1400622e0) reaches the world lsi. It signals the world script event
  `"RemoteSam"` with the string as payload, and only if a world script waits for it. No waiter appears in the
  extracted subset of content scripts (`lab/lua`), so it is not a code channel **[M]**.

### 1.3 Files at startup (all shell) [D][M]

| mechanism | S4 | SM | behaviour |
|---|---|---|---|
| `UserCfg.lua`: `scr_strUserCfg` (CString, default `"UserCfg.lua"`) + `scr_bUseUserCfg` (BOOL, **default 1**) | runner 0x140b73f10; cvars 0x1422ab000 / 0x14224cc44 | 0x140b744a0; 0x1422881c0 / 0x14222b9f4 | run during app init (0x14003b4a0, after the .ini). If the file is missing, a default commented script is created at that path (logs "User startup config not found in %1. Creating default script.") |
| `prj_strExecCfg` (CString, record short name `exec`) | 0x142273428 | 0x1422504c8 | if non-empty at app init: `shell_exec_file(path, 0xc)` (unsigned file allowed). Set with `+prj_strExecCfg <file>`, the lab's existing `+cvar` mechanism. Whether `+exec <file>` is accepted as an alias is **[I]** |
| `prj_strOptionsScript` | 0x142273778 | 0x142250818 | dedicated server only (0x140056d20, "Loading options from %1") |
| console `exec <file>` | 0x140e62840 | 0x140e636c0 | shell, flags 8 |
| `scrExecuteCommandsFromFile(path)` | 0x140b74200 | 0x140b74790 | `exec_file_lsi(ctx->cur, path, 0)`: the *current* lsi, so the shell from the console, or the world when called from a world script |
| `scrExecuteFileGlobal_unsigned(path)` | 0x140b74250 | 0x140b747e0 | shell, flags 8 |
| `psp_aaecfAutoExecConfigFiles` (ProjectStartupParams) | record 0x141e03200 | — | resource-side autoexec list, not a user switch |
| dev scripts (`CMSDevScripts` menu, `syscmdInvokeScriptsMenu`) | runner 0x140859800 | 0x1408596c0 | **world lsi**, but `AddDevScriptBinding` (0x140858c20 / SM 0x140858a70) and `scrExecuteDevScriptIfExists` (0x140b743c0 / SM 0x140b74950) only read their arguments. The bindings list (0x14228f5d8) stays empty in all four exes |

- **File modes.** `exec_file_lsi` (below) opens with mode `"r"`, or `"rU"` when flags & 8. The mode parser
  0x14115f120 gives `.lua` under `"r"` signature level 4 and gives `"rU"` level 0 (no signature).
- Whether the final exe refuses an unsigned loose `.lua` at level 4 was not traced to the end. Avoid the question:
  the bridge reads the file itself (§5). **[I]**
- No cvar runs a script when a level starts. WorldScripts are started by `CWorldInfoEntity` start 0x1403d4020
  (`<world>/Scripts/WorldScripts.lua`, R13). Overriding that file with a loose copy is possible in principle, but it
  would change official content, so it is rejected.

## 2. Mod-side bridge

### 2.1 The engine's own "run Lua in a world" path [D]
The developer-script runner **0x140859800** (SM 0x1408596c0) is exactly the wanted operation, done for a file:

```c
pi    = menu_pi(menu);                        // 0x140148b70: *(*(menu+0xb8)+0x30)
world = pi_to_world(pi);                      // 0x140b7cc30 (SM 0x140b7d070)
if (!world) return 0;
lsi   = world_to_lsi(world);                  // 0x140b79820 (SM 0x140b79e70): *(*(world+0x130)+0x58)
if (file_exists(path) && exec_file_lsi(lsi, path, 0)) return 1;   // 0x14111c520, 0x140b74950 (SM 0x140b74ec0)
log("Failed to execute dev script %1\n");
```

**`exec_file_lsi(lsi, const char* path, uint flags)`** 0x140b74950 (SM 0x140b74ec0):
1. It opens the file through the VFS with `"r"` or `"rU"` (above) and reads all of it.
2. It prefixes the chunk-name marker `"--[[@<path>-@]] "` (0x140d92670).
3. It calls `lua_exec_string(lsi, script_ctx(), text, flags)`.

### 2.2 `lua_exec_string`: contract [D]
`int lua_exec_string(CLuaScriptInterface* lsi, void* ctx, const char* src, uint32_t flags)`, at **0x140b748a0** (SM
**0x140b74e10**, Sam4_Unrestricted 0x140b76fd0, SamSM_Unrestricted 0x140b77530):

```text
old = ctx->+0xa8;  ctx->+0xa8 = lsi;
ok  = lsi->vt[0x98](lsi, ctx, src, &params /* {ptr=0,count=0}, 0x10-byte entries */, flags);
ctx->+0xa8 = old;  free(params);  return ok;
```

- `src` is a plain NUL-terminated `const char*`. The engine passes literals, e.g. `"globals.GetDeathHints = nil"`
  in 0x140ab13a0 and `"crmReportCrumbsSize()"`.
- lsi vt+0x98 = **0x140e807d0** (SM 0x140e816d0) → scheduler **0x140e77d00** (SM 0x140e78be0):
  1. Recursion guard: depth >0x13 replaces the chunk with an assert.
  2. `luaL_loadstring(L, src)`. On a compile error it logs `"Lua error> %1"` unless `flags & 2`, then pops.
  3. Otherwise **0x140e77e80** runs it:
     - `lua_newthread`, with a count hook (0x140e7dac0, 100000 instructions; "Lua script execution was too
       long! Stopping the script!");
     - the coroutine is registered with the scheduler;
     - for a world lsi (scheduler kind 1) with no params, the chunk environment is the shared
       **`_CT_LUAMT_PROTECTEDENVIRONMENT_`** (0x140e77c80, SM 0x140e78b60);
     - `lua_resume`. If the chunk yields (`Wait(...)`), the scheduler resumes it on later lsi steps. If it finishes,
       the coroutine is cleaned up (0x140e78840).
- Return: nonzero when the chunk compiled and its first resume did not fail.
- `flags`:
  - bit 1 (2) silences compile errors (the console's `return` attempt uses 3);
  - the console's statement run uses 1, and file loads use 0, 8 or 0xc;
  - the exact meanings of 1, 4 and 8 inside the scheduler are unknown. **Pass 0.**
- **What the chunk sees** (protected environment builder 0x140e78120):
  - copied names: `worldGlobals`, `globals`, `Event(s)`, `CustomEvent`, `Delay`, `OnEvery`, `RunHandled`,
    `BreakableRunHandled`, `BreakRunHandled`, `SignalEvent`, `WaitForever`, `RunAsync`, `Times`, `CallOnGroup`,
    `IsDeleted`, `NewGroupVar`, `dofile`, `loadfile`, `import`, `print`, `ipairs`, `pairs`, `tonumber`, `tostring`,
    `unpack`, `assert`, `Depfile`, `table`, `string`, `HasMember`, `CallScriptRPC`, `RegisterNetworkDescriptor`,
    `pcall`;
  - everything else, including `Wait` and every `prj*`/`cht*` function, resolves through the
    `_CT_LUAMT_GLOBALINDEX_` metatable at permission level 2;
  - **`worldInfo` is not a global.** Use `worldGlobals.worldInfo` (set by WorldScripts.lua line 1). There is no
    `GetWorldInfo()` global; use `entity:GetWorldInfo()`. **[D][M]**
- Use `local` for every variable. Writes to `worldGlobals` end up in savegames (Pluto, R13 §1.3). So does a chunk
  coroutine that is still waiting when a save happens. The lab has `saves.sh snapshot/restore` for this.

### 2.3 Getting the lsi from the local player [D]
- `world = *(entity + 0x30)`. This is the same load as `CEntity::NetIsServer` (R13) and R17's `state_world`.
- `lsi = world_to_lsi(world)` (0x140b79820 / SM 0x140b79e70). Check that `lsi` is non-null and that
  `*(int*)(lsi+8) == 1` (kind getter vt+0x18 = 0x14008d380 returns `+8`).
- `ctx = script_ctx()` (0x1411223e0, SM 0x141122ba0; TLS index global 0x1423c3638 / SM 0x1423a0318). Call it on the
  thread that runs the chunk and never cache it across threads.
- The alternative from a pi uses `pi_to_world` (0x140b7cc30 / SM 0x140b7d070), as §2.1 does.

### 2.4 Raw Lua API fallback (R13 table, cross-checked here) [D]
Use this only if lsi vt+0x98 is unsuitable. It has no coroutine, so `Wait()` errors. It runs under the real `_G`,
where `globals` = `worldGlobals`, and has no timeout hook.

```text
L = *(lua_State**)(lsi + 0x38);  top = lua_gettop(L);
old = ctx->+0xa8; ctx->+0xa8 = lsi;                 // engine C functions check it (R13)
if (luaL_loadbuffer(L, buf, len, "=lab_lua") == 0) lua_pcall(L, 0, 0, 0);
else / on error: log lua_tolstring(L, -1, 0);
lua_settop(L, top); ctx->+0xa8 = old;
```

| function | Sam4 | SamSM | evidence |
|---|---|---|---|
| luaL_loadbuffer | 0x140fd8f30 | 0x140fd9860 | R13 (H) |
| luaL_loadstring | 0x140fd9140 | 0x140fd9a70 | R13; called by the scheduler 0x140e77d00 / SM 0x140e78be0 (`call 0x140fd9a70`) |
| lua_pcall | 0x140f63050 | 0x140f63dd0 | R13 (H) |
| lua_gettop / lua_settop | 0x140f62c80 / 0x140f63980 | 0x140f63a00 / 0x140f64700 | R13; settop(L,-2) in 0x140e79200 |
| lua_getfield / lua_setfield | 0x140f62b80 / 0x140f63810 | 0x140f63900 / 0x140f64590 | R13; `getfield(L, GLOBALSINDEX, "worldGlobals")` in 0x140e79200 |
| lua_tolstring | 0x140f63b50 | 0x140f648d0 | R13; also the `__index` key read |
| lua_newthread / lua_xmove / lua_resume / lua_sethook | 0x140f62ee0 / 0x140f63d50 / 0x140fdb580 / 0x140f64a60 | not ported | call shape in 0x140e77e80 (M) |
| L → lsi | 0x140e81f60 | 0x140e82e70 | R13, port_sites |

## 3. Addresses

| item | Sam4 616154 | SamSM 628492 | method | conf. |
|---|---|---|---|---|
| **script_ctx** `void* ()` (TLS) | **0x1411223e0** | **0x141122ba0** | sig 32 B, unique in all 4 | D |
| **world_to_lsi** `lsi* (world*)` | **0x140b79820** | **0x140b79e70** | sig 16 B, unique in all 4 | D |
| pi_to_world `world* (pi*)` | 0x140b7cc30 | 0x140b7d070 | 2nd call of dev_script_run | D |
| **lua_exec_string** `int (lsi*, ctx*, const char*, uint)` | **0x140b748a0** | **0x140b74e10** | sig 64 B, unique in all 4 | D |
| lua_exec_string_cur `int (lsi*, const char*, uint)` (gets ctx itself) | 0x140b75130 | 0x140b756a0 | port_sites | D |
| lsi vt+0x98 exec chunk `(lsi, ctx, src, params*, flags)` | 0x140e807d0 | 0x140e816d0 | sig 48 B, unique in all 4 | D |
| scheduler exec / run | 0x140e77d00 / 0x140e77e80 | 0x140e78be0 / not ported | port_sites | D |
| lsi vtable | 0x141f345b8 | 0x141f18f60 | R13 | D |
| exec_file_lsi `int (lsi*, const char* path, uint flags)` | 0x140b74950 | 0x140b74ec0 | 2nd call of `scrExecuteCommandsFromFile` | D |
| dev_script_run `int (menu*, const char* path)` | 0x140859800 | 0x1408596c0 | sig 64 B, unique in all 4 | D |
| shell lsi global / getter | 0x1422ab068 / 0x140b75370 | 0x142288228 / 0x140b758e0 | port_sites (global read back) | D |
| shell_exec_file `(path, flags)` | 0x140b75170 | 0x140b756e0 | sig_at full | D |
| console key handler (pi vt+0x70) | 0x140053030 | 0x140053090 | sig 96 B, unique in all 4 | D |
| console block global (always 0) | 0x142273a34 | 0x142250b18 | port_sites | D |
| console IsActive / SetActive | 0x140aaee70 / 0x140ab0640 | 0x140aaf5e0 / 0x140ab0da0 | port_sites | D |
| RCon/console line processor | 0x140e63da0 | 0x140e64c30 | port_sites 24 B + sig_at 160 B | D |
| user-cfg runner | 0x140b73f10 | 0x140b744a0 | sig 64 B, unique in all 4 | D |
| `prj_bDisableConsolePause` | 0x1422736a0 | 0x142250740 | harvest | M |
| `cht_bEnableCheats` (INDEX) | 0x142276210 | 0x142253388 | harvest | M |
| `prjStartGenericWeaponAnimation` | 0x1405b05c0 | 0x1405b0490 | harvest (R17) | M |
| `CWorldInfoEntity::GetPlayer()` / `GetPlayersCount()` | 0x1403bcd90 / 0x1403bf2c0 | 0x1403bcaa0 / 0x1403bf010 | harvest; GetPlayer takes **no index**. It returns the game-info player and logs "should be called only for debugging purposes" | D/M |
| `CPlayerPuppetEntity::IsLocalOperator` | 0x140286060 | 0x140289d20 | harvest | M |

The Unrestricted builds were checked for the console only:
- Sam4_Unrestricted: key handler 0x140055980, gate 0x142273a34, exec string 0x140b76fd0, world_to_lsi 0x140b7c1d0.
- SamSM_Unrestricted: key handler 0x1400559d0, gate 0x14224fb18, exec string 0x140b77530, world_to_lsi 0x140b7c810.

## 4. Suggested `signatures.json` entries
All were verified read-only against the four exes with the same `sigscan.signature`/`find` logic and `nth_call` as
`tools/port_symbols.py`. Each `src` signature matches exactly once in every exe, and the `call_of` index resolves to
the addresses above in every exe.

```json
"script_ctx": {"src": "0x1411223e0", "len": 32, "_note": "per-thread engine context (TLS); ctx+0xa8 = executing CLuaScriptInterface (R13, R20 s2.3)"},
"lua_world_to_lsi": {"src": "0x140b79820", "len": 16, "_note": "world -> CLuaScriptInterface: *(*(world+0x130)+0x58) (R13, R20 s2.3)"},
"lua_exec_string": {"src": "0x140b748a0", "len": 64, "_note": "int (lsi, ctx, const char* src, uint flags): ctx+0xa8 bracket + lsi vt+0x98, chunk runs as a scheduler coroutine (R20 s2.2)"},
"lua_exec_file": {"call_of": "scrExecuteCommandsFromFile", "index": 1, "_note": "int (lsi, const char* path, uint flags); flags&8 opens rU (unsigned) (R20 s2.1)"},
"dev_script_run": {"src": "0x140859800", "len": 64, "_note": "engine dev-script runner: pi->world->lsi->lua_exec_file(path,0) (R20 s2.1)"},
"pi_to_world": {"call_of": "dev_script_run", "index": 1, "_note": "project instance -> world (R13, R20 s2.1)"},
"console_key": {"src": "0x140053030", "len": 96, "_note": "pi vt+0x70 key handler: F1 (0x51) / Tilde (0x61) toggle the console (R20 s1.1)"}
```

Optional entry: `"lua_exec_chunk": {"src": "0x140e807d0", "len": 48}` (lsi vt+0x98), also unique in all four.

## 5. Recommended implementation: `lab_lua=<file>` (lab only)

**Scope.**
- Lab builds only. The key is in `ss4smvr/options.txt`. When the key is absent the payload does nothing (as with
  `lab_giveall`).
- The file is private lab content, outside the game tree or under `Bin/x64/ss4smvr/`. Never ship or commit it.

**Option.** `se4::options` only has `int get(key, fallback)`. Add a string getter, e.g.
`const char* getStr(const char* key)`, so the key and path are read at attach. A relative path resolves against the
payload directory.

**Load.**
- At attach (no CRT under the loader lock, see LAB_NOTES), or lazily on the first tick, read the whole file with
  `CreateFileW`/`ReadFile` into a mod-owned buffer and append a NUL.
- Prefix `"--[[@lab_lua-@]] "`, as the engine does, so errors name the chunk.
- Do not use the engine VFS (this avoids the signature level question in §1.3).

**Run once, on the simulation thread, in the local player's world.**
- Use the existing local-VR-player tick, `player_vr.cpp` `hookedCa` (the ClientAction sender 0x14048b160 path where
  `vehicles::step` runs).
- For a desktop-player lab run without VR, any other per-tick local-player hook on the same thread will do.

```cpp
// per build table: T.scriptCtx, T.worldToLsi, T.luaExecString (signatures.json names above)
static char* g_labLua;            // file text, NUL-terminated, or nullptr
static int   g_labLuaDelay;       // ticks; e.g. options "lab_lua_delay" (default ~90) so WorldScripts have run
static bool  g_labLuaDone;

void labLuaTick(uint8_t* localPlayer) {
    if (!g_labLua || g_labLuaDone || !localPlayer) return;
    if (g_labLuaDelay > 0) { --g_labLuaDelay; return; }
    g_labLuaDone = true;                                       // exactly once, even on failure
    void* world = *reinterpret_cast<void**>(localPlayer + 0x30);
    void* lsi   = world ? T.worldToLsi(world) : nullptr;
    if (!lsi || *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(lsi) + 8) != 1) {
        VRLOG("lab_lua: no world script interface"); return;
    }
    void* ctx = T.scriptCtx();                                 // this thread's ctx
    int ok = T.luaExecString(lsi, ctx, g_labLua, 0);           // brackets ctx+0xa8 itself
    VRLOG("lab_lua: executed (%s)", ok ? "ok" : "failed - see 'Lua error>' in the game log");
}
// in hookedCa, before or after realCa:  if (vrd == g_localVrd) labLuaTick(player);
```

**Notes.**
- The chunk may `Wait(Delay(t))` / `Wait(CustomEvent(...))`. The engine scheduler resumes it on later world script
  steps, so a native delay is optional. A file can start with `Wait(Delay(2))` instead.
- Errors appear in the game log as `Lua error> …`. Compile errors come from 0x140e77d00; runtime errors are reported
  by the scheduler **[I: exact text]**. Use `print(...)` for lab markers (`check_log.py`).
- Single-player is the host, so host-only calls such as `prjStartGenericWeaponAnimation` and `prjGiveItemToPlayer`
  work.
- Before a run that may save, snapshot the saves with `saves.sh` (memory note): world globals and suspended
  coroutines are persisted.

**Example lab file (SS4).** This is mod-authored. For SM use
`Content/SeriousSamSM/Databases/Weapons/SirianInjector_SM.ep`.

```lua
local wi = worldGlobals.worldInfo
local player = wi:GetPlayer()            -- debug macro, logs one warning; single-player lab only
print("lab_lua: player " .. tostring(player))
local pwp = wi:LoadResource(Depfile("Content/SeriousSam4/Databases/Weapons/SirianInjector.ep"))
prjStartGenericWeaponAnimation(player, pwp, "Use", "GWASF_STOP_MOVING|GWASF_STOP_LOOKING|GWASF_GIVE_NEW_WEAPON")
```

The full crate flow (PP layers, VFX, Netricsa tab, skill point; R17 §1.1) can be reproduced the same way. Write the
lab file from the steps in R17 and keep any code copied from `lab/lua` in the private file only. Alternatively, in a
level with a skill-point crate, call
`worldGlobals.OnPlayerUsedSkillPointCrate_Net(player, <composite entity id>)`. That RPC wrapper exists once the
crate script has been imported (`CreateRPC` stores it in `worldGlobals` in single-player too).

**Console alternative for shell-level lab steps** (no code needed):
- send X11 `F1` to open, type the line, `Return`, then `F1` to close;
- or pass `+prj_strExecCfg <file>` at launch for a shell script at startup;
- use these for cvars and cheats (`cht_bEnableCheats=1`) only.

## 6. Unknowns and runtime checks
1. **Thread.** That the ClientAction-sender hook, the world lsi step and WorldScripts start all run on one
   (simulation/game-loop) thread is inferred (R13 runtime check 1). Log `GetCurrentThreadId()` in `labLuaTick` and in
   a temporary hook of `exec_file_env` 0x140b74b00 (SM 0x140b75070) when WorldScripts.lua runs.
2. Where the world lsi's vt+0x90 step is called from was not found statically (the lsi pointer is not loaded
   inline). The scheduler resumes waiting chunks there.
3. The exact meaning of exec flags 1, 4 and 8 inside the scheduler, and the exact text of runtime-error log lines.
4. Whether the final exes refuse unsigned loose `.lua` files opened with `"r"` (signature level 4). This is avoided
   by reading the file natively.
5. Whether `+exec <file>` is a command-line alias of `prj_strExecCfg` (record short name `exec`).
6. The window-system bit 29 (assumed to be Alt) and the transport for `"remotecall "` messages.
7. `UserCfg.lua`'s resolved directory under Proton (0x1410f0c50 path rules). The game logs "Executing user startup
   config from %1" or "…Creating default script", and it **creates the file if missing**. Do not point the lab at
   the user's real directories.
