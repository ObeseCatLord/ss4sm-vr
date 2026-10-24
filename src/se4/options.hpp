#pragma once
// Lab/runtime switches from ss4smvr/options.txt next to the payload ("key=value" per line, '#' comments),
// read once at attach. Unknown keys are ignored; a missing file means every default.
namespace se4::options {
void load(const wchar_t* payloadDirFile);  // any path inside the payload directory (e.g. the log path)
int get(const char* key, int fallback);
}  // namespace se4::options
