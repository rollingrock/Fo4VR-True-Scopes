#pragma once

namespace TrueScopes::ScopeRender
{
	// Resolve addresses (RIP-decoded from verified code anchors) and allocate the
	// persistent BSShaderAccumulator. Returns false if any anchor fails verification —
	// the caller should then stay on the copy fill.
	bool Init();

	// Perform one mono world render from PrimaryWeaponScopeCamera into a temp render
	// target and copy it into the lens target. SEH-guarded: on any fault it logs,
	// permanently disables itself (Available() goes false), and returns false so the
	// caller can fall back to the copy fill. Must be called from the render-thread fill
	// hook (renderer locked).
	bool Render();

	// True once Init succeeded and no fault has tripped the guard.
	bool Available();

	// True only while our own deferred-resolve call is on the stack. The two
	// resolve accum-bind call-site hooks (Hooks.cpp) key off this to force
	// bind mode 3 (no clear) so our pre-drawn sun pass survives into the composite.
	bool InOwnResolve();
	// v0.2.78: invoked by ResolveAccumBind0Hook to run the sun pass INSIDE the resolve,
	// after the G-buffer geometry exists for it to shade. No-op unless a render deferred one.
	void RunPendingSunExec() noexcept;
	// v0.2.133: the deferred-decal (bullet-hole) stage - fired once per own
	// resolve from ResolveAccumBind0Hook, before the accum bind.
	void RunPendingDecalStage() noexcept;

	// Hooks::Install reports whether the two resolve bind sites were hooked; the sun
	// pre-draw is skipped when they are not (it would just be cleared again).
	void SetSunBindHooksInstalled(bool a_installed);


	// v0.2.116 — POSE FREEZE: one-shot dim of the frozen lens picture (fill hook,
	// live->frozen edge). Builds the LensComposite inputs itself; SEH-guarded.
	// Render-thread only.
	void DimFrozenLens(float a_factor);

	// v0.2.119 — plugin-owned widget presence support: may presence show the
	// widget (fit applied, or fit disabled by choice)?
	bool WidgetPresentable();

	// Run ident probe + widget fit outside a live fill (render thread; SEH'd).
	void PresenceFit();
	// v0.2.124: request a camera-smoothing state reset (scope-in edge, weapon
	// swap) — adopted on the next live fill.
	void CamSmoothReset() noexcept;
	// v0.2.125: does the frozen lens still need its presence-time prime fill?
	bool LensPrimeNeeded() noexcept;
	void LensPrimeDone() noexcept;

	// (v0.2.68 had a ResetWidgetFit() called on scope-in. REMOVED in v0.2.69: the engine
	// rewrites ScopeParent at EQUIP, not scope-in, so that reset re-captured our own
	// output as the new baseline and compounded the offset every scope cycle. The fit now
	// detects an engine rewrite by comparing the node against the values it last wrote,
	// which needs no external event and cannot be hooked to the wrong one.)


	// Thread id that currently holds the renderer+4 scoped bracket (0 = none).
	// The +4-reader hook answers "scoped" only to this thread.
	std::uint32_t OwnRenderThread();

	// Clear the fault latch so the next Render() attempts the own render again.
	// Called on scope-in (after the TOML reload) when retryAfterFault is set —
	// lets the user bisect a faulting config live instead of restarting the game.
	bool RetryAfterFault();  // true = a latch was actually cleared

	// --- DevBench surface (v0.2.65) -----------------------------------------
	// Everything below exists so the query server can answer questions that
	// previously required grepping the heartbeat log (emitted only every 300
	// renders) or attaching a debugger.


	// Live snapshot of the render diagnostics. These are the same values the
	// heartbeat prints, plus the engine pointers we resolve at Init() — having
	// those readable means an ad-hoc /read can target the sun pass config, the
	// render context or the accumulator without re-deriving anything.
	struct Diagnostics
	{
		// resolved once at Init (0 = unresolved)
		std::uintptr_t ssnArray, accum, gfxState, ctxPtrA, ctxPtrB, sunConfig;
		// captured each render
		std::uintptr_t rtm, renderer, camera;
		// lifecycle
		std::uint64_t renders;
		std::int32_t  lastStep;
		bool          available, faulted, sunBindHooks;
		// counters (cumulative)
		std::uint64_t nan61, nan62, sunPreNaN, sunPostNaN, camDataBad, invProjRejects, fogNulls, dumpFiles;
		// last-render state
		std::uint32_t passTotal;
		std::int32_t  lightsShadowed, lightsQueued, eyeCount;
		std::int32_t  sunPass, sunIsSSN, skyRoots, skyDrawn;
		// v0.2.76: sunPass means "we called the exec"; sunDrew is its RETURN VALUE,
		// i.e. whether FUN_142891040's technique gate let the draw happen at all.
		std::int32_t  sunDrew;
		std::uint64_t sunDrewCount, sunGatedCount;
		std::uint32_t sunCfgFlags;
		float         camRect[6];
		std::int32_t  viewport[6];
		std::int32_t  lightsClamp;  // v0.2.73: perfLightsMax applied last render (-1 = none)
	};
	Diagnostics GetDiagnostics();

