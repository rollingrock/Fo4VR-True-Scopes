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

	// Hooks::Install reports whether the two resolve bind sites were hooked; the sun
	// pre-draw is skipped when they are not (it would just be cleared again).
	void SetSunBindHooksInstalled(bool a_installed);

	// Diagnostic: paint the lens RT a solid color (burst forensics — color-codes
	// frames the fill hook skipped). Render-thread only.
	void TintLens(float a_r, float a_g, float a_b);

	// (v0.2.68 had a ResetWidgetFit() called on scope-in. REMOVED in v0.2.69: the engine
	// rewrites ScopeParent at EQUIP, not scope-in, so that reset re-captured our own
	// output as the new baseline and compounded the offset every scope cycle. The fit now
	// detects an engine rewrite by comparing the node against the values it last wrote,
	// which needs no external event and cannot be hooked to the wrong one.)

	// Diagnostic (v0.2.52): 1-pixel readback of the lens RT 0x62 at fill-hook ENTRY,
	// i.e. what it held after every other writer in the PREVIOUS frame finished. The
	// in-render readback only ever saw our own write; this is the one that can catch
	// a later third-party writer. Edge-logged ("PREFILL 0x62 -> DARK/lit"), gated on
	// diagLensReadback. Render-thread only.
	void SamplePreFillLens();

	// Thread id that currently holds the renderer+4 scoped bracket (0 = none).
	// The +4-reader hook answers "scoped" only to this thread.
	std::uint32_t OwnRenderThread();

	// Clear the fault latch so the next Render() attempts the own render again.
	// Called on scope-in (after the TOML reload) when retryAfterFault is set —
	// lets the user bisect a faulting config live instead of restarting the game.
	void RetryAfterFault();

	// --- DevBench surface (v0.2.65) -----------------------------------------
	// Everything below exists so the query server can answer questions that
	// previously required grepping the heartbeat log (emitted only every 300
	// renders) or attaching a debugger.

	// Request a full-surface dump on the NEXT render, regardless of the
	// diagDumpLensEveryNRenders cadence. Removes the "set every=N, guess a
	// window, hope it crosses a multiple" dance — which silently produced zero
	// files during the 2026-08-08 session because the render rate (~9/s) was far
	// below the assumed one.
	void RequestDump();

	// Withdraw a pending RequestDump(). A request that outlives its caller's
	// timeout would otherwise fire on some later render in a different scene.
	void CancelDumpRequest();

	// Count of completed dump EVENTS. Poll after RequestDump() to know the dump
	// actually happened rather than assuming it did.
	std::uint64_t DumpEventCount();

	// Index (frame counter) used to name the files of the most recent dump event,
	// i.e. the NNNNNN in <NNNNNN>_62_lens.bmp. 0 = nothing dumped yet.
	std::uint64_t LastDumpIndex();

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
		std::uint32_t sunCfgFlags;
		float         camRect[6];
		std::int32_t  viewport[6];
		std::int32_t  lightsClamp;  // v0.2.73: perfLightsMax applied last render (-1 = none)
	};
	Diagnostics GetDiagnostics();

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
