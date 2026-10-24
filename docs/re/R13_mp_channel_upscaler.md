# Gate R13: multiplayer mod channel and upscaler insertion (Sam4.exe 616154, ported to SamSM.exe 628492)

Static analysis only (Ghidra copy `lab/ghidra_r13/sam4`, capstone, PE scans, the extracted Lua in `lab/lua`
and the shader sources embedded in the exe). No game, Wine, Proton or VR runtime was run. Sam4 addresses are
PE VAs. **SM** addresses come from `tools/port_sites.py` (new): a unique wildcarded signature (rel32 targets and
RIP displacements wildcarded, 24..192 bytes) of the function, or of the instruction that references a global, in
which case the global is read back from the decoded SM instruction. SM values marked "slot" were read from the
ported vtable; values marked "harvest" are in `docs/re/builds/SamSM.json`. Member offsets come from SE4
reflection records (`tools/meta_members.py`, record given) or the decompile at the cited address.
Unknown means unknown. "lsi" = `CLuaScriptInterface`, "ctx" = the per-thread engine context returned by
0x1411223e0, "netif" = the world's network interface (client or server object), "pi" = project instance.

New read-only tools: `tools/substrrefs.py` (strings containing a substring, e.g. log lines ending in `\n`, and
their code/data refs), `tools/port_sites.py` (port functions, call sites and referenced globals between builds).
Private evidence (never committed): `lab/r13/a*.c` decompiles, `lab/r13/port_*.txt` port logs,
`lab/r13/shaders/` (embedded shader text), `lab/lua/Content/Shared/Scripts/Network.lua` and
`lab/lua/Content/SeriousSam4/Scripts/WorldScripts.lua` (extracted with `tools/gro_extract.py`).

## 0. Summary

**Part 1 (mod channel).**
* The engine links stock **Lua 5.1.3** (`$Lua: Lua 5.1.3 Copyright ...` at 0x141f8f8f0). Every `lua_*`/`luaL_*`
  function a bridge needs was identified from the stock base library (`base_funcs` table 0x141f9b0a0) and ported
  to SM (section 1.2). Libraries opened: base, package, table, string, debug (no io/os, `lualibs` 0x141f9b5b0).
* World scripts run in a **`CLuaScriptInterface`** (vtable 0x141f345b8) whose `lua_State*` is at **lsi+0x38**.
  A world lsi has kind 1 (lsi+0x08). Reach it with **world→lsi 0x140b79820** (`*(*(world+0x130)+0x58)`), the
  world from **0x140b7cc30(pi)**, or the global list of all lsi objects at **0x1422ed268**. Script C functions
  check the *current* lsi in **ctx+0xa8**, so native code must set that around any call into Lua.
* Script RPCs are a generic net message, **`CNMScriptRPC`** (vtable 0x141f52d20): a 32-bit name hash, a
  reliable flag and a packed-argument byte array. `CallScriptRPC` (C function 0x140e76870) builds one and gives
  it to netif `vt+0xe0`. A client sends to the server; the server broadcasts to every connected client. The
  receiver resolves the hash to a string and calls `globals[name]` (= `worldGlobals[name]` in a world). **An
  unknown name or a missing function only logs one line: no kick, no crash** (0x140eafe30, 0x140e76160).
* Lua numbers travel as **float32**, strings as NUL-terminated bytes. A packet holds at most 1000 bytes
  (0x14224d4ec = 1000). `Network.lua`'s `CreateRPC` sends "unreliable" RPCs **reliably** (confirmed bug).
* A native RPC descriptor (the `SetPlayingInVR` path) is **not usable for a mixed lobby**: an unknown function
  index makes the server terminate the client and makes a client disconnect (0x140bbb8a0, 0x140bb6870).
* **Recommendation (section 3):** use `CNMScriptRPC` as a raw transport from native code with mod-private name
  hashes. Detour `CNMScriptRPC::Process` (0x140eafe30) to consume our hashes before the engine looks them up.
  Send with netif `vt+0xe0` (client→server) and the server's per-client send `vt+0xe8` (server→one acked
  client). Then no Lua, no savegame contamination and no traffic reaches unmodded peers after the handshake.

**Part 2 (upscaler).**
* Per eye (two-pass), the scene is rendered into a sub-rectangle of a shared **HDR canvas** (drawport global
  0x1422d7358, RGBA16F "HDR 3D canvas", created by 0x140c88a10; MSAA twin 0x1422d7320). Post-processing then
  writes the view's **3D canvas**, which is sub-sampled to `ren_fMax3DRenderingMPix` × `ren_fRenderingPixelRatio`
  (0x140b98fb0). Finally the canvas is **rescaled** into the eye render target (0x140bc0d20 → RescaleShader
  0x140bbf2c0 or `gfx_copy` 0x140b5a770).
* **Post-processing starts at 0x140c89010** (called by the render command 0x140ed32a0). It resolves MSAA, then
  0x140ba2ae0 calls the project's PP handler (global 0x1422d4850 → 0x140ad40c0 → PP object 0x142296270:
  setup 0x140affcf0, render 0x140aff360). Tonemapping is in the **final pass** (0x140afa930, `FinalPassPS`):
  exposure from a 1×1 luminance texture, then bloom, `ToneMap`, colour ramp/gamma and FXAA.
* `GENERATE_MVECS` is gated by a **read-only constant at 0x141e04078 (= 0)** AND `gfx_iAntiAliasingSamples == 1`.
  No cvar enables it. Motion vectors exist only in depth-prepass shaders, use *left-eye* previous matrices only
  (`c_mPrevModelToClipLeft` / `c_mPrevAbsToClipLeft`, no right-eye previous anywhere), and ignore skinning motion.
* **Device slot 0x070 (ours) is the scene projection every frame**: the player view 0x14048dad0 → 0x140478a80
  returns `device->vt[0x70](out, device+0x18 eye, ren_fNearClipPlaneDistance, ren_fFarClipPlaneDistance)`
  whenever device+0x18 ≠ 0. Jitter can be injected there per eye.
* **Recommendation (section 5):** Phase A, DLAA or FSR 3.1 native AA, applied in place on the HDR canvas in
  a detour of 0x140c89010, before PP and at render resolution. Phase B, upscaling: the same detour upscales into
  an engine-created output-size HDR target, calls the original PP with the eye target as destination, and
  suppresses the canvas rescale. Motion vectors are synthesized per eye from depth with our own unjittered
  matrices. The engine's MVECS stay off.

---

# Part 1: mod channel for multiplayer

## 1. The statically linked Lua 5.1.3

### 1.1 Identification
* Version string 0x141f8f8f0; `"Lua 5.1"` pushed as `_VERSION` in `luaopen_base` **0x140fdd250**.
* `base_funcs` (`luaL_Reg`) at **0x141f9b0a0**: assert 0x140fdc760, collectgarbage 0x140fdbe30, dofile 0x140fdc6f0,
  error 0x140fdc110, gcinfo 0x140fdc4b0, getfenv 0x140fdc290, getmetatable 0x140fdc190, loadfile 0x140fdc600,
  load 0x140fdc660, loadstring 0x140fdc580, next 0x140fdc520, pcall 0x140fdc930, print 0x140fdbf00, rawequal
  0x140fdc3b0, rawget 0x140fdc400, rawset 0x140fdc450, select 0x140fdc880, setfenv 0x140fdc2f0, setmetatable
  0x140fdc1f0, tonumber 0x140fdbfe0, tostring 0x140fdca00, type 0x140fdc4e0, unpack 0x140fdc7c0, xpcall
  0x140fdc990. `newproxy` was removed (no string). Besides `base_funcs`, `luaopen_base` adds only `_G`,
  `_VERSION`, `ipairs`, `pairs` and the `coroutine` table.
* `luaL_openlibs` **0x140fdd350** walks `lualibs` 0x141f9b5b0: "" (base), package 0x141019400, table 0x141015850,
  string 0x141016b00, debug 0x1410188a0.
* Pseudo-indices are stock: `LUA_REGISTRYINDEX` −10000 (`0xffffd8f0`), `LUA_GLOBALSINDEX` −10002
  (`0xffffd8ee`), as passed in 0x140e7f3e0 / 0x140fdd250. `lua_Number` is `double` (tonumber/pushnumber use it).

### 1.2 API addresses (identified from the luaB_* bodies, which are stock 5.1.3; all ported to SM)

