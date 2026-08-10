#pragma once

namespace Settings
{
	template <class T>
	class Setting
	{
	public:
		using value_type = T;

		Setting(
			std::string_view a_group,
			std::string_view a_key,
			value_type a_default) noexcept :
			_group(a_group),
			_key(a_key),
			_value(a_default)
		{}

		[[nodiscard]] auto group() const noexcept -> std::string_view { return this->_group; }
		[[nodiscard]] auto key() const noexcept -> std::string_view { return this->_key; }

		template <class Self>
		[[nodiscard]] auto&& get(this Self&& a_self) noexcept
		{
			return std::forward<Self>(a_self)._value;
		}

		template <class Self>
		[[nodiscard]] auto&& operator*(this Self&& a_self) noexcept
		{
			return std::forward<Self>(a_self).get();
		}

	private:
		std::string_view _group;
		std::string_view _key;
		value_type _value;
	};

	using bSetting = Setting<bool>;
	using iSetting = Setting<std::int64_t>;
	using fSetting = Setting<double>;
	using sSetting = Setting<std::string>;

#define MAKE_SETTING(a_type, a_group, a_key, a_default) \
	inline auto a_key = a_type(a_group##sv, #a_key##sv, a_default)

	// Master switch for the per-frame lens fill.
	MAKE_SETTING(bSetting, "TrueScopesVR", fillEnabled, true);
	// Fill cadence: 1 = every frame, 2 = every other frame, ... RT 0x62 persists
	// between frames, so low cadence just lowers the lens refresh rate.
	// DEFAULT 2 (v0.2.40): at cadence 1 the own render intermittently produces
	// black lens frames in content-heavy scenes (user-bisected 2026-08-08:
	// lensMode 1 never stutters, cadence 2 never stutters, cadence 1 does) —
	// per-frame engine resource contention; root cause hunt deferred.
	MAKE_SETTING(iSetting, "TrueScopesVR", fillEveryNFrames, std::int64_t(2));
	// Eye-gate OFF hysteresis in ms (v0.2.50): the vanilla gate flickers off in
	// 200-900ms windows while aiming, and every off-edge plays the widget's
	// fade-to-black over the lens (the "black bursts"). Off-edges are only
	// honored after the gate stayed off this long. 0 = vanilla behavior.
	MAKE_SETTING(iSetting, "TrueScopesVR", scopeOffHoldMs, std::int64_t(1500));
	// Write 2 (always-on) into the iScopeEnabled:VR value cell after game load.
	// WARNING (2026-08-08): currently CRASHES on scope-in (ScopeMenu/input-layer
	// null-deref, crash-2026-08-08-17-07-51) — needs rework before use.
	// Optional: with the edge-triggered state hooks, the vanilla default (1, eye-gated)
	// works correctly — this just keeps the widget/fill on at all times.
	MAKE_SETTING(bSetting, "TrueScopesVR", forceAlwaysOn, false);
	// What fills the lens while scoped: 1 = copy of the main frame (phase-1 placeholder),
	// 2 = real mono world render from the scope camera (falls back to 1 on init failure
	// or a render fault).
	MAKE_SETTING(iSetting, "TrueScopesVR", lensMode, std::int64_t(2));
	// Scope render field of view in degrees.
	//
	// 0 = DERIVE IT (v0.2.90, recommended). The render FOV is fixed by the scope's
	// real magnification M and the lens geometry:
	//        theta_render = 2*atan( (R / d) / M )
	// with R the lens disc's world radius, d the eye-to-lens distance, and M the
	// weapon's own zoomData fovMult (6.0 for the hunting rifle's long scope). That
	// makes a 4x scope genuinely 4x, per weapon, with nothing to hand-tune. It is
	// recomputed each render on purpose: a real scope keeps its magnification as
	// you move your head back and merely narrows what you can see, which is what
	// recomputing d reproduces.
	//
	// Any positive value overrides the derivation. 2.4 was the hand-tuned hunting
	// rifle value and is kept as the default until the derived one is confirmed in
	// VR against it -- the derived figure is logged every heartbeat either way, so
	// the comparison costs nothing and replacing a known-good calibration with an
	// unverified formula costs a session.
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeFovDegrees, 2.4);
	// Frustum near/far planes for the scope camera. SetCameraFOV takes them as
	// (FAR, NEAR) — the code passes them in that order (v0.2.36 depth-inversion fix;
	// the swapped order reversed the projection → farthest-wins depth). near==far
	// produces a NaN projection and an eternally black render — the v0.2.x black-lens
	// root cause. 15 = the engine's default near; far generous for scoped sightlines.
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeNearClip, 15.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeFarClip, 250000.0);
	// Camera position in weapon-local space (applied to PrimaryWeaponScopeCamera's local
	// translate each render). Push the camera to the objective (front) end of the scope
	// tube so the render looks out of the glass, not at the tube interior. Y = forward
	// along the barrel in weapon space. Tune live: the TOML reloads on every scope-in.
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeCamOffsetX, 0.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeCamOffsetY, 15.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeCamOffsetZ, 0.0);
	// v0.2.71 — THE PERF FIX (§3.7e). Cull the scope accumulation against the scope
	// camera's own frustum instead of whatever was left in the camera's combined
	// frustum slot.
	//
	// BSShaderUtil::AccumulateScene does NOT cull with the BSCullingProcess we hand
	// it. It builds a fresh BSCullingGroup and calls BSCullingGroup::SetCamera
	// (0x140638270), which derives the six clip planes from
	//     *(NiFrustum**)(camera + 0x200)   -- the COMBINED (all-eye union) frustum
	//     camera + 0x70                    -- the camera's world transform
	// and NOT from the per-eye frustum array at camera+0x1a0 that SetCameraFOV fills.
	//
	// SetCameraFOV only rebuilds the combined frustum in its `if (1 < eyeCount)` tail
	// (FUN_141c2bf80). Our scope camera is MONO (logged eyes=1), so that tail never
	// runs: it writes eye 0 at [cam+0x1a0] and, via FUN_141c2bee0, only the NEAR
	// field of the combined frustum. The lateral extents and far plane stayed at
	// whatever the engine last left there -- so the accumulation culled against a
	// frustum unrelated to the scope, and the 2026-08-09 FOV sweep measured a 50x FOV
	// change moving the workload ~5% (14,411 passes at 2.4 deg vs 13,672 at 120 deg).
	// Cost: ~21 ms per render, ~2x the whole main scene, all of it geometry that a
	// 2.4 deg frustum should have rejected before the draw.
	//
	// The fix mirrors eye 0 into the combined slot right after SetCameraFOV, and also
	// sets the culling process's own frustum (NiCullingProcess::SetFrustum) the way
	// the engine's own cull helper FUN_141d4dc50 does, for the paths that do read it.
	// Live-toggleable so the §3.7e ladder is a clean A/B on one flag.
	MAKE_SETTING(bSetting, "TrueScopesVR", cullToScopeFrustum, true);
	// --- v0.2.73 PERF: the stopwatch and its two levers ------------------------
	// Per-stage timing of our own render: D3D11 timestamp queries on the GPU
	// timeline plus QPC on the CPU, means reported by DevBench /state and the
	// heartbeat. Flip OFF then ON to reset the accumulators — that is how you take
	// a clean measurement window without restarting anything.
	//
	// Why this exists: the v0.2.72 culling fix took the render 27.3 -> 13.6 ms, but
	// draw passes fell 19x while time fell only 2x, so the remaining cost is NOT
	// per-object submission. The two standing suspects (1024^2 render resolution,
	// 385 shadowed lights) are GUESSES, and the previous two perf theories that were
	// guesses (v0.2.71's combined frustum, and "the cull object isn't ours") were
	// both wrong. Measure the stages, then fix the one that is expensive.
	// Cost when on: 8 timestamp queries per render, collected 2-3 renders later
	// without ever blocking. Not free, but far below the noise floor of a VR frame.
	MAKE_SETTING(bSetting, "TrueScopesVR", perfTimers, true);
	// LEVER 1 — render resolution. Scales the scope camera's viewport to the
	// top-left sub-rectangle of the (engine-allocated, fixed 1024^2) scope G-buffer:
	// 0.5 = render at 512x512, quarter the pixels. Applied AFTER SetCameraFOV, which
	// forces the port back to full-frame {0,1,1,0} itself on every call.
	// Port semantics verified in Ghidra (TS_BSGraphicsState_BuildViewportFromCamDataRect,
	// 0x141d8d480): rect = {left, right, top, bottom, minDepth, maxDepth}, and
	// viewport = (w*left, (1-top)*h, (right-left)*w, (top-bottom)*h).
	// ⚠️ The projection is NOT touched, so the same image is squeezed into that
	// sub-rect while the lens delivery still samples the whole surface: the lens
	// will show the picture shrunk into one corner. THAT IS EXPECTED AND FINE for a
	// measurement run — it tells us what the resolution is WORTH before any work is
	// spent making the delivery sample the sub-rect. 1.0 = untouched.
	MAKE_SETTING(fSetting, "TrueScopesVR", perfRenderScale, 1.0);
	// LEVER 2 — light count. Clamps the two light-loop counts the deferred resolve
	// iterates (ShadowSceneNode +0x1a8 shadowed, +0x1c0 queued) across our resolve
	// call only, restoring them immediately after. 0 = draw no light volumes at all,
	// -1 = untouched. At a 2.4 deg FOV the engine still processes 385 shadowed
	// lights; a light volume that intersects a pinhole frustum projects across the
	// WHOLE target, so the overdraw is plausibly the remaining cost. This says so in
	// one reading instead of a session of theory.
	// The heartbeat keeps reporting the true counts (they are re-read after the
	// restore), so a clamped run is not silently indistinguishable from a normal one
	// — `lightsClamp=` in the log is the marker.
	MAKE_SETTING(iSetting, "TrueScopesVR", perfLightsMax, std::int64_t(-1));

	// Draw the engine's sun BSDFLightDir pass into our scope light accumulation before
	// the resolve (v0.2.26). Requires the resolve accum-bind hooks; falls back to the
	// v0.2.25 look (ambient + local lights only) when off or unavailable.
	MAKE_SETTING(bSetting, "TrueScopesVR", sunEnabled, true);
	// Scale on the sun's light color for the scope render only (1.0 = engine value).
	// Doubles as a diagnostic: if lowering this does not dim the lens, the overbright
	// artifact is not coming through the light color path.
	MAKE_SETTING(fSetting, "TrueScopesVR", sunBrightnessScale, 1.0);
	// Let the sun pass write the specular accumulation target. Its spec output is
	// currently garbage (world-pos reconstruction constants — under investigation);
	// off = sun contributes diffuse only and spec stays cleared. Turn on to re-test.
	MAKE_SETTING(bSetting, "TrueScopesVR", sunSpecEnabled, false);
	// Tone bisect (v0.2.36): the accum pre-clear is the ambient base light level.
	// Vanilla lazily clears to the fog RGB with alpha 1; the lens reads paler/cooler
	// than the world (17:58 outdoor A/B), suspect ambient double-count through this
	// clear. Scale the fog RGB (0 = black clear = the too-dark v0.2.27 look,
	// 1 = current) and set the alpha independently (if accum alpha is a mask term
	// the composite consumes, 1.0 everywhere may add uniform light — try 0).
	// 🎛️ USER-FACING: LENS BRIGHTNESS / AMBIENT FILL.
	// A flat fog-coloured fill added to the light accumulation before the sun and local
	// lights. Raising it lifts the shadows and washes the image out; lowering it deepens
	// contrast. Exposed deliberately as a taste / display-calibration knob (user's call,
	// 2026-08-10) rather than a fixed constant -- headsets and preferences differ.
	//   0.04  default. Bisected in VR against the unscoped world: 0.00 read slightly
	//         dark, 0.08 slightly bright, so the match sits between them.
	//   0.30  noticeably lifted;  1.0  blown out (this was the pre-v0.2.83 value, when
	//         the fill was standing in for a sun that contributed nothing).
	// ⚠️ COUPLED TO sunExecEnabled. This fill was always a SUBSTITUTE for the missing
	// sun; with a real sun it stacks on top. FOOTGUN: a low value WITH sunExecEnabled
	// false is a near-black lens. Move the two together.
	MAKE_SETTING(fSetting, "TrueScopesVR", accumClearScale, 0.04);
	MAKE_SETTING(fSetting, "TrueScopesVR", accumClearAlpha, 1.0);
	// Draw the sky into the lens (v0.2.36): accumulate the sky roots if the world
	// accumulation didn't already produce group-0xC passes, then draw group 0xC into
	// 0x61 after the composite (vanilla order: depth-tested, fills only far pixels).
	MAKE_SETTING(bSetting, "TrueScopesVR", skyEnabled, true);
	// Which sky roots to accumulate: 1 = dome only (Sky+0x8), 2 = sun/cloud only,
	// 3 = both. Bisects a faulting root live (the sun/cloud root carries the
	// occlusion-query glare geometry — prime suspect if the sky draw faults).
	MAKE_SETTING(iSetting, "TrueScopesVR", skyRootMask, std::int64_t(3));
	// v0.2.67 — THE CRESCENT (§3.2). Unbind the depth-stencil around the lens
	// delivery. Step 16 of the render restores DS logical 1 (vanilla's post-resolve
	// pattern), which rtm+0x15fc maps to physical 2 = the MAIN VR EYE's depth-
	// stencil; the ImageSpace delivery quad then draws with it bound and is
	// stencil-masked to the headset's hidden-area mesh. That accounts for every
	// measured property of the artifact: ~20% of 0x62 unwritten, a smooth curve a
	// quad and a scissor rect both cannot produce, the SAME arc from two different
	// effects (tonemap 0x27b08c0 and raw copy 0x27b0880, so not the shader), all
	// four corners clipped asymmetrically, and a boundary that does not move with
	// the camera. Live-toggleable for a clean A/B: flip it and re-dump 0x62.
	MAKE_SETTING(bSetting, "TrueScopesVR", deliveryUnbindDS, true);
	// --- widget fit (v0.2.68) -------------------------------------------------
	// Fit the vanilla VR scope widget to the REAL scope's lens instead of leaving it
	// at Bethesda's oversized floating disc. The widget mesh hangs off the engine's
	// "ScopeParent" node (player+0x7d0) and its render surface `render_circle:0` is a
	// disc of radius 7.852 centred on that node's origin, so
	//     ScopeParent.scale = widgetApertureRadius / 7.852
	// Shipped scopes measure an ocular radius of 0.76–4.56 (scale 0.097–0.581), i.e.
	// vanilla is 2–6x oversized — which is why the real scope mesh currently pokes
	// through the middle of the widget.
	// DEFAULT OFF: a wrong scale can make the lens vanish or swallow the view, and
	// that is indistinguishable from a broken render. Flip it live via DevBench.
	MAKE_SETTING(bSetting, "TrueScopesVR", widgetFitEnabled, false);
	// Ocular aperture radius, in mesh units, for scopes the plugin does NOT
	// recognise — modded optics, or a vanilla one whose node name is missing from
	// the built-in table. Default is the hunting rifle's glass shape (measured
	// 1.267). Recognised scopes use their own radius instead; see perScopeAperture.
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetApertureRadius, 1.267);
	// v0.2.85: look the aperture up PER SCOPE from the node names in the equipped
	// weapon's 3D, instead of using one number for every optic. The shipped table
	// covers the 26 vanilla scopes; [Scopes] in the TOML overrides and extends it.
	// Turn this off to go back to the single widgetApertureRadius above.
	MAKE_SETTING(bSetting, "TrueScopesVR", perScopeAperture, true);
	// Bypass the aperture math and set ScopeParent's scale directly (0 = derive).
	// For bisecting when the derived value looks wrong.
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetScaleOverride, 0.0);
	// Local-space nudge applied on top of the engine's own ScopeParent translation
	// (captured as a baseline, never accumulated). For sliding the shrunken widget
	// onto the real lens once the scale is right.
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetOffsetX, 0.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetOffsetY, 0.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetOffsetZ, 0.0);
	// Black-burst forensics (v0.2.43): per-frame 1-pixel GPU readback of the light
	// accum (0x6a) and composite (0x61); dark frames logged with raw pixel hex.
	// Costs two GPU sync stalls per render — enable only while hunting.
	MAKE_SETTING(bSetting, "TrueScopesVR", diagLensReadback, false);
	// Burst forensics (v0.2.45): color-code frames the fill hook did not fill —
	// RED = eye-gate says inactive (fill paused), GREEN = cadence skip. If a black
	// burst shows as a color instead, the burst is a non-filled frame.
	MAKE_SETTING(bSetting, "TrueScopesVR", diagPauseTint, false);
	// v0.2.54: dump the FULL lens chain (0x61 composite + 0x62 delivered) to BMPs
	// every N renders, into <Documents>/My Games/Fallout4VR/F4SE/TrueScopesDumps.
	// 0 = off. One-pixel readbacks report "lit" in every mode while the lens is
	// visibly black, so they can no longer distinguish "texture is fine" from
	// "texture is black except where we sample" — this shows the whole surface,
	// including where any black sits relative to the delivery footprint. Costs a
	// full-surface GPU copy + map + file write on the dump frame (capped at 80
	// files per session), so use a large N: 180 ~ every 2s at 90Hz.
	MAKE_SETTING(iSetting, "TrueScopesVR", diagDumpLensEveryNRenders, std::int64_t(0));
	// v0.2.56: on each dump event, also dump the intermediate deferred buffers —
	// G-buffer albedo 0x63 / normals 0x64 and light accum diffuse 0x6a / specular
	// 0x6b — so one capture shows WHICH stage first contains NaN (magenta) instead
	// of needing a lensMode ladder with a separate repro per rung. Each dump event
	// then writes 6 files (~19MB); the per-buffer NaN percentage is logged too, so
	// the log alone identifies the stage even without opening the images.
	MAKE_SETTING(bSetting, "TrueScopesVR", diagDumpBuffers, false);
	// v0.2.57: skip ONLY the sun's fullscreen BSDFLightDir exec, keeping the accum
	// clear/binds, camera state and pre-resolve G-buffer rebind that share its block.
	// sunEnabled=false is NOT equivalent — it drops all of those too and faults the
	// delivery (step 17, C0000005) since the ImageSpace copy needs the camera state.
	// DEFAULT FLIPPED TO false IN v0.2.62 — this pass is the black-lens bug.
	// Measured (v0.2.60/61): 0x6a reads the fog clear immediately BEFORE the exec on
	// every frame and NaN in all channels immediately after it on ~20% of frames
	// (sun6a=0/891, matching nan=891 exactly), with healthy inputs — eye view
	// matrices orthonormal, projection correct, eye positions mirrored, camdata=0.
	// One fullscreen additive draw then makes the whole accumulation buffer NaN,
	// which displays as BLACK while probing as LIT.
	// Turning it off costs NOTHING visually: §6.7's tone bisect proved the pass
	// contributes zero light today (brightness tracks accumClearScale linearly and
	// scale 0 is pitch black WITH the exec running — the "sunlit" look was always
	// fog-colored ambient). So this trades a defect for nothing until the pass is
	// fixed. Remaining suspect for the NaN: the pass config is rebuilt on the
	// engine's job-queue worker threads while we execute it on the render thread —
	// our cfgClean/cfgBuilt test is a check-then-use race. Needs a live x64dbg read
	// of the constant buffer at a NaN draw to confirm.
	// ✅ DEFAULT TRUE since v0.2.83 — VR-CONFIRMED 2026-08-09 (screenshot 20260809161650).
	// This was false from v0.2.62 because the pass wrote NaN and contributed no light.
	// BOTH causes are now fixed: it ran before the G-buffer existed (v0.2.78) and then
	// drew into the still-bound G-buffer MRT (v0.2.82). With those, the lens shows a
	// genuinely sun-lit scene. ⚠️ Coupled to accumClearScale — see the note there.
	MAKE_SETTING(bSetting, "TrueScopesVR", sunExecEnabled, true);
	// v0.2.76 — THE ACCUMULATION TARGET FIELD.
	// Ghidra 2026-08-09 (TS_DrawWorld_PreWorldLightingStage, 0x142846d60): the engine
	// re-selects `ctx+0x1c` before EVERY pass in the pre-world lighting stage, from the
	// renderer+4 reader:  ctx+0x1c = FUN_141d947d0(renderer) ? 0x6a : 0x24  — i.e. it is
	// the LIGHT-ACCUMULATION TARGET, and 0xffffffff is used for "none" elsewhere in the
	// same function. Our sun context comes from FUN_142812be0, which zeroes that field
	// and never sets it, so we have been exec'ing the sun pass with accum target 0.
	// -1 = leave the constructor's value alone (the pre-v0.2.76 behaviour, so the first
	// reading is clean); 0x6a = what the engine would set for a scoped pass.
	// Kept as an int rather than a bool so 0x24 can be tried without a rebuild.
	MAKE_SETTING(iSetting, "TrueScopesVR", sunCtxAccumTarget, -1);
	// v0.2.77 — sample the G-buffer (0x63 albedo / 0x64 normals) immediately before the
	// sun exec. A deferred directional light shades by SAMPLING the G-buffer, and ours
	// runs before the resolve that draws the G-buffer geometry — so it may be shading an
	// empty one, which computes exactly zero. Default ON: cheap, and the ordering
	// question is the live one.
	MAKE_SETTING(bSetting, "TrueScopesVR", diagSunOrderProbe, true);
	// v0.2.78 — THE §3.1 FIX. Run the sun BSDFLightDir exec from inside the resolve
	// (at the light-accum bind) instead of before it. Proven necessary v0.2.77: at the
	// old call site the G-buffer is still the black clear (albedo 0xFF000000, normals
	// 0x00000000), so the light had nothing to shade and computed exactly zero.
	// Default FALSE for one build so the A/B is a single live flag against the measured
	// baseline (0x6a meanLum 155.0); flip to true and watch that number move.
	// ✅ DEFAULT TRUE since v0.2.83 — this IS the fix, not an experiment.
	MAKE_SETTING(bSetting, "TrueScopesVR", sunExecInResolve, true);
	// v0.2.79 — re-write staging+0x1d0 (the inverse projection the sun pass reconstructs
	// position with) immediately before the exec.
	// ❌ NEGATIVE RESULT, DEFAULT FALSE (2026-08-09). The hypothesis was that the resolve
	// commits its own camera state between our pre-resolve write and the deferred exec,
	// leaving a stale/zeroed (but FINITE, hence undetected) inverse projection. It does
	// not. The value logged at the exec is already correct —
	//     row0=[0.021905374 0 0 ~0]  row3=[0 0 -0.06666266 0.06666666]
	// — and a controlled A/B in one spot reads 0x6a NaN 80.2% / meanLum 88.9 with this
	// ON and OFF and ON again, identical. It is a no-op; kept only so the negative is
	// re-testable without a rebuild. Do not spend time here again.
	MAKE_SETTING(bSetting, "TrueScopesVR", sunReapplyInvProj, false);
	// v0.2.82 — bind + COMMIT the light-accumulation MRT immediately before the deferred
	// sun exec. Measured (v0.2.81 ProbeBoundResources) at the two call sites:
	//   pre-resolve (old) : PS SRV [0,1,5,6,7],   RTs bound = 1
	//   in-resolve  (new) : PS SRV [0,1,2,3,5,8], RTs bound = 4
	// Four render targets is the G-BUFFER MRT, still bound: the RT manager STAGES binds
	// and commits them later, and ResolveAccumBind0Hook sits on the slot-0 staging call,
	// before slot 1 is staged and before the commit. The sun was therefore drawing into
	// the G-buffer, corrupting albedo/normals, and the resolve's light volumes read NaN
	// normals afterwards -- which is why the NaN was exactly the geometry pixels and the
	// sky stayed clean.
	MAKE_SETTING(bSetting, "TrueScopesVR", sunResolveRebindAccum, true);
	// Re-arm the renderer on scope-in after a fault (the fault latch is otherwise
	// session-permanent). Lets a faulting config be bisected via TOML edits without
	// restarting the game. The faulting step is logged each time.
	MAKE_SETTING(bSetting, "TrueScopesVR", retryAfterFault, true);
	// --- DevBench (dev tooling, not a gameplay feature) -----------------------
	// Localhost query server that lets an agent read live state, flip the
	// settings above, and tail the log without a rebuild/relaunch cycle. See
	// src/DevBench/DevBench.h.
	//
	// Opens a loopback-only listening socket that can read arbitrary process
	// memory through /read. Correct trade for a local dev bench, and no release
	// gate is needed: this repo is the RESEARCH repo and never ships. The
	// production mod will be a brand-new repo rebuilt with only shipping code,
	// so DevBench is simply not copied across.
	MAKE_SETTING(bSetting, "TrueScopesVR", devbenchEnabled, true);
	// First port tried; the server walks up to +15 if it is busy and logs the
	// one it actually bound. 8930 keeps clear of alandtse/devbench's 8920/8921.
	MAKE_SETTING(iSetting, "TrueScopesVR", devbenchPort, std::int64_t(8930));

	// Suppress the vanilla scope-in world-blackout imagespace modifier (ScopeMenu's
	// full-strength zoomData imod). Core to the world+scope experience.
	MAKE_SETTING(bSetting, "TrueScopesVR", disableScopeBlackout, true);
	// Also suppress the eye-approach dimming fade (cosmetic; vanilla feel if left on).
	MAKE_SETTING(bSetting, "TrueScopesVR", disableApproachFade, false);

