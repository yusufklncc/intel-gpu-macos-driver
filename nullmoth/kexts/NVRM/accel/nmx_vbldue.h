/*
 * NMXAccel (kexts/NVRM/accel, PolyForm Noncommercial 1.0.0 with the rest of this directory): when a frame the present
 * worker is about to show was due on the panel, as a pure function of what it knows, so it can be checked off the
 * machine (tests/nmx_vbldue_test.cpp).
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Design: docs/design/DESIGN-display-boost.md. The present worker waits for the frame's GPU work (nvSurfWaitReady); a
 * frame whose work is still running at the start of vblank it was due at missed that vblank, and the worker tells IntelXe
 * then (i915's intel_display_rps_boost_after_vblank: a wait on the crtc's vblank queue that boosts when the flip's fence
 * has not signalled by the vblank). The frame is due at the first start of vblank after t0, the time the worker took it
 * (its present, or the landing of the frame it waited behind).
 */
#pragma once
#include <stdint.h>

// The frame length from the framebuffer's NVRMFlipWaitUs (5/4 of a frame, in us; IntelXeFB publishFlipWait). 0 =
// unknown: no property (nvAccelFlipWaitUs answers 50000 then) or a value that is no refresh rate (24 Hz and up).
static inline uint64_t nmxFrameNsFromFlipWait(uint64_t us)
{
    if (!us || us >= 50000) return 0;
    return us * 1000 * 4 / 5;
}

// The first start of vblank after t0Ns. lastVblNs: a start of vblank the framebuffer saw (0 = no phase known); frameNs:
// the frame length (0 = unknown). Without a phase: one frame after t0 (any interval of a frame holds one start of vblank,
// so a frame not done by then certainly missed one). 0 = unknown (no frame length).
static inline uint64_t nmxVblDueNs(uint64_t t0Ns, uint64_t lastVblNs, uint64_t frameNs)
{
    if (!frameNs) return 0;
    if (!lastVblNs) return t0Ns + frameNs;
    if (lastVblNs > t0Ns) {   // vblanks at lastVbl - j * frame: the earliest that is still after t0
        const uint64_t j = (lastVblNs - t0Ns - 1) / frameNs;
        return lastVblNs - j * frameNs;
    }
    return lastVblNs + ((t0Ns - lastVblNs) / frameNs + 1) * frameNs;
}

// The phase from IntelXeFB's op 4 query (NMXScanout version 4): agoNs since the pipe's last start of vblank at queryNs.
// 0 = no phase (the framebuffer answered ~0: its timestamps do not look live, or it is older).
static inline uint64_t nmxLastVblNs(uint64_t queryNs, uint64_t agoNs, uint64_t frameNs)
{
    if (agoNs == ~0ull || !frameNs || agoNs > 2 * frameNs || agoNs > queryNs) return 0;
    return queryNs - agoNs;
}
