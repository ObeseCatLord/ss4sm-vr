#pragma once
// VR full-body pose (arms on the tracked hands) for every player with CPlayerVRData, on every machine, through
// an engine motion handler on the player model (docs/re/R9_body_ik.md). Option body_ik (default 0).
namespace body {
bool install();
}