#undef MAKE_SETTING

	// Installed by DevBench so live overrides survive the scope-in TOML reload.
	// Without it, every /config/set would be silently reverted the next time the
	// player raised the scope --- an experiment that quietly measures the file's
	// values instead of the ones you set is worse than no experiment.
	inline std::function<void()> postLoadHook;

	// --- [Scopes] : per-scope aperture overrides (v0.2.85) --------------------
	// A free-form table, `NodeName = radius`, keyed by a node name from the
	// equipped weapon's 3D. It is NOT a fixed list of settings, because the whole
	// point is that a user can add an entry for a scope the plugin has never seen
	// -- the log prints the node names whenever a lookup misses, so adding one is
	// copy-paste rather than reverse engineering.
	//
	// Written on the game thread at scope-in, read on the render thread during a
	// probe, hence the lock. Contention is nil (both are rare events); correctness
	// is not, since the map reallocates on reload.
	inline std::mutex                                     scopeApertureLock;
	inline std::map<std::string, double, std::less<>>     scopeApertures;

	// Radius for a node name, or 0.0 when the user has not overridden it. 0 means
	// "no opinion" rather than "zero radius": a real 0 would collapse the widget,
	// so it is rejected at load rather than being allowed to mean something.
	[[nodiscard]] inline double ScopeApertureOverride(std::string_view a_node)
	{
		const std::scoped_lock lock(scopeApertureLock);
		const auto             it = scopeApertures.find(a_node);
		return it == scopeApertures.end() ? 0.0 : it->second;
	}

	inline void load()
	{
		toml::table config;
		try {
			config = toml::parse_file("Data/F4SE/Plugins/TrueScopesVR.toml"sv);
		} catch (const std::exception& e) {
			logger::warn(FMT_STRING("TrueScopesVR.toml not loaded ({}), using defaults"), e.what());
			return;
		}

#define LOAD(a_setting)                                                              \
	if (const auto tweak = config[a_setting.group()][a_setting.key()]; tweak) {      \
		if (const auto value = tweak.as<decltype(a_setting)::value_type>(); value) { \
			*a_setting = value->get();                                               \
		}                                                                            \
	}

		LOAD(fillEnabled);
		LOAD(fillEveryNFrames);
		LOAD(scopeOffHoldMs);
		LOAD(forceAlwaysOn);
		LOAD(lensMode);
		LOAD(scopeFovDegrees);
		LOAD(scopeNearClip);
		LOAD(scopeFarClip);
		LOAD(scopeCamOffsetX);
		LOAD(scopeCamOffsetY);
		LOAD(scopeCamOffsetZ);
		LOAD(cullToScopeFrustum);
		LOAD(perfTimers);
		LOAD(perfRenderScale);
		LOAD(perfLightsMax);
		LOAD(sunEnabled);
		LOAD(sunBrightnessScale);
		LOAD(sunSpecEnabled);
		LOAD(accumClearScale);
		LOAD(accumClearAlpha);
		LOAD(skyEnabled);
		LOAD(skyRootMask);
		LOAD(deliveryUnbindDS);
		LOAD(widgetFitEnabled);
		LOAD(widgetApertureRadius);
		LOAD(perScopeAperture);
		LOAD(widgetScaleOverride);
		LOAD(widgetOffsetX);
		LOAD(widgetOffsetY);
		LOAD(widgetOffsetZ);
		LOAD(retryAfterFault);
		LOAD(diagLensReadback);
		LOAD(diagPauseTint);
		LOAD(diagDumpLensEveryNRenders);
		LOAD(diagDumpBuffers);
		LOAD(sunExecEnabled);
		LOAD(sunCtxAccumTarget);
		LOAD(diagSunOrderProbe);
		LOAD(sunExecInResolve);
		LOAD(sunReapplyInvProj);
		LOAD(sunResolveRebindAccum);
		LOAD(disableScopeBlackout);
		LOAD(disableApproachFade);
		LOAD(devbenchEnabled);
		LOAD(devbenchPort);

#undef LOAD

		// [Scopes] -- every key is a node name, so it cannot be a LOAD() list.
		{
			std::map<std::string, double, std::less<>> fresh;
			if (const auto* scopes = config["Scopes"].as_table()) {
				for (const auto& [key, value] : *scopes) {
					double radius = 0.0;
					if (const auto* f = value.as_floating_point()) {
						radius = f->get();
					} else if (const auto* i = value.as_integer()) {
						radius = static_cast<double>(i->get());
					}
					// A zero or absurd radius makes the lens vanish or swallow the
					// view, which reads as a broken render rather than a bad setting.
					// Say which entry is wrong instead of silently fitting to it.
					if (radius > 0.01 && radius < 64.0) {
						fresh.emplace(std::string(key.str()), radius);
					} else {
						logger::warn(FMT_STRING("[Scopes] {} = {} ignored (expected a radius between 0.01 and 64)"),
							std::string(key.str()), radius);
					}
				}
			}
			const std::scoped_lock lock(scopeApertureLock);
			scopeApertures = std::move(fresh);
			if (!scopeApertures.empty()) {
				logger::info(FMT_STRING("[Scopes]: {} aperture override(s) loaded"), scopeApertures.size());
			}
		}

		logger::info(
			FMT_STRING("settings: fillEnabled={} fillEveryNFrames={} forceAlwaysOn={} lensMode={} scopeFovDegrees={} sunEnabled={} disableScopeBlackout={} disableApproachFade={}"),
			*fillEnabled, *fillEveryNFrames, *forceAlwaysOn, *lensMode, *scopeFovDegrees, *sunEnabled, *disableScopeBlackout, *disableApproachFade);

		if (postLoadHook) {
			postLoadHook();
		}
	}
}