	// v0.2.90 derived FOV. `derived` is computed every render whether or not it is
	// used, so it can be compared against a hand-tuned scopeFovDegrees before
	// anyone trusts it. `used` is what SetCameraFOV actually got.
	//   theta_render = 2*atan( (R / d) / M )
	//   R = lens disc world radius, d = eye-to-lens distance, M = zoomData fovMult
	struct FovInfo
	{
		float used;
		float derived;
		float discRadius;
		float eyeDistance;
	};
	FovInfo GetFovInfo();

	// --- v0.2.92 automatic widget placement ----------------------------------
	// The candidate offset that would put the lens disc on the optic's ocular
	// face, derived from the scope's world bounding sphere and the eye position.
	// ALWAYS computed, applied only when `widgetAutoPlace` is on — so it can be
	// checked against a hand-tuned value that is known to be right, before it is
	// trusted on a scope nobody has ever looked through.
	struct PlacementReport
	{
		bool  valid;
		bool  applied;       // widgetAutoPlace is on AND the candidate is valid
		float offset[3];     // candidate local offset from the engine baseline
		float target[3];     // world point it aims at
		float baseWorld[3];  // where the untouched baseline puts the disc
		float boundCenter[3];
		float boundRadius;
		float miss;  // |target - baseWorld|: how far off the baseline is today
		char  reason[72];
		char  method[16];  // "census" (exact) or "bound" (heuristic fallback)
		bool  haveBoth;
		float agreement;  // |census - heuristic|, the heuristic's measured error
		// Closed-loop state (v0.2.94). The loop steers by observing where the disc
		// actually lands, so it converges even if the assumed parent transform is
		// wrong — `parentResidual` says whether it was.
		bool  converged;
		bool  diverged;
		int   steps;
		float residual;        // |target - actual disc position| right now
		float bestResidual;
		float discWorld[3];    // where the disc actually is, same frame as target
		float parentResidual;  // 0 = assumed parent transform checks out
	};
	PlacementReport GetPlacement();

	// Recompute the latched placement on the next render (a probe found a
	// different scope, or a setting feeding it changed).
	void InvalidatePlacement();

	// --- v0.2.73 stage stopwatch ---------------------------------------------
	// Mean milliseconds per stage of our own render since the last reset, measured
	// on BOTH timelines: GPU via D3D11 timestamp queries, CPU via QPC across the
	// same marks. Both are needed — AccumulateScene is CPU pass-list building with
	// near-zero GPU work and the resolve is the reverse, and which one dominates
	// decides whether the fix is fewer objects or fewer pixels.
	//
	// Reset the window with DevBench `/perf/reset` (or any `perfTimers` write, which
	// calls the same thing). ⚠️ v0.2.73 detected the reset as an off->on EDGE seen by
	// the render thread, so two back-to-back /config/set calls landed between renders
	// and the reset silently never fired -- reporting session-long means that damped a
	// 9 ms effect to 1 ms. v0.2.74 uses a latch the render thread cannot miss.
	inline constexpr std::size_t kStageCount = 7;
	inline constexpr const char* kStageNames[kStageCount] = {
		"setup",    // camera + FOV + widget fit + G-buffer binds and clears
		"lights",   // ShadowSceneNode::ProcessQueuedLights — the per-light fit
		"accum",    // BSShaderUtil::AccumulateScene — culling + pass-list build
		"sun",      // accum clears/binds + the BSDFLightDir exec (off by default)
		"resolve",  // the deferred resolve: G-buffer draw + light volumes + composite
		"sky",      // sky root accumulation + immediate group draw
		"deliver"   // FinishAccum + the tonemapped 0x61 -> 0x62 lens copy
	};
	struct StageTimes
	{
		double        gpuMs[kStageCount];
		double        cpuMs[kStageCount];
		double        gpuTotalMs;
		double        cpuTotalMs;
		std::uint64_t gpuSamples;  // renders averaged in (GPU)
		std::uint64_t cpuSamples;  // ... and CPU; differs when the query ring is full
		std::uint64_t disjoint;    // renders discarded because the GPU clock changed
		bool          enabled;
		bool          available;   // false = CreateQuery failed; CPU numbers only
	};
	StageTimes GetStageTimes();

	// Clear the timing accumulators before the next render. Safe from any thread.
	void ResetStageTimers();
}
