# Gate R4 — content mounting (partial)

- First launch from Steam ran `Sam4_Unrestricted.exe`; the log shows unsigned workshop archives mounting:
  `$S:\steamapps\workshop\content\257420\<id>\zz<id>.gro: N files, signature: NONE`.
- Engine scans `Content/SeriousSam4/Scripts/CustomWorldScripts/` per world ("Found 0 script(s)") —
  a candidate Lua entry point for mod scripts.
- To do: add-only loose/`.gro` test in the normal exe; MP content checks.