Calling convention is Win64 `__fastcall` (C ABI of Lua 5.1). Confidence H = the call shape inside a stock luaB_*
function fixes the identity; M = identified from an engine call site only.

| function | Sam4 | SamSM | evidence |
|---|---|---|---|
| lua_newstate | 0x140fdbbe0 | 0x140fdc520 | allocates 0x268, called by lsi init 0x140e7f3e0 with allocator 0x140e81fd0 (H) |
| lua_close | 0x140fdbb60 | 0x140fdc4a0 | lsi teardown 0x140e81590 (M) |
| lua_gettop | 0x140f62c80 | 0x140f63a00 | luaB_pcall/print (H) |
| lua_settop (`lua_pop`) | 0x140f63980 | 0x140f64700 | luaB_rawget `settop(L,2)`, print `(L,-2)` (H) |
| lua_pushvalue | 0x140f633e0 | 0x140f64160 | luaopen_base `pushvalue(L,GLOBALSINDEX)` (H) |
| lua_insert | 0x140f62d20 | 0x140f63aa0 | luaB_pcall `insert(L,1)` (H) |
| lua_replace | 0x140f636a0 | 0x140f64420 (call site 0x140fdd326) | luaB_xpcall (H) |
| lua_checkstack | 0x140f62730 | 0x140f634b0 | "Lua stack overflow!" guard 0x140e7a7e0 (M) |
| lua_type / lua_typename | 0x140f63d00 / 0x140f63d30 | 0x140f64a80 / 0x140f64ab0 | luaB_type (H) |
| lua_isnumber | 0x140f62da0 | 0x140f63b20 | luaB_tonumber (H) |
| lua_isstring | 0x140f62dd0 | 0x140f63b50 | RegisterNetworkDescriptor arg check, lsi vt+0x78 (M) |
| lua_tonumber | 0x140f63bf0 | 0x140f64970 | luaB_tonumber (H) |
| lua_toboolean | 0x140f63ad0 | 0x140f64850 | luaB_assert (H) |
| lua_tolstring | 0x140f63b50 | 0x140f648d0 | luaB_print/tostring (H) |
| lua_touserdata | 0x140f63cc0 | 0x140f64a40 | lsi_from_L 0x140e81f60 (M) |
| lua_topointer | 0x140f63c30 | 0x140f649b0 | luaB_tostring `"%s: %p"` (H) |
| lua_pushnil / pushnumber / pushinteger | 0x140f632d0 / 0x140f632f0 / 0x140f63220 | 0x140f64050 / 0x140f64070 / 0x140f63fa0 | luaB_tonumber; pushinteger writes tag 3 + `(double)n` (H) |
| lua_pushlstring / pushstring / pushfstring | 0x140f63260 / 0x140f63310 / 0x140f631d0 | 0x140f63fe0 / 0x140f64090 / 0x140f63f50 | luaopen_base, luaB_tostring (H) |
| **lua_pushcclosure** | **0x140f63100** | **0x140f63e80** | luaopen_base `pushcclosure(L,f,0/1)` (H) |
| lua_pushboolean | 0x140f630e0 | 0x140f63e60 | luaB_pcall (H) |
| lua_pushlightuserdata | 0x140f63240 | 0x140f63fc0 | writes tag 2; lsi registers itself with it (H) |
| **lua_getfield** | **0x140f62b80** | **0x140f63900** | luaB_print `getfield(L,GLOBALSINDEX,"tostring")` (H) |
| lua_rawget / lua_rawset | 0x140f634b0 / 0x140f63530 | 0x140f64230 / 0x140f642b0 | luaB_rawget/rawset (H) |
| lua_createtable | 0x140f62840 | 0x140f635c0 | `createtable(L,0,0)` in 0x140e79200 (M) |
| lua_getmetatable / lua_setmetatable | 0x140f62bf0 / 0x140f63890 | 0x140f63970 / 0x140f64610 | 0x140e7f3e0 sets `_CT_LUAMT_GLOBALINDEX_` on globals if none (M) |
| **lua_setfield** (`lua_setglobal`) | **0x140f63810** | **0x140f64590** | luaopen_base `setfield(L,GLOBALSINDEX,"_VERSION")` (H) |
| lua_settable | 0x140f63950 | 0x140f646d0 | perms table fill 0x140e7a7e0 (M) |
| lua_call | 0x140f626e0 | 0x140f63460 | luaB_print `call(L,1,1)` (H) |
| **lua_pcall** | **0x140f63050** | **0x140f63dd0** | luaB_pcall `(L, top-1, -1, 0)` (r9d = 0 at 0x140fdc94b) (H) |
| lua_load | 0x140f62e90 | 0x140f63c10 | luaL_loadbuffer tail (H) |
| lua_error | 0x140f62960 | 0x140f636e0 | luaB_dofile (H) |
| lua_gc | 0x140f62970 | 0x140f636f0 | `(L,0,0)` / `(L,1,…)` around lsi init (M) |
| lua_next / lua_objlen | 0x140f62f90 / 0x140f62fd0 | 0x140f63d10 / 0x140f63d50 | table walks in 0x140f54de0 (M) |
| luaL_register | 0x140fd9480 | 0x140fd9db0 | `luaL_register(L,"_G",base_funcs)` (H) |
| luaL_newmetatable | 0x140fd9180 | 0x140fd9ab0 | body = stock (getfield registry, createtable, setfield) (H) |
| luaL_checklstring / luaL_optlstring | 0x140fd8590 / 0x140fd9280 | 0x140fd8ec0 / 0x140fd9bb0 | luaB_loadstring (H) |
| luaL_checkany / luaL_checktype | 0x140fd8500 / 0x140fd8780 | 0x140fd8e30 / 0x140fd90b0 | luaB_pcall / rawget (H) |
| luaL_error / luaL_argerror | 0x140fd8880 / 0x140fd8330 | 0x140fd91b0 / 0x140fd8c60 | luaB_assert `"%s"`, luaB_tonumber "base out of range" (H) |
| luaL_callmeta / luaL_optinteger | 0x140fd8440 / 0x140fd9210 | 0x140fd8d70 / 0x140fd9b40 | luaB_tostring `"__tostring"`, luaB_tonumber (H) |
| **luaL_loadbuffer** | **0x140fd8f30** | **0x140fd9860** | luaB_loadstring; body = `lua_load(L, getS 0x140fd81b0, &ls, name)` (H) |
| luaL_loadstring / luaL_loadfile | 0x140fd9140 / 0x140fd8f60 | 0x140fd9a70 / 0x140fd9890 | strlen + loadbuffer body; luaB_dofile (H) |
| luaL_openlibs / luaopen_base | 0x140fdd350 / 0x140fdd250 | 0x140fddc90 / 0x140fddb90 | lualibs walk (H) |

Not identified: `lua_rawgeti/rawseti`, `lua_newuserdata`, `lua_tointeger` (`lua_tonumber` covers it).

### 1.3 The world script VM: `CLuaScriptInterface`

| item | Sam4 | SamSM | evidence |
|---|---|---|---|
| vtable | 0x141f345b8 | 0x141f18f60 | stored by dtor 0x140e7eb80 (lea at 0x140e7eb8f) |
| lsi+0x08 kind (1 = world) | — | — | set by vt+0x20 Init(kind) 0x140e80ae0; 0x140e80320 refuses kind ≠ 1 ("does not belong to the world") |
| lsi+0x20 netif, +0x28 script net-descriptor registry | — | — | read by CallScriptRPC / RegisterNetworkDescriptor through ctx+0xa8; +0x20 is filled in 0x140e80d30 from `*(X+0x230)`, the same netif that `CEntity::NetIs*` reach through world+0x1c0 |
| **lsi+0x38 `lua_State*`** | — | — | `= lua_newstate(...)` in InitState **0x140e7f3e0** (SM 0x140e802d0) |
| lsi+0x40 scheduler (0x48 bytes, ctor 0x140e776e0; loads `Engine/ScriptLua/LuaScheduler.lua` in 0x140e78b00) | — | — | 0x140e80ae0 |
| lsi+0x60 call state (0x38 bytes, 0x140fdd7e0) | — | — | 0x140e7f3e0; BeginCall uses `*(lsi+0x60)+0x30` |
| registry key `_CT_LUAREG_LUASCRIPTINTERFACE_` = lightuserdata lsi | — | — | 0x140e7f3e0; **L→lsi: 0x140e81f60** (SM 0x140e82e70) |
| global list of all lsi (`{ptr,count,...}`, count at +8) | **0x1422ed268** | **0x1422ca3f8** | add 0x140e7ed90 from vt+0x20 / 0x140e80d30, remove in 0x140e81590 |
| world → lsi | **0x140b79820** `*(*(world+0x130)+0x58)` | 0x140b79e70 | used by CNMScriptRPC::Process |
| pi → world | 0x140b7cc30 (`pi+0x10`→`+8`, ref-counted) | 0x140b7d070 (call site in Render 0x14005400b) | Render 0x140053b70 |
| netif → world | 0x140bb3a10 `*(*(*(n+0xe80)+0x10)+8)` | 0x140bb40f0 | CNMScriptRPC::Process |
| world → netif | `*(*(world+0x1c0)+0x230)` | same offsets (SM 0x140ba63a0) | `CEntity::NetIsServer` 0x140ba5cc0 (entity+0x30 = world); netif vt+0x68 IsServer, vt+0x70 IsClient |

