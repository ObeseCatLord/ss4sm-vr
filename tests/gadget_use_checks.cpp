// In-hand gadget activation (core/gadget_use.hpp).
#undef NDEBUG
#include "core/gadget_use.hpp"
#include <cassert>
#include <cstdio>

using namespace core;
using namespace core::gadget;

int main() {
    // Classification: name keys first, then the bot usage type.
    assert(classify("MedKitGadgetItem", 1) == Use::Inject);
    assert(classify("SeriousRageGadgetItem", 1) == Use::Drink);
    assert(classify("ExtraLifeGadgetItem", 1) == Use::Chest);
    assert(classify("CompanionDroneGadgetItemName", 1) == Use::Toss);
    assert(classify("BlackHoleGadgetItem", 2) == Use::Throw);
    assert(classify(nullptr, 4) == Use::Throw);
    assert(classify("SeriousTimeGadgetItem", 4) == Use::Toss);
    assert(classify("SomethingNew", 1) == Use::Trigger);
    assert(classify(nullptr, 0) == Use::Trigger);

    const Pose head{{}, {0, 1.7f, 0}};  // looking down -Z
    Frame f;
    f.headValid = f.handValid = f.otherValid = true;
    f.head = head;

    // Drink: the hand starts at the mouth (not armed), leaves, comes back and stays for the dwell.
    {
        Activation a;
        a.begin(Use::Drink);
        const Vec3 m = mouth(head);
        f.hand = {{}, m + Vec3{0, 0, 0.06f}};  // tip = grip - 0.06 z = the mouth
        assert(!a.update(f, 10) && !a.update(f, 500));
        f.hand.p = {0.3f, 1.2f, -0.3f};
        assert(!a.update(f, 600));
        f.hand.p = m + Vec3{0, 0, 0.06f};
        assert(!a.update(f, 700));
        assert(!a.update(f, 750));
        assert(a.update(f, 805));
        assert(a.fired() && !a.update(f, 900));
    }
    // Passing through the mouth zone faster than the dwell does not fire.
    {
        Activation a;
        a.begin(Use::Drink);
        f.hand.p = {0.3f, 1.2f, -0.3f};
        a.update(f, 0);
        f.hand.p = mouth(head) + Vec3{0, 0, 0.06f};
        assert(!a.update(f, 20) && !a.update(f, 80));
        f.hand.p = {0.3f, 1.2f, -0.3f};
        assert(!a.update(f, 130));
    }
    // Inject: the needle tip on the other forearm (other grip at the origin of its frame; forearm along +Z).
    {
        Activation a;
        a.begin(Use::Inject);
        f.other = {{}, {-0.2f, 1.1f, -0.3f}};
        f.hand.p = {0.3f, 1.1f, -0.3f};
        assert(!a.update(f, 0));
        f.hand.p = f.other.p + Vec3{0.03f, 0, 0.15f + 0.06f};  // tip 3 cm off the forearm, mid-way to the elbow
        assert(forearmDistance(f.other, tip(f.hand)) < 0.031f);
        assert(!a.update(f, 10));
        assert(a.update(f, 120));
        // The other hand itself (not its forearm) is too far ahead.
        Activation b;
        b.begin(Use::Inject);
        f.hand.p = {0.3f, 1.1f, -0.3f};
        b.update(f, 0);
        f.hand.p = f.other.p + Vec3{0, 0, -0.15f + 0.06f};
        assert(!b.update(f, 10) && !b.update(f, 200));
        // Without the other hand tracked nothing fires.
        Activation c;
        c.begin(Use::Inject);
        f.otherValid = false;
        assert(!c.update(f, 0) && !c.update(f, 500));
        f.otherValid = true;
    }
    // Inject into the body: the needle pointing into the belly fires; a hand resting at the hip with the needle
    // pointing forward does not.
    {
        f.otherValid = false;
        Activation a;
        a.begin(Use::Inject);
        f.hand = {{}, {0.4f, 1.2f, -0.4f}};
        a.update(f, 0);
        assert(a.armed());
        const Quat back{0, 1, 0, 0};  // grip -Z towards +Z (into the body)
        f.hand = {back, {0, 1.2f, -0.16f}};
        assert(!a.update(f, 10));
        assert(a.update(f, 115));
        Activation b;
        b.begin(Use::Inject);
        f.hand = {{}, {0.4f, 1.2f, -0.4f}};
        b.update(f, 0);
        f.hand = {{}, {0.15f, 1.0f, 0.05f}};  // tip 16 cm from the axis, pointing forward
        bool pin = true;
        assert(bodyDistance(head, f.hand, pin) < kBodyRadius && !pin);
        assert(!b.update(f, 10) && !b.update(f, 300));
        // Turned into the hip it goes off.
        f.hand.q = Quat{0, 0.70710678f, 0, 0.70710678f};  // -Z turned to -X (towards the axis)
        b.update(f, 310);
        assert(b.update(f, 420));
        // Lost tracking does not arm.
        Activation c;
        c.begin(Use::Inject);
        f.hand = {back, {0, 1.2f, -0.16f}};
        f.handValid = false;
        c.update(f, 0);
        f.handValid = true;
        assert(!c.update(f, 10) && !c.update(f, 300) && !c.armed());
        f.otherValid = true;
    }
    // Chest.
    {
        Activation a;
        a.begin(Use::Chest);
        f.hand.p = {0.4f, 1.0f, -0.4f};
        a.update(f, 0);
        f.hand.p = chest(head) + Vec3{0, 0, 0.06f};
        a.update(f, 10);
        assert(a.update(f, 115));
    }
    // Trigger: a trigger held at bring-up must be released first.
    {
        Activation a;
        a.begin(Use::Trigger);
        f.trigger = 1;
        assert(!a.update(f, 0));
        f.trigger = 0;
        assert(!a.update(f, 10));
        f.trigger = 0.9f;
        assert(a.update(f, 20));
    }
    // Throw: a forward hand motion of 3 m/s goes off; a slow one does not.
    {
        Activation a;
        a.begin(Use::Throw);
        for (int i = 0; i <= 10; ++i) {
            f.hand.p = {0.2f, 1.4f, -0.002f * i};  // 0.2 m/s
            assert(!a.update(f, uint64_t(i) * 10));
        }
        bool fired = false;
        for (int i = 0; i <= 10 && !fired; ++i) {
            f.hand.p = {0.2f, 1.4f, -0.02f - 0.03f * i};  // 3 m/s
            fired = a.update(f, 110 + uint64_t(i) * 10);
        }
        assert(fired);
        // Backwards (wind-up) motion is not a throw.
        Activation b;
        b.begin(Use::Toss);
        bool backFired = false;
        for (int i = 0; i <= 10; ++i) {
            f.hand.p = {0.2f, 1.4f, 0.03f * i};
            backFired |= b.update(f, uint64_t(i) * 10);
        }
        assert(!backFired);
    }
    std::puts("gadget_use_checks ok");
    return 0;
}
