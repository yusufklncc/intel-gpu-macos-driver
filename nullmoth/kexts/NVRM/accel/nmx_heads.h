/*
 * NMXAccel (kexts/NVRM/accel, PolyForm Noncommercial 1.0.0 with the rest of this directory): what the accelerator does
 * about each IntelXeFB head, as a pure function of what it knows, so the same rule drives the late-head arm and the
 * diagnostic sysctl debug.nmxaccel_heads, and can be checked off the machine (tests/nmx_heads_test.cpp).
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Design: docs/design/DESIGN-multihead-accel.md.
 */
#pragma once

enum NmxHeadVerdict {
    kNmxHeadAbsent = 0,        // nothing has published at this index
    kNmxHeadPiped,             // handed to the display machine, and a display pipe drives this very framebuffer
    kNmxHeadRegisteredNoPipe,  // handed to the display machine, but no pipe of its own was found (see the log)
    kNmxHeadBootArm,           // the boot-time arm is still pending: it registers every published head with head 0
    kNmxHeadLateArm,           // published after the arm, multi-head knob on: registered now (through IOGraphics' gate)
    kNmxHeadNotReady,          // as kNmxHeadLateArm, but its scan-out does not describe itself yet: retried
    kNmxHeadManual,            // published after the arm, knob off: only `sysctl debug.nvaccelfb=3` registers it (as before)
    kNmxHeadNotArmed,          // the accelerator is not armed (held, failed, -intelxe_noaccel): nothing registers any head
    kNmxHeadBlocked,           // a lower index has not published: the registration walk stops before this one
    kNmxHeadTerminated,        // its framebuffer went away (IntelXeFB stopped); the slot is free for a new one
    kNmxHeadVerdictCount
};

struct NmxHeadIn {
    bool published;    // slotted from the IntelXeFramebuffer publish notification
    bool registered;   // found_framebuffer was called for it
    bool piped;        // a display pipe drives it (pipe+0x98 read back)
    bool scanoutOk;    // nvScanoutInfo answered with a CPU mapping and a size
    bool terminated;   // its framebuffer terminated after it was slotted
};
struct NmxAccelIn {
    bool knob;               // nmxheads=1 / NVRAM nmx-heads=1 / debug.nmxaccel_heads_auto
    bool armed;              // debug.nvaccelfb reads 1 for real (gNvAccelArmed)
    bool armPending;         // the boot-time arm is waiting or running
    unsigned publishedMask;  // bit i = head i published (gNvHeadsPublished)
};

static inline int nmxHeadVerdict(unsigned idx, const NmxHeadIn &h, const NmxAccelIn &a)
{
    if (h.terminated && !h.published) return kNmxHeadTerminated;
    if (!h.published) return kNmxHeadAbsent;
    if (h.registered) return h.piped ? kNmxHeadPiped : kNmxHeadRegisteredNoPipe;
    // registerPendingLocked walks 0, 1, 2, ... and stops at the first index that has not published
    for (unsigned j = 0; j < idx && j < 32; j++)
        if (!((a.publishedMask >> j) & 1u)) return kNmxHeadBlocked;
    if (a.armPending) return kNmxHeadBootArm;
    if (!a.armed) return kNmxHeadNotArmed;
    if (idx == 0) return kNmxHeadManual;   // head 0 is only ever the boot arm's (or the sysctl's)
    if (!a.knob) return kNmxHeadManual;
    return h.scanoutOk ? kNmxHeadLateArm : kNmxHeadNotReady;
}

static inline const char *nmxHeadVerdictName(int v)
{
    switch (v) {
    case kNmxHeadAbsent:           return "absent";
    case kNmxHeadPiped:            return "armed (own display pipe)";
    case kNmxHeadRegisteredNoPipe: return "registered, NO pipe of its own";
    case kNmxHeadBootArm:          return "would be armed by the pending boot arm";
    case kNmxHeadLateArm:          return "would be armed now (multi-head knob)";
    case kNmxHeadNotReady:         return "would be armed once its scan-out describes itself";
    case kNmxHeadManual:           return "waits for `sysctl debug.nvaccelfb=3` (multi-head knob off)";
    case kNmxHeadNotArmed:         return "not armed: the accelerator itself is not armed";
    case kNmxHeadBlocked:          return "blocked: a lower head has not published";
    case kNmxHeadTerminated:       return "terminated (slot free)";
    default:                       return "?";
    }
}

// Per-head capabilities, read from the framebuffer's properties when it publishes. Head 0 (IntelXeFramebuffer) has all of
// them; the second pipe's framebuffer (IntelXeFramebufferPipe) has none: no flips (no nmxScanout), no pipe gamma, no
// hardware cursor, no adaptive sync. With the knob on, a head without a capability is never asked for it.
enum {
    kNmxCapFlip   = 1u << 0,   // IntelXeFBScanoutVersion: nmxScanout map/flip/home
    kNmxCapGamma  = 1u << 1,   // IntelXeFBPipeGammaSize: nmxPipeGamma
    kNmxCapTiming = 1u << 2,   // NVRMTiming: NMXAGDC can answer IOPresentment's link and plane-scaler queries
    kNmxCapPipeFB = 1u << 3,   // an IntelXeFramebufferPipe (the second pipe's framebuffer)
};
// true when the knob is on and head (not 0) lacks cap: the caller skips the call it would make
static inline bool nmxHeadLacks(bool knob, unsigned head, unsigned caps, unsigned cap)
{
    return knob && head != 0 && !(caps & cap);
}