**Per-thread context and the "current lsi".** `0x1411223e0` returns ctx from TLS (index global **0x1423c3638**,
SM 0x1423a0318; slot +0x178). **ctx+0xa8 is the lsi currently executing.** `exec_file_env` **0x140b74b00** (SM
0x140b75070) saves it, sets it to its lsi, runs and restores it. The call frame `0x140b74400`/`0x140b744e0` does
the same. `CallScriptRPC` dereferences `*(ctx+0xa8)+0x20` **without a null check** (0x140e76870), and
`script_error` 0x140e81e60 only longjmps (`luaL_error`) when the erroring `L` belongs to ctx+0xa8, otherwise it
logs. So native code that enters Lua must bracket the call: `old = ctx->cur; ctx->cur = lsi; …; ctx->cur = old`.

**Globals.** In a world lsi, `globals` *is* `worldGlobals` (0x140e79200 when kind 1: `globals = worldGlobals`).
The real globals table has metatable `_CT_LUAMT_GLOBALINDEX_` with `__index` 0x140fe2c10, which resolves engine
script functions and cvars by name with permission checks, and `__newindex` 0x140fe2680, which refuses new
globals ("Script tried to write to a new global variable"). Native code must therefore use `lua_rawset`, not
`lua_setfield`, to add anything to `_G`. Content scripts run in a protected environment
(`_CT_LUAMT_PROTECTEDENVIRONMENT_`) built by 0x140e78120. It copies a fixed list of globals (worldGlobals,
globals, Event…, dofile, import, print, CallScriptRPC, RegisterNetworkDescriptor, pcall…), so a C function added
to `_G` later is invisible to content scripts. A chunk we load with `luaL_loadbuffer` gets the real `_G` as its
environment.

**Savegames (important for any Lua bridge).** The whole world Lua state is serialized with Pluto: the persist
root table holds `worldGlobals`, the scheduler tables, the object-reference table and the protected environment
(0x140e79200 → `_CT_LUAREG_PERSISTEDOBJECTSTABLE`; persist **lsi vt+0xb8 0x140e81290**, serialize vt+0x88
0x140e7fe30, unpersist **0x140e803c0**). Pluto refuses C functions that are not permanents:
`"Attempt to persist a C function"` (0x141f9c930, used at 0x140fe355e in 0x140fe3547). So a C closure stored in
`worldGlobals` breaks saving (0x140e81290 then logs "Pluto error while persisting" and drops the state), and a
Lua closure stored there ends up inside the user's save. After a load, the state is unpersisted rather than
WorldScripts being re-run (0x140e80320 → 0x140e803c0), and the engine calls the script function `OnLoadGame`
(0x140bc4f00, frame at 0x140bc51fe).

**Where world scripts start.** `CWorldInfoEntity` start/reset **0x1403d4020** (SM 0x1403d4050) runs
`<world>/Scripts/WorldScripts.lua` via `exec_file_env(lsi, path, {worldInfo, levelChanged})` at **0x1403d43ca**
(SM 0x1403d43fa), then calls `GetWeaponsForGiveAll` through the call frame. 0x140b74b00 reads the file and runs it
with lsi vt+0x98 **0x140e807d0** (SM 0x140e816d0) → scheduler 0x140e77d00 (`luaL_loadstring` 0x140fd9140, then a
scheduler coroutine 0x140e77e80). Scripts can therefore `Wait()`.

**Calling a Lua function by name from native code** (what the engine itself does):
`callframe_init(frame, ctx, lsi, "name", reportMissing)` 0x140b74400 → lsi vt+0xa0 BeginCall 0x140e7ee80
(SM 0x140e7fd80, slot). Pushers are at `frame[4]` (vt+0x48 Vector3f, +0x58 Quaternion, +0x60 QuatVect, +0x68
handle, +0x78 resource), `exists` 0x140b74cd0, `call` 0x140b74800 → lsi vt+0xa8 0x140e80b80 (`lua_pcall`,
"Error performing function call" on error), dtor 0x140b744e0. Name lookup: `globals[name]`, with dotted paths.

**Thread.** No thread is created for scripts: everything above runs on the caller's thread with that thread's
TLS ctx. CNMScriptRPC processing (net receive) and WorldScripts (world start) both use 0x1411223e0. That they run
on the game-loop thread (R10: main loop 0x14003a810 → game loop 0x140058800) is **inferred**, not traced. Runtime
check 1.

## 2. Script networking

### 2.1 Content side (`Network.lua`, SS4 and SM byte-identical, sha256 checked)
* `worldGlobals.CreateRPC("server"|"client", "reliable"|"unreliable", name, fn)`. On a single-player game it just
  stores `fn`. Otherwise it builds `rpcName = "RPC_" .. ("server_"|"client_") .. ("reliable_") .. name`,
  calls `RegisterScriptRPC(rpcName)`, stores `worldGlobals[rpcName] = fn` and, on the machine that *originates*
  the RPC ("server" RPC on the host, "client" RPC on a client), stores `worldGlobals[name]` as a wrapper that
  calls `CallScriptRPC(rpcName, reliable, ...)` and then `fn(...)` locally.
* **Bug (confirms PLAN item 7):** in the unreliable branch the script assigns the prefix string to the
  reliability variable instead of extending the prefix. The variable is then a non-nil string, so the wrapper
  takes the reliable branch: every "unreliable" content RPC is sent **reliable**, with the name
  `RPC_<side>_<name>`.
* Content uses it, e.g. `ItemCratePickup.lua` ("client","reliable"), `Achriman.lua` ("server","reliable").
* `RegisterScriptRPC` (script function 0x140e75f20, SM 0x140e76df0 harvest; record 0x141f31b88, parameter
  `IDENT idRPCName`) only reads its argument as an IDENT. That interns the string, so the receiver can resolve the
  hash (2.3).
* State sync for script entities is separate: `RegisterNetworkDescriptor` (C function 0x140f54de0, SM
  0x140f55b60) + `netPrepareClassForScriptNetSync` (0x140e37a50) + `CNetObject::AssignScriptNetworkDescriptor`
  (0x140bdb3f0). Member types: SLONG, CString, Handle, Ptr, IDENT, BOOL, FLOAT, Vector3f, ULONG, UBYTE,
  Quaternion4f, QuatVect. Flags: "server"/"client". At most 254 members. Optional `PostReceiveServerUpdate` /
  `PostReceiveClientUpdate`. It needs a class prepared identically on all peers (NetSyncClasses.lua), so it is
  not a channel for optional mod data.

### 2.2 `CallScriptRPC` (Lua C function **0x140e76870**, SM 0x140e77750; registered as a global in 0x140e7f3e0)
Arguments: `(string name, boolean reliable, ...)`. Errors go through `script_error` with
`"Error making script RPC call: %1"`: "running without a network interface!" (lsi+0x20 null), "first argument
must be a string!", "second argument must be a boolean (for reliable specification)!". Packed-argument encoding
(tag byte + payload, appended with 0x140e76090 / 0x140e75fb0):

