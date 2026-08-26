#pragma once

// Pose-based scope activation, replacing the vanilla eye-gate verdict.
//
// The vanilla gate (TS_Player_UpdateScopeEyeGate, 0x140ef7b00) activates only
// under an eye-scope distance of fWeaponDistEnterScope:VRInput = 38 units, so a
// pistol scope at arm's length (~45-55 units) can never activate. Its verdict
// reaches the enable switch FUN_140efaa60 through exactly one per-frame call
// site (0x140ef851f, game thread); Hooks.cpp thunks that call and routes the
// verdict through OnGateVerdict() below.
//
// The replacement test, measured against the actual tube: eye to ocular
// distance (ocular = the ScopeParent widget origin, engine-owned; see the note
// in Compute()), the eye's perpendicular distance from the tube axis (the cone
// test made distance-invariant), and the angle between the HMD forward axis and
// the direction to the ocular. The eye is the nearer of the two real eyes
// (camera root +/- half IPD along the head X axis) and must sit behind the
// ocular. Every threshold has an enter/exit pair for hysteresis and is a TOML
// knob, live-tunable through DevBench.
//
// Vanilla's menu/holster force-off paths keep their meaning and ScopeMenu stays
// sighted-state driven (reticle and Steady work at arm's length). iScopeEnabled=2
// is not used anywhere: the ScopeMenu ctor dereferences a failed input-layer
// allocation and crashes.

#include <cstdint>

namespace TrueScopes::PoseGate
{
	// Game thread, once per eligible frame, from the verdict call-site thunk.
	// Returns the verdict to feed vanilla's enable switch. With
	// poseGateEnabled=false (or pose sources missing) it is a transparent
	// passthrough of the vanilla verdict.
	bool OnGateVerdict(std::uintptr_t a_player, bool a_vanillaVerdict);

	// Render thread (fill hook): true when the lens fill should run. While the
	// widget is active but this is false, the lens is frozen - RT 0x62 persists,
	// so freeze = don't fill. Always true when the pose gate is not in charge.
	bool FillLive();

	// True when the verdict site has not evaluated within the last a_maxFrames
	// game frames while the pose gate had taken ownership - i.e. the weapon is
	// holstered or a blocking menu is open (vanilla eligibility stops the site).
	// The fill hook uses this to force the widget off edge that vanilla's own
	// per-frame force-off would have produced (it reads the real renderer+3,
	// which this design keeps at 0). Without it a mid-unequip re-arm latches
	// scope-active forever and the next draw fires no on edge, silently skipping
	// the TOML reload and the scope re-ident.
	bool VerdictStale(std::uint64_t a_maxFrames);

	// True when the verdict site has not run for a_maxFrames game frames at all
	// (weapon holstered / blocking menu), independent of pose-gate ownership -
	// the hide signal for plugin-owned widget presence.
	bool SiteStale(std::uint64_t a_maxFrames);

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
