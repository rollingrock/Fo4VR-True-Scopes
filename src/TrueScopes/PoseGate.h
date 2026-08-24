#pragma once

// v0.2.116 — POSE-BASED ACTIVATION.
//
// The vanilla eye-gate (TS_Player_UpdateScopeEyeGate, 0x140ef7b00 — decompiled
// 2026-08-24) activates the scope only when the eye↔scope distance is under
// fWeaponDistEnterScope:VRInput = 38 units (~54 cm), which makes a pistol scope
// at arm's length (~45-55 units) impossible to activate. Its verdict is fed to
// the enable switch FUN_140efaa60 through exactly ONE per-frame call site
// (0x140ef851f, game thread, from Main::OnIdle); Hooks.cpp thunks that call and
// routes the verdict through OnGateVerdict() below.
//
// Our test replaces vanilla's three checks (HMD-to-weapon look cone 25°/35°,
// weapon-forward cone 7°/15° widened by (60/dist)², distance 38/40) with a
// geometrically better-conditioned trio measured against the ACTUAL tube:
//   * distance     eye → ocular point (ScopeIdent's measured ocular face when
//                  probed, else the ScopeParent widget origin), in units
//   * lateral      the eye's perpendicular distance from the tube AXIS line, in
//                  units — this is vanilla's cone test made distance-invariant:
//                  a fixed lateral band is a wide angle up close and a narrow
//                  angle at arm's length, which is exactly the optics
//   * look cone    angle between the HMD's forward axis and the direction to
//                  the ocular — the head must actually be oriented at the scope
// The eye position is the NEARER of the two real eyes (camera root ± half IPD
// along the head X axis — the v0.2.114 aiming-eye selection), and the eye must
// sit BEHIND the ocular (positive axial component along the tube).
//
// Every threshold has an enter/exit pair (spatial hysteresis — this is what
// retires the scopeOffHoldMs time-hold hack for the pose path), and everything
// is a TOML knob, live-tunable through DevBench.
//
// Vanilla's own machinery is otherwise untouched: menu/holster force-off paths
// keep their meaning, ScopeMenu lifecycle stays sighted-state driven (it opens
// independently of eye proximity — reticle and Steady work at arm's length),
// and the Steady hold-breath bit (player+0x12a1 bit 8) is bypassed only in the
// sense that our verdict replaces the whole expression it short-circuited.
// iScopeEnabled=2 is NOT used anywhere (the forceAlwaysOn crash was root-caused
// 2026-08-24: the ScopeMenu ctor dereferences a failed input-layer allocation).

#include <cstdint>

namespace TrueScopes::PoseGate
{
	// Game thread, once per eligible frame, from the verdict call-site thunk.
	// Returns the verdict to feed vanilla's enable switch (the WIDGET verdict).
	// With poseGateEnabled=false (or pose sources missing) it is a transparent
	// passthrough of the vanilla verdict.
	bool OnGateVerdict(std::uintptr_t a_player, bool a_vanillaVerdict);

	// Render thread (fill hook): true when the lens fill should run. While the
	// widget is active but this is false, the lens is FROZEN — RT 0x62 persists,
	// so freeze = don't fill. Always true when the pose gate is not in charge.
	bool FillLive();

	// Diagnostics for DevBench /state.
	struct Diag
	{
		bool          enabled;   // poseGateEnabled setting
		bool          owned;     // enabled AND pose sources valid (we decide)
		bool          fillLive;
		float         dist;      // last eye→ocular distance (units)
		float         lateral;   // last eye distance from the tube axis (units)
		float         lookDeg;   // last HMD-forward → ocular angle (degrees)
		std::uint32_t evals;     // verdict evaluations (advances only while eligible)
	};
	Diag GetDiag();
}