| Lua value | tag | payload | receiver (0x140e76160) |
|---|---|---|---|
| nil | `N` | — | pushnil |
| boolean | `t` / `f` | — | pushboolean |
| number | `n` | **float32** (`(float)lua_tonumber`) | pushnumber(double) — integers above 2^24 lose precision |
| string | `s` | bytes **including the NUL** (`strlen+1`, 0x140e769a8) | pushstring; embedded NULs impossible |
| table that is a net object (entity) | `h` | 32-bit handle, remapped on the client side (0x140bb8c80 / 0x140bb8340) | handle pusher |
| userdata Vector3f / QuatVect / Quaternion4f | `v` / `q` / `Q` | 12 / 28 / 16 bytes | typed pushers |
| userdata resource | `p` | 32-bit name hash (0x140cdcdc0) | resource lookup |
| function, thread, light userdata, plain table | — | error "invalid argument %1 (type %2)" | — |

The message is then built on the stack (`CNMScriptRPC` ctor 0x140eafa90, name hash 0x140eafe10, flag at +0x18,
args at +0x20) and passed to **netif vt+0xe0**, then destroyed (0x140eafaf0).

### 2.3 The `CNMScriptRPC` message

| item | Sam4 | SamSM |
|---|---|---|
| class record ("CNMScriptRPC", size 0x30, base CNetworkMessage) | 0x141f52ae8 | — |
| vtable | 0x141f52d20 | 0x141f37770 (lea in ctor 0x140eb0972) |
| ctor / dtor / clone (vt+0x20) | 0x140eafa90 / 0x140eafaf0 / 0x140eafbf0 | 0x140eb0960 / 0x140eb09c0 / 0x140eb0ac0 |
| set name: `+0x1c = hash(name)` | 0x140eafe10 | 0x140eb0ce0 |
| vt+0x50 = vt+0x58 (process on server / on client) | 0x140eafdd0 → **Process 0x140eafe30(ctx, netif, msg)** | 0x140eb0ca0 → 0x140eb0d00 |
| vt+0x68 IsReliable = `return this->+0x18` | 0x1409437e0 (shared stub) | — |
| vt+0x28 = 1 ("game message", queued until in game; base returns 0) | 0x1400472e0 | — |
| append bytes to args `(arr, data, len)` | 0x140e75fb0 | 0x140e76e80 |

Members (meta, `tools/meta_class.py --class CNMScriptRPC`): `nm_bReliable` +0x18 BOOL, `nm_ulFunctionNameHash`
+0x1c ULONG, `nm_aubPackedArgs` +0x20 `CStaticStackArray<UBYTE>` (ptr +0x20, count +0x28, capacity +0x2c,
engine allocator 0x1410c5a20 / free 0x1410c55f0). The fields are serialized by reflection, so the arg array is
opaque bytes on the wire.

* **Name hash** 0x1410c2770 (SM 0x1410c2f10): `h = h*0x1003f + (signed char)c` over `strlen`, 32 bits.
* **Receive path:** `Process` 0x140eafe30 does `world = 0x140bb3a10(netif)`, then
  `name = hash2str 0x1410c6850(hash)`. The lookup is in the process-wide IDENT intern table 0x14231a760, filled
  by `ident_from_str` 0x1410c65f0. If that fails it logs **"Error processing script RPC: cannot resolve function
  name from hash %1"** and returns. Otherwise `lsi = world_to_lsi(world)` and lsi vt+0x60 (0x140e80d20 →
  **0x140e76160**). That opens a call frame for `globals[name]`. If the function is missing it logs
  **"Unable to call script RPC '%1': function doesn't exist!"** and returns *before* parsing the arguments.
  Malformed arguments log "invalid data type encoded" / "error reading arguments" and stop. A Lua error inside
  the handler is caught by `lua_pcall` (lsi vt+0xa8). vt+0x50 always returns 1.
* **Contrast:** the native object RPC messages (`CNMReliableRPC` 0x141f526f8 strings, server handlers
  0x140bbb8a0 / 0x140bbb9a0) log "Error executing reliable RPC: %1 / Terminating client %2" and kick the sender
  (0x140bbb7a0). The client handler 0x140bb6870 disconnects ("Disconnecting from server..."). The script RPC path
  has **no** such branch.

### 2.4 Routing
* **Client:** netif vt+0xe0 **0x140bb8d00** (client vtable 0x141e1a1d8; SM 0x140bb93d0, vtable 0x141dfea68)
  enqueues to the server connection (`client+0x1b18`, 0x140e58950). The server **does not forward** a client's
  script RPC. It only runs `worldGlobals[name]` locally; content re-broadcasts with a "server" RPC.
* **Server, broadcast:** vt+0xe0 **0x140bbc440** (server vtable 0x141e1af98; SM 0x140bbcb20, vtable
  0x141dff828). It sends to each of the **18** client slots (`server+0xe98 + i*0xb28`, slot state > 3) **only if
  `server+0xd794 ≠ 0`** (because CNMScriptRPC vt+0x28 = 1). `+0xd794` is set to 1 by server vt+0x20
  0x140bbbff0, which also pushes the world state to every client, and set to 1 then back to 0 around one send
  in vt+0x18 0x140bbc1c0. The ctor 0x140bbaaa0 initializes it (0x140bbabb4). Its exact lifecycle is a runtime
  check (2).
* **Server, one client:** vt+0xe8 **0x140bbc520** `(server, msg, int* slot)` (SM 0x140bbcc00, slot). It enqueues
  to `server+0xe98 + slot*0xb28` with **no** `+0xd794` gate (only a relevance test for world-object messages,
  vt+0x30 = 0 here).
* **Sender identity on the server:** `server+0xd790` holds the client slot being processed. The kick path uses
  it as the offender (0x140bbb957: `local = server+0xd790`, then 0x140bbb7a0 terminates that client). It is −1
  outside loops (0x140bbabaa, 0x140bbb767). Its validity inside `Process` is inferred (medium confidence, runtime
  check 3). The script RPC itself carries no sender.
* Messages are cloned on enqueue (`msg->vt[0x20]`, 0x140e58950), so a stack message may be sent and then
  destroyed.

### 2.5 Reliability, size and rate
* `nm_bReliable` is honoured: vt+0x68 returns it, and the packet writer **0x140e58c70** (SM 0x140e59a90) writes
  reliable messages first and moves them to the resend ring. Unreliable messages are written only into in-game
  packets and are not tracked. Script RPCs are not de-duplicated (vt+0x30 = 0 skips the merge in 0x140e58950).
  Unsent messages wait in the queue rather than being dropped (inferred from 0x140e58c70: a message that does not
  fit stops the loop).
* **Size:** max UDP packet = **1000** bytes (global 0x14224d4ec = 1000 in .data, check at 0x140fd78fe; SM
  0x14222c298 = 1000). A message is written whole into a packet or waits (`0x140f56440` fails when out of space).
  No fragmentation was found. Keep a script RPC well under ~900 bytes. Budget ≤ 128 bytes.
* **Rate:** `cli_iMaxOutBPS` 0x14224d4e0 (default 6000 in .data), `cli_iMaxBPS` 0x14224d4dc (20000),
  `ser_iMaxClientBPS` 0x14224d4e4 (20000); SM 0x14222c28c / 0x14222c288 / 0x14222c290. Units are presumably bytes
  per second (unknown). The user's config may override these values.

### 2.6 Native net-call descriptors (the `SetPlayingInVR` path) — not suitable
`SetPlayingInVR` 0x140bdc6f0 (SM 0x140bdce60) calls **net call 0x140bb3ad0**
`(netif, &err, frame, 0, *(descriptor 0x1422d58d0 + 0x90))`. The descriptor is the meta function record of a
class RPC, and +0x90 is its function index. The receiver 0x140bb37e0 looks the index up with 0x140bb50c0(class,
idx). An unknown index raises "RPC call attempted with invalid function (object type = '%1', function index =
%2)!" or "...on a non-RPC function". Through the server handlers above that **terminates the client**, and on a
client it **disconnects**. A mod-registered descriptor would only exist on modded peers, and adding records to a
class table risks shifting indices. A brand-new `CNetworkMessage` subclass would be an unknown message type on
unmodded peers. Its handling was not traced and is assumed fatal. Rejected for a mixed lobby.

## 3. Recommended implementation (Part 1): native `CNMScriptRPC` transport

**Why:** `CNMScriptRPC` is a message type every peer already accepts. It is built and consumed from native code,
the payload is opaque bytes, and an unmodded peer that receives it only logs one line (2.3). Lua is not involved,
so nothing reaches `worldGlobals` or savegames, and no ctx+0xa8 bracket is needed.

