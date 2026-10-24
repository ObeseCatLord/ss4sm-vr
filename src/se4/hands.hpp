#pragma once
// Hands separate from the guns (docs/re/R15_hands.md). Today: the native two-hand grip driven by the off hand's
// squeeze, as in eduke32_vr. The weapon step (R15 s4.2) grips a two-handed weapon while the other hand's fire bit
// is held and its controller is within the snap distance of the weapon's grip attachment; the mod substitutes that
// bit, for the step only, with its grip intent:
//   - engage on a squeeze press edge while the off hand is within reach of LeftHandGripEffector,
//   - hold while the squeeze stays down (hysteresis), release when it is let go.
// While the off hand is in reach or gripping, its squeeze is kept out of the pad mapping (no gadget throw).
// Option grip_squeeze (default 1).
namespace hands {
bool install();
// Present thread: true when this XR hand (0 left, 1 right) is gripping a weapon or within reach of a grip.
bool squeezeTaken(int hand);
}  // namespace hands
