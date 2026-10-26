#pragma once
// Readable names of engine objects for logs and content checks (no engine calls; memory is probed first).
#include <cstdint>
namespace se4::names {
// A readable NUL-terminated ASCII string at p (3+ printable characters), else null.
const char* ascii(const void* p);
// The resource file of a weapon's params (en_pwpWeaponParams -> rs_prf +0x10 -> rf_strAssetFN +0x18, R17 s2), e.g.
// "Content/SeriousSam4/Databases/Weapons/SirianInjector.ep"; the string is read at the field or one pointer away.
const char* weaponFile(uint8_t* weapon);
}  // namespace se4::names