```cpp
// per-build table (Sam4 / SamSM): see section 6
struct NmScriptRpc { uint8_t raw[0x30]; };               // +0x18 reliable, +0x1c hash, +0x20 CStaticStackArray<UBYTE>
uint32_t se4Hash(const char* s) { uint32_t h = 0; for (; *s; ++s) h = h * 0x1003f + (int8_t)*s; return h; }
const uint32_t kHello = se4Hash("SSVR.Hello.1"), kAck = se4Hash("SSVR.Ack.1"), kPose = se4Hash("SSVR.Pose.1");

void sendRaw(void* netif, uint32_t hash, bool reliable, const void* data, int len, const int* slotOrNull) {
    NmScriptRpc m; T.nmCtor(&m);                           // 0x140eafa90 / SM 0x140eb0960
    *(int*)(m.raw + 0x18) = reliable; *(uint32_t*)(m.raw + 0x1c) = hash;
    T.appendBytes(m.raw + 0x20, data, len);                // 0x140e75fb0 / SM 0x140e76e80 (engine allocator)
    auto vt = *(void***)netif;
    if (slotOrNull) ((void(*)(void*, void*, const int*))vt[0xe8 / 8])(netif, &m, slotOrNull);  // server → one client
    else            ((void(*)(void*, void*))vt[0xe0 / 8])(netif, &m);                          // client → server
    T.nmDtor(&m);                                          // 0x140eafaf0 / SM 0x140eb09c0 (message was cloned)
}
// detour CNMScriptRPC::Process 0x140eafe30 / SM 0x140eb0d00:  void (void* ctx, void* netif, void* msg)
void hkProcess(void* ctx, void* netif, uint8_t* msg) {
    uint32_t h = *(uint32_t*)(msg + 0x1c);
    if (h == kHello || h == kAck || h == kPose) {           // ours: never reaches hash2str / Lua
        const uint8_t* p = *(uint8_t**)(msg + 0x20); int n = *(int*)(msg + 0x28);
        bool isServer = ((int(*)(void*))(*(void***)netif)[0x68 / 8])(netif);
        int slot = isServer ? *(int*)((uint8_t*)netif + 0xd790) : -1;   // sender slot (runtime check 3)
        modNet::onMessage(h, p, n, slot, *(int*)(msg + 0x18));
        return;
    }
    realProcess(ctx, netif, msg);
}
```
* **netif** = `*(*(world + 0x1c0) + 0x230)` (2.x table; world from `0x140b7cc30(*(pi global))`, pi = 0x142273478 /
  SM 0x142250518). Client vs server: netif vt+0x70 / vt+0x68. Call sends on the same thread as the engine's own
  net traffic, i.e. the simulation tick (runtime check 1).
* **Never intern our names** (do not call `RegisterScriptRPC`/`ident_from_str` on them). An unmodded peer then
  fails at hash→string and logs once. The detour matches hashes directly. Check at build time that the three
  hashes differ from `se4Hash` of every `RPC_*` name in content (cheap host test).
* **Handshake (a):**
  1. When the client's local player exists (in game), the client sends `Hello{magic "SSVR", proto, modVersion,
     build (616154|628492), exe fingerprint (text CRC from fingerprints.json), content hash,
     en_iPlayerIndex, flags (VR on, body IK, melee)}` **reliably** to the server. It retries every 2 s, at most
     3 times. `en_iPlayerIndex` is `CPlayerActorPuppetEntity` +0xf70 (meta rec 0x141861d48, flags 0x4000 =
     replicated; **SM +0xf78**, rec 0x141846e18). It is the cross-machine peer key, because net handles are
     remapped per machine.
  2. A modded server records `slot = server+0xd790 → {playerIndex, caps}` and replies `Ack{proto, accepted,
     reason, server build}` to **that slot only** (vt+0xe8). On a mismatch the reply is `accepted = 0` and VR data
     is disabled for that peer (no kick). The client shows a message.
  3. An unmodded server logs "cannot resolve function name" at most 3 times. The client gets no Ack and runs
     flat-compatible (no VR channel). A modded server never sends anything to a client that has not said Hello,
     so unmodded clients see nothing.
  4. The server sends `Peers{list of acked playerIndex + caps}` to each acked client on change, reliably.
* **VR data (b):** each VR client sends `Pose{playerIndex, PosePacket 64 B (src/core/wire.hpp)}`, **unreliable**,
  30 Hz, only after an accepted Ack. 70 bytes of payload. The server keeps the latest packet per player and
  relays it unreliably with vt+0xe8 to every *other* acked client. If bandwidth is short it drops to 15 Hz.
  Budget: at 30 Hz one VR client upstream ≈ 2.1 kB/s plus headers (against `cli_iMaxOutBPS` 6000).
  Downstream ≈ 2.1 kB/s × other VR players. The server applies the poses to its own copy of the remote player (vrd
  hands, R3 s4) and validates melee there (PLAN). Receivers fall back to the native puppet after 0.5 s
  (PLAN Phase 4).
* **Degradation:** no kick and no crash in any mix, because the transport never touches the native RPC tables
  and unknown hashes only log. A message for a player index that is not in game is ignored. The detour checks
  `n` against the expected size and ignores anything malformed. Both directions are gated on the Ack.
* PLAN MP-1 preference 1 (native `ClientActionVR` for the poses the engine itself needs) is unaffected. This
  channel carries the handshake and the extra 64-byte block, and is the fallback transport for poses.

**Alternative (PLAN item 7, Lua bridge), if Lua-side logic is wanted later.** Find the world lsi (world→lsi),
`L = lsi+0x38`. Bracket ctx+0xa8. Register natives with `lua_pushstring` + `lua_pushcclosure` + `lua_rawset(L,
LUA_GLOBALSINDEX)` (rawset because of `__newindex`). Run chunks with `luaL_loadbuffer` + `lua_pcall`, or
`lsi vt+0x98(lsi, ctx, source, params, 0)` to get engine semantics (coroutine, `Wait`). Call
`CallScriptRPC(name, false, ...)` directly to get real unreliable sends. **Because of Pluto**, hook lsi vt+0xb8
(0x140e81290 / SM 0x140e82190) to `rawset(nil)` every mod key in `worldGlobals` before the original runs and
restore them afterwards. Re-run the bootstrap after `OnLoadGame` / unpersist 0x140e803c0 (SM 0x140e812c0). Bring
up the bootstrap after `exec_file_env` returns for `WorldScripts.lua` (hook 0x140b74b00, SM 0x140b75070, path
suffix check). This route also means number args truncate to float32 and strings cannot hold NULs. It costs more
than the native transport and adds savegame risk, so it is not the recommendation for R13.

---

# Part 2: upscaler insertion (DLSS/FSR per eye)

## 4. Renderer facts

### 4.1 One eye, two-pass (R10 / EYE_TARGETS for the outer part)
1. VR view 0x1401dcbe0 → eye loop **0x1401dc520** (SM 0x1401dc0f0): `rt = device->vt[0x28](gfx, eye)` (eye RT,
   size = device slot 0x60 × `vr_fRenderingPixelRatio` via 0x140b6aca0). Then `device+0x18 = eye` and
   `Render(pi, rtDrawport, 1)` through 0x1401deaf0.
2. Player view **0x14048dad0** (SM 0x14048e0e0): projection **0x140478a80** (SM 0x1404790b0) → device slot
   0x70 for eye `device+0x18`. The eye transform is device slot 0x78. Then **RenderView 0x140bc1cd0** (SM
   0x140bc23c0). Other cameras do the same: 0x1400d7de6, 0x1400ea915, 0x1400ff7b1, 0x140455944, 0x14062d876,
   0x14071e5b7, 0x140997de8 (slot 0x70/0x78 with eye = `device+0x18`, `lab/device_calls_sam4.json`), each with
   its own RenderView call (0x1403db890, 0x14062f0f7, 0x14071f96e, 0x1407bf460 call 0x140bc1cd0).
3. RenderView: `0x140b98fb0(dpW, dpH, &w, &h)` (**MPix limiter**, SM 0x140b99660). If
   `ren_fMax3DRenderingMPix` (0x1422ab2f4, clamped 0..1000) > 0 and `MPix/(w·h/1e6)` < 0.95, it scales both sides
   by `sqrt` of the ratio. Then both sides are multiplied by `ren_fRenderingPixelRatio` (0x14224cdd4, clamped
   0.0625..16). If the result differs from the drawport, or `ren_bForceCAS` (0x1422efdc4), a **3D canvas** of
   that size is allocated (0x140e172f0, "resized 3D canvas", capped at `gfx_pixMaxRenderTargetSize` 0x1422ab3c0)
   and the original drawport is kept in 0x1422d5198. The scene matrices from the gfx object are copied into a
   render-view command (0x140edf970 / 0x140ee6850). With single-pass stereo (0x140b6b3a0) a second set at
   gfx[0x1d2..0x1f9] is copied too.
4. HDR targets: **0x140c88a10** (SM 0x140c88f90). HDR mode 0x14224e1cc is copied from 0x14224cdf8 (unnamed):
   0 = LDR, 1 = 64-bit (fmt 5, RGBA16F inferred), else 128-bit (fmt 6, falls back to 5). The target is
   sized from the larger of the current target (`gfx+0x1c78`) and **device slot 0x68**, then the MPix limiter,
   then `gfx_pixMaxRenderTargetSize`. It is re-created when that size or the AA setting changes. It creates
   **`HDR 3D canvas`** with `rt_create 0x140b5e370(gfx,w,h,0,fmt,0x26,4)` (SM 0x140b5e8e0) into drawport global
   **0x1422d7358** (SM 0x1422b4508). Flag 4 doubles the width when gfx vt+0x278(0) ≠ 0 (double-wide stereo; the
   meaning of vt+0x278 is unknown). An MSAA twin goes to **0x1422d7320** (SM 0x1422b44d0) when
   `gfx_iAntiAliasingSamples` > 1. Each view uses a sub-rectangle: `0x140c891f0(singleSample, w, h)` (SM
   0x140c89770) sets the box.
5. The view's render commands run inside **0x140bc0d20** (SM 0x140bc1410; `FUN_140bbef50` per command list). The
   PP render command **0x140ed32a0** (SM 0x140ed4370) calls **PP dispatch 0x140c89010(gfx, view+0x288 (the 3D
   canvas), flags=view+0x2e0, …)** (SM 0x140c89590), described in 4.4.
6. Back in 0x140bc0d20, the canvas is copied into the saved drawport. Same size → mode 5 (CAS); canvas/output
   > 2.0625 → mode 6 ("Wide-kernel" downscale). Otherwise `ren_iRescalingFilter` decides (0x14224d36c: 0 point
   copy, 1 filtered copy, 2/3 bicubic/sharp bicubic, 4 Lanczos, 5 CAS). CAS becomes Lanczos when downscaling
   and a filtered copy when upscaling by more than ~2.06× (ratio < 0.4848). **Rescale pass 0x140bbf2c0** (SM
   0x140bbf9a0; mode 5 → CAS 0x140ec8cb0) runs, or `gfx_copy` **0x140b5a770** (SM 0x140b5acd0) for the simple
   filters.
7. Back in 0x1401dc520: gfx vt+0x288(tex, 3) (TRANSFER_SRC), device slot 0x18 submit (EYE_TARGETS).

### 4.2 Render vs output resolution (summary)
* Output per eye = eye RT = slot 0x60 × `vr_fRenderingPixelRatio` (0x14224cb38, default 1.0 in .data).
* Render (shading) resolution per eye = eye RT → MPix limiter → × `ren_fRenderingPixelRatio`. The engine upsamples
  the canvas spatially (6.) after post-processing, so **everything from bloom to FXAA runs at render resolution**.
* `ren_fMax3DRenderingMPix` and `ren_fRenderingPixelRatio` are user settings. For the upscaler the mod sets
  `ren_fMax3DRenderingMPix = 0` at runtime and drives scale with `ren_fRenderingPixelRatio`. It does not write the
  user's ini.

### 4.3 Colour, depth and motion vectors during an eye render
* **Colour:** HDR canvas sub-rect (0x1422d7358 + box), pre-tonemap, RGBA16F. With MSAA it is first resolved from
  0x1422d7320 in 0x140c89010 (when `flags & 0x20 == 0`, via `gfx_copy`).
* **Depth:** the scene depth buffer bound with the HDR canvas is not referenced by a global found here
  (**unknown**; runtime check 4: capture the depth `VkImage` with the HDR canvas extent in `src/vk`). There is also
  the depth-prepass linear-depth colour target written by `DepthShadowShader` (non-shadow path:
  `vDepth.r = ENCODE_DEPTH(linear z)`). Its format comes from 0x140f4ada0 (SM 0x140f4bb20): 0x13/0x14 by
  `efx_iSSAOQuality`. It is only switched to a 4-channel format 5/6 when MVECS are on.
* **Motion vectors:** dormant (4.5).

### 4.4 Where post-processing (and tonemapping) starts
`0x140c89010` (PP dispatch): flags bit 0 means plain copy (no PP). Otherwise it calls **0x140ba2ae0** (SM
0x140ba31b0) `(gfx, &hdrCanvas, dst, flags, …)`:
* if `gfx_bAllowPostprocessing` (0x14224d200) and handler **0x1422d4850** (SM 0x1422b1a10; set by 0x140ba2c80 from
  project init 0x14004a5da) → `handler->vt[0x8]` = **0x140ad40c0** (pass vtable 0x141cbc410; SM 0x140ad4590):
  `pp = *0x142296270` (SM 0x142273430), `setup 0x140affcf0(pp, src, dst, …)` (src → pp+0x140, dst → pp+0x148,
  SM 0x140b00180). Then `pp->vt[0x88]` = **PP render 0x140aff360** (PP vtable 0x141cd8e90; SM 0x140aff7d0 in vtable
  0x141cbd700). That handles auto exposure (`efx_bAutoExposure` 0x14224e1c8), DOF (`efx_bDepthOfField`), motion
  blur (`efx_bMotionBlur`, frame blending), Gaussian blur, zoom blur and light streaks. It ends with the chain
  0x140afe1d0 (SM 0x140afe640), bloom 0x140afc210 (SM 0x140afc640) and the **final pass 0x140afa930** (SM
  0x140afad40).
* otherwise `gfx_copy(src → dst)`.

Final pass shader `PPFinalPassShaders.sh` / `FinalPassPS` (embedded): `col = clamp(HDR)`. It computes
`fExp = tex(t_sFinalLuma).r * c_fFixedExposure` (a 1×1 adapted-luminance texture), adds bloom, does
`col *= fExp`, then **`ToneMap(col)`** (`ren_bToneMapping`, alt modes `efx_iAltToneMapping`), then colour ramp,
gamma, levels and HDR OETF. FXAA (`gfx_iFXAA`, 0x140afa930 → 0x140b02230) runs after it. **So the last point at
render resolution and before any post FX is the entry of 0x140c89010 (or 0x140ba2ae0).**

### 4.5 `GENERATE_MVECS`
* C++ adds `"GENERATE_MVECS=1;"` (0x141693458) to the permutation only when **`[0x141e04078] != 0` (a .rdata
  constant, value 0; SM 0x141de88e8 = 0)** **and `gfx_iAntiAliasingSamples == 1`**. This happens in grass
  0x1402130f0 (check at 0x140213249), vegetation 0x140231a70 (0x140231c17) and the depth-shadow permutations
  0x14022c640 (15 checks). The same constant gates the C++ side that computes previous matrices:
  0x140210a17, 0x14022262a, 0x14022fd17, 0x140bfaaf0, 0x140fa515d (terrain crumbs). Those read previous-frame
  fields at `view+0xd30..+0xdfc` (doubles; semantics unverified). It also gates the depth-target format
  (0x140f4ada0/0x140f4ae4c → fmt 5/6), material setup 0x140ce0855 and 0x140db3ab1. **There is no cvar.** Enabling
  it means writing that read-only constant in memory before any pipeline is created, which is a code patch.
* Shader coverage: `DepthShadowShader.sh`, `GrassShader.vsh`, `VegetationShader.vsh` and `TerrainShader.sh` only
  (depth pass). The PS writes `vDepth = float4(depth, velocity.xy * c_vViewportSizeInPixels * 0.5, 1)`.
* **Left eye only:** constants are `c_mPrevModelToClipLeft` / `c_mPrevAbsToClipLeft`. No right-eye previous matrix
  exists in any embedded shader. `vOutPosPreviousClipRight` (vegetation) is declared and never used.
  `vClipSpacePos` is taken from `VS_OUT_POS` *before* `VS_OUT_EXPORT_VR` selects the right-eye X, so in
  single-pass the right eye gets left-eye MVs. In two-pass each pass's "Left" matrices are that eye's. Whether
  the stored previous matrix is per eye or shared by both passes is **unknown**: both eyes are rendered from the
  same viewer, so it is probably shared, which would make one eye's "previous" equal the other eye's current.
* **No deformation MVs:** skinned meshes use `SkinVertexPos` with the current bones and only the previous
  *model* matrix.
* Shader cache: whether `CachedShaders_VLK.gro` contains MVEC permutations is unknown (presumably not). Runtime
  compilation would be needed.

### 4.6 Projection and jitter
`0x140478a80`: `if (device && device+0x18 != 0) return device->vt[0x70](out, device+0x18, ren_fNearClipPlaneDistance
(0x14224d388), ren_fFarClipPlaneDistance (0x14224d38c))`. Otherwise it builds a perspective from the FOV. So in
VR **our `sProjection` (src/se4/vr_device.cpp) is the scene projection of every eye pass, every frame**, for the
player and for the other cameras listed in 4.1. Slot 0x70 is also called for eyes 1 and 2 by the union-frustum
helper 0x140b6a8f0 (SM 0x140b6aed0, culling for single prepare) and by VR-view internals (0x1401d9fda,
0x1401dad19, 0x1401db72a). A sub-pixel jitter is harmless there.

## 5. Recommended implementation (Part 2): FSR 3.1 / DLSS-DLAA per eye

Preconditions: two-pass stereo (`vr_iUsingSinglePassStereo` < 1, R10 note 5), `gfx_iAntiAliasingSamples = 1`,
`gfx_iFXAA = 0`, `efx_bMotionBlur = 0`, `vr_bAllowDepthOfField = 0` / `efx_bDepthOfField = 0`. These are runtime
cvar writes while the mod is active, restored on exit, never saved. The HUD/crosshair stay on composition layers
(GOAL), so they are outside the upscaler input.

**Jitter (both phases).** In `sProjection`, for eye 1/2 and only while the upscaler is active, offset the FOV
tangents before `core::engineProjection`:
`l += jx·(r−l)/Wr; r += jx·(r−l)/Wr; u += jy·(u−d)/Hr; d += jy·(u−d)/Hr`. Here (jx, jy) ∈ (−0.5, 0.5) comes from a
Halton(2,3) sequence with ≥ 8·(out/render)² phases, the same index for both eyes, and Wr×Hr is the eye's render
size (4.2). This is convention-free (it shifts the frustum by sub-pixels). The unjittered fov is kept for MVs.
Advance the index once per frame (for example in device slot 0x30, called every frame per R10), not per call.

**Motion vectors.** Do not enable the engine MVECS (4.5: .rdata patch, left-eye only, shared previous, no
skinning, shader recompile). Instead, in `src/vk`, run a compute pass per eye before the upscaler. It reads the
scene depth (runtime check 4) and reprojects with `prevVP_eye · inverse(curVP_eye)` built from **unjittered**
matrices. `VP_eye = proj(fov_eye, near, far) · view_eye`, where `view_eye` = engine camera placement ∘ our slot
0x78 eye transform. The camera placement is the vrd anchor (R3 `vrd+0x2c0`) ∘ head (slot 0x80); verify against
the engine (runtime check 5). This is correct for static geometry and camera motion and ghosts on fast enemies,
as the research doc expects. Moving-object MVs are a later step (the left-eye MVECS shaders could be patched,
out of scope for R13).

**Phase A — DLAA / FSR 3.1 native AA (no resolution change).** Detour **PP dispatch 0x140c89010** (SM
0x140c89590). When `device == ours`, `device+0x18 ∈ {1,2}`, the view render type is 0 (`*(*(ctx+0x10)+0x3b0) == 0`; RenderView
0x140bc1cd0 writes 0 there and 0x140aff360 tests it), and `flags & 1 == 0`, then before calling the original:
1. Take the colour as the HDR canvas `VkImage` and sub-rect (0x1422d7358 drawport box +0x10..+0x1c). Take depth
   from runtime check 4 and MVs from the pass above, jitter = this eye's.
2. Evaluate the eye's context (`NVSDK_NGX_Handle` / `ffxContext`, one per eye, `maxContexts` ≥ 2) into a mod
   image, then copy back into the canvas sub-rect. Use the current command buffer (`gfx+0x1ea8`, EYE_TARGETS).
3. Call the original 0x140c89010: tonemap, bloom etc. run unchanged on the anti-aliased HDR image.
Exposure: use the auto-exposure flag first. The engine's adapted luminance (`t_sFinalLuma`) is produced inside the
PP and would be one frame late. Reset history on level load, on a camera change (a different camera function from
4.1 calls slot 0x70), on menu ↔ game and on teleports.

**Phase B — upscaling (render < output).** Set `ren_fRenderingPixelRatio = 1/scale` (`ren_fMax3DRenderingMPix =
0`). In the same detour:
1. Upscale the canvas sub-rect (render res) into an **engine-created** output-size HDR target. Create it with
   `rt_create 0x140b5e370(gfx, outW, outH, 0, fmt 5, 0x26, 0)` once per size, on the render thread, and capture
   its `VkImage` in `src/vk` the way eye targets are captured.
2. Call the original PP with **src = that target and dst = the eye RT drawport** (saved drawport 0x1422d5198 for
   this view). The PP sizes its intermediates from dst (0x140aff360 reads `dst+0x10..0x1c`), so post FX run at
   output resolution.
3. Suppress the canvas rescale for this view: skip 0x140bbf2c0 / `gfx_copy` in 0x140bc0d20 by detouring
   0x140bbf2c0 and that single `gfx_copy` call site while a flag set in step 2 is active.
Fallback if step 2 proves fragile: upscale **after** PP (LDR) by replacing the rescale (0x140bbf2c0) with the
upscaler. Inputs are the post-tonemap canvas, depth and MVs, with the HDR flag off. The cost is temporal
upscaling of bloom/vignette.

**Other:** use a negative mip bias of `log2(render/output)` (vkCreateSampler hook, DLSS_FOVEATION A3). Keep per-eye
history images because the HDR canvas is shared by both eyes. Foveation VRS stays on the scene passes only.
DLSS on Linux/Proton uses the Windows NGX path (research A3).

## 6. Per-build address table (Sam4 616154 / SamSM 628492)

| name | Sam4 | SamSM | how SM was found |
|---|---|---|---|
| TLS ctx getter / TLS index global | 0x1411223e0 / 0x1423c3638 | 0x141122ba0 / 0x1423a0318 | sig / decoded |
| lsi vtable / list global | 0x141f345b8 / 0x1422ed268 | 0x141f18f60 / 0x1422ca3f8 | decoded from ported sites |
| lsi InitState / Init(kind) vt+0x20 | 0x140e7f3e0 / 0x140e80ae0 | 0x140e802d0 / 0x140e819e0 | sig, slot agrees |
| lsi persist vt+0xb8 / serialize vt+0x88 / unpersist | 0x140e81290 / 0x140e7fe30 / 0x140e803c0 | 0x140e82190 / 0x140e80d20 / 0x140e812c0 | sig, slots agree |
| lsi exec source vt+0x98 / BeginCall vt+0xa0 / Call vt+0xa8 / End vt+0xb0 | 0x140e807d0 / 0x140e7ee80 / 0x140e80b80 / 0x140e80750 | 0x140e816d0 / 0x140e7fd80 / 0x140e81a80 / 0x140e81650 | sig; BeginCall from slot |
| lsi RPC receive vt+0x60 → unpack/call | 0x140e80d20 → 0x140e76160 | 0x140e81c20 → 0x140e77030 | sig, slot agrees |
| L→lsi / script_error | 0x140e81f60 / 0x140e81e60 | 0x140e82e70 / 0x140e82d70 | sig |
| exec_file_env / WorldScripts call site / CWorldInfoEntity start | 0x140b74b00 / 0x1403d43ca / 0x1403d4020 | 0x140b75070 / 0x1403d43fa / 0x1403d4050 | sig |
| call frame init / call / dtor / exists | 0x140b74400 / 0x140b74800 / 0x140b744e0 / 0x140b74cd0 | 0x140b74990 / 0x140b74d70 / 0x140b74a70 / 0x140b75240 | sig |
| world→lsi / pi→world / netif→world | 0x140b79820 / 0x140b7cc30 / 0x140bb3a10 | 0x140b79e70 / 0x140b7d070 / 0x140bb40f0 | sig; pi→world from call site |
| CallScriptRPC (C) / RegisterScriptRPC / RegisterNetworkDescriptor (C) | 0x140e76870 / 0x140e75f20 / 0x140f54de0 | 0x140e77750 / 0x140e76df0 / 0x140f55b60 | sig (+harvest) |
| CNMScriptRPC vtable / ctor / dtor / clone | 0x141f52d20 / 0x140eafa90 / 0x140eafaf0 / 0x140eafbf0 | 0x141f37770 / 0x140eb0960 / 0x140eb09c0 / 0x140eb0ac0 | decoded / sig |
| CNMScriptRPC SetName / Process / vt+0x50 | 0x140eafe10 / **0x140eafe30** / 0x140eafdd0 | 0x140eb0ce0 / **0x140eb0d00** / 0x140eb0ca0 | sig, slots 0x50/0x58 agree |
| args append bytes / append tag | 0x140e75fb0 / 0x140e76090 | 0x140e76e80 / 0x140e76f60 | sig |
| name hash / hash→string / IDENT intern | 0x1410c2770 / 0x1410c6850 / 0x1410c65f0 | 0x1410c2f10 / 0x1410c7000 / 0x1410c6da0 | sig; hash2str via call site |
| client net vtable / vt+0xe0 send | 0x141e1a1d8 / 0x140bb8d00 | 0x141dfea68 / 0x140bb93d0 | slot |
| server net vtable / vt+0xe0 broadcast / vt+0xe8 send-one / vt+0x18 / vt+0x20 | 0x141e1af98 / 0x140bbc440 / 0x140bbc520 / 0x140bbc1c0 / 0x140bbbff0 | 0x141dff828 / 0x140bbcb20 / 0x140bbcc00 / 0x140bbc8a0 / 0x140bbc6d0 | slot (+sig) |
| server fields +0xd790 sender slot, +0xd794 send gate, slots +0xe98 × 0xb28 (18) | same | same (0x140bbc037, 0x140bbcb27, 0x140bbcc48) | decoded |
| conn enqueue / packet writer / max UDP global (=1000) | 0x140e58950 / 0x140e58c70 / 0x14224d4ec | 0x140e59770 / 0x140e59a90 / 0x14222c298 | sig / decoded |
| native net call / native RPC exec / srv kick handlers / client exec | 0x140bb3ad0 / 0x140bb37e0 / 0x140bbb8a0, 0x140bbb9a0 / 0x140bb6870 | 0x140bb41b0 / 0x140bb3ec0 / 0x140bbbf80, 0x140bbc080 / 0x140bb6f40 | sig |
| `en_iPlayerIndex` | +0xf70 (rec 0x141861d48) | **+0xf78** (rec 0x141846e18) | meta |
| MVECS .rdata constant | 0x141e04078 (= 0) | 0x141de88e8 (= 0) | decoded from 3 ported sites |
| grass / vegetation / depth-shadow perms / depth target fmt | 0x1402130f0 / 0x140231a70 / 0x14022c640 / 0x140f4ada0 | (site 0x140213089) / 0x1402319d0 / 0x14022c480 / 0x140f4bb20 | sig |
| player view / player projection / union frustum / eye loop | 0x14048dad0 / 0x140478a80 / 0x140b6a8f0 / 0x1401dc520 | 0x14048e0e0 / 0x1404790b0 / 0x140b6aed0 / 0x1401dc0f0 | sig |
| RenderView / execute view + rescale / rescale pass / MPix limiter | 0x140bc1cd0 / 0x140bc0d20 / 0x140bbf2c0 / 0x140b98fb0 | 0x140bc23c0 / 0x140bc1410 / 0x140bbf9a0 / 0x140b99660 | sig |
| HDR RT alloc / HDR canvas dp / MSAA dp / sub-rect / rt_create | 0x140c88a10 / 0x1422d7358 / 0x1422d7320 / 0x140c891f0 / 0x140b5e370 | 0x140c88f90 / 0x1422b4508 / 0x1422b44d0 / 0x140c89770 / 0x140b5e8e0 | sig / decoded / call site |
| PP render cmd / PP dispatch / PP apply / handler global | 0x140ed32a0 / **0x140c89010** / 0x140ba2ae0 / 0x1422d4850 | 0x140ed4370 / **0x140c89590** / 0x140ba31b0 / 0x1422b1a10 | sig / decoded |
| PP pass execute / PP object global / setup / render / final pass | 0x140ad40c0 / 0x142296270 / 0x140affcf0 / 0x140aff360 / 0x140afa930 | 0x140ad4590 / 0x142273430 / 0x140b00180 / 0x140aff7d0 / 0x140afad40 | sig / decoded / slot |
| PP chain / bloom / gfx_copy / CAS pass | 0x140afe1d0 / 0x140afc210 / 0x140b5a770 / 0x140ec8cb0 | 0x140afe640 / 0x140afc640 / 0x140b5acd0 / 0x140ec9cb0 | call sites / sig |
| cvars (harvest) | MPix 0x1422ab2f4, PixelRatio 0x14224cdd4, Rescaling 0x14224d36c, ForceCAS 0x1422efdc4, ToneMapping 0x14224e534, AllowPP 0x14224d200, AA samples 0x14224cdfc, MaxRT 0x1422ab3c0, vr PixelRatio 0x14224cb38, FXAA 0x14224bee8, AutoExposure 0x14224e1c8, MotionBlur 0x14224beac, DOF 0x14224bea8, vr AllowDOF 0x14229624c, near/far 0x14224d388/0x14224d38c | 0x1422884b4, 0x14222bb84, 0x14222c110, 0x1422ccf54, 0x14222d2e4, 0x14222bfb0, 0x14222bbac, 0x142288580, 0x14222b8e8, 0x14222ada8, 0x14222cf78, 0x14222ad6c, 0x14222ad68, 0x14227340c, 0x14222c12c/0x14222c130 | harvest |

The Lua API rows are in 1.2. Unrestricted builds were not ported (`tools/port_sites.py` does it the same way).

## 7. Unknowns and runtime checks (in order)
1. Thread identity of world scripts / CNMScriptRPC processing vs the game loop. Log `GetCurrentThreadId()` in the
   Process detour, in the WorldScripts hook and in the present hook.
2. Lifecycle of `server+0xd794` (server vt+0x20 vs vt+0x18 order around level changes). It only matters for
   broadcasts. The recommended design uses vt+0xe8, which has no gate.
3. `server+0xd790` = sender slot inside `CNMScriptRPC::Process` on the server (log it with a two-client lab).
4. The scene depth `VkImage` that belongs to the HDR canvas (capture by extent/usage in `src/vk`). Also whether
   render commands execute synchronously inside 0x140bc0d20, so that `device+0x18` is valid in the PP detour.
5. Engine camera world placement per eye equals vrd anchor ∘ head ∘ slot 0x78 (compare reprojected depth with the
   next frame). Meaning of gfx matrices gfx[0x18e..0x199] / gfx+0xc2c.. copied in RenderView (candidate source
   for view-proj).
6. HDR mode global 0x14224cdf8 name/default; meaning of gfx vt+0x278 (double-wide HDR canvas).
7. Whether the MVEC permutations exist in `CachedShaders_VLK.gro`, and whether the previous-matrix fields
   (`view+0xd30..`) are per eye (only if engine MVECS are ever revisited).
8. Bandwidth cvar units and the user's actual values; queue latency of unreliable script RPCs under load.

## 8. Tools added (read-only)
* `tools/substrrefs.py <exe> <symbols.json> [--max N] <substring>...` — strings containing a substring, with code
  and data references.
* `tools/port_sites.py <src.exe> <dst.exe> <va>[=name][@disp]...` — ports functions, call sites and referenced
  globals. It prints the decoded target instruction and referenced VA, and flags mismatches. `@disp` accepts the
  displacement positions printed by `cvar_readers.py`/`riprefs.py`.
