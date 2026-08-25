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
	// v0.2.124 — ONE EURO CAMERA DAMPING (STATUS 3.7d): magnification amplifies
	// hand tremor ~6x at 4x zoom; a speed-adaptive filter crushes at-rest
	// tremor without lagging deliberate pans. Filters ONLY our scope render
	// camera's orientation — never hands, weapon, or vanilla state. Code
	// default OFF (poseGateEnabled precedent); the deployed TOML enables it
	// for the field-judgment session.
	MAKE_SETTING(bSetting, "TrueScopesVR", camSmoothEnabled, false);
	MAKE_SETTING(fSetting, "TrueScopesVR", camSmoothMinCutoffHz, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", camSmoothBeta, 0.5);
	MAKE_SETTING(fSetting, "TrueScopesVR", camSmoothDCutoffHz, 1.0);
	MAKE_SETTING(iSetting, "TrueScopesVR", camSmoothResetMs, std::int64_t(250));
	MAKE_SETTING(fSetting, "TrueScopesVR", camSmoothSnapDegrees, 30.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", camSmoothMaxLagDegrees, 2.0);
	// v0.2.125 — fill the lens ONCE when the widget comes up (per equip), so the
	// disc never sits black before the first aim (field 2026-08-24 21:07: the
	// placement converged in 314 ms but the picture waited 33 s for a scope-in).
	MAKE_SETTING(bSetting, "TrueScopesVR", lensPrimeOnPresence, true);
	// Re-fill the FROZEN lens every N seconds while the widget is up (0 = off).
	// Each refresh costs one render-length frame hitch — an A/B knob, not a
	// shipping default.
	MAKE_SETTING(fSetting, "TrueScopesVR", poseIdleRefreshSeconds, 0.0);
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
	// --- own delivery pass (v0.2.104) ----------------------------------------
	// After the engine tonemap writes the picture into 0x62, run our own fullscreen
	// pixel shader over it: composite the engine's Scaleform reticle (Test B
	// 2026-08-23 proved the vanilla reticle quad sits UNDER our picture disc) and
	// apply the glass look. Everything below is live-tunable via DevBench.
	// v0.2.109: controller verdict chords (grip+A yes / grip+B no / grip+trigger
	// skip) for guided test passes; read passively off OpenVR, haptic ack.
	MAKE_SETTING(bSetting, "TrueScopesVR", verdictInputEnabled, true);
	// v0.2.111 DEBUG: arm the scope render directly, bypassing the vanilla
	// raise detection. For HEADLESS lens verification (null driver + /dump/now):
	// the widget rig never exists, but the render + composite + delivery all run
	// and 0x61/0x62 dump normally. Not for gameplay.
	MAKE_SETTING(bSetting, "TrueScopesVR", forceScopeActive, false);
	MAKE_SETTING(bSetting, "TrueScopesVR", lensCompositeEnabled, true);
	// Composite the reticle from the ScopeMenu renderer's offscreen RT. Hides the
	// vanilla `render_UI:0` quad while active (restored on scope-off).
	MAKE_SETTING(bSetting, "TrueScopesVR", reticleEnabled, true);
	// Interface3D renderer to take the reticle from (the VR scope menu's name).
	MAKE_SETTING(sSetting, "TrueScopesVR", reticleRendererName, std::string("ScopeMenu"));
	// true = +0x1d8 (HUD-glass post-effected copy), false = +0x1dc (raw crisp strokes).
	MAKE_SETTING(bSetting, "TrueScopesVR", reticleUseGlassed, false);
	MAKE_SETTING(fSetting, "TrueScopesVR", reticleAlpha, 1.0);
	// lens-uv -> reticle-uv: ruv = (uv-0.5)*scale + 0.5 + offset. Scale > 1 shrinks
	// the reticle on the lens; use X != Y to undo any aspect squash of the source RT.
	MAKE_SETTING(fSetting, "TrueScopesVR", reticleScaleX, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", reticleScaleY, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", reticleOffsetX, 0.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", reticleOffsetY, 0.0);
	// Radial vignette in disc units (1.0 = the picture disc's edge): full brightness
	// inside `inner`, fades to `1-strength` by `outer`; `power` shapes the curve.
	MAKE_SETTING(fSetting, "TrueScopesVR", vignetteInner, 0.78);
	MAKE_SETTING(fSetting, "TrueScopesVR", vignetteOuter, 1.02);
	MAKE_SETTING(fSetting, "TrueScopesVR", vignetteStrength, 0.85);
	MAKE_SETTING(fSetting, "TrueScopesVR", vignettePower, 1.0);
	// Colour multiplier + exposure applied to the picture before the reticle.
	MAKE_SETTING(fSetting, "TrueScopesVR", lensTintR, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", lensTintG, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", lensTintB, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", lensExposure, 1.0);
	// v0.2.111 in-lens recreations of the suppressed zoom imods. Strengths are
	// 0..1 master faders over looks derived from the IMAD records themselves:
	// NV: tint (0.31,0.82,0.22)@0.69 + brightness x1.1+0.4 + gain (zd_ScopeNightVision)
	// Recon: desaturate to ~0.3, warm tint (1,0.88,0.84)@0.39, contrast x0.2+0.8
	// (zd_ScopeTargetingRecon). Screen-type scopes also drop the radial vignette
	// for a square soft edge automatically.
	MAKE_SETTING(fSetting, "TrueScopesVR", nvEffectStrength, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", nvGain, 1.6);
	MAKE_SETTING(fSetting, "TrueScopesVR", reconEffectStrength, 1.0);
	// --- glass effects (v0.2.113) --------------------------------------------
	// The deferred half of the own-delivery-pass design (SESSION_2026-08-23_
	// RETICLE_AND_GLASS.md §4 step 3). All three apply to OPTICAL tubes only —
	// screen-type optics (recon/thermal) are displays and skip them.
	//
	// Eye-box / exit pupil: the picture clips and dims as the head moves off the
	// tube axis, computed per frame from the REAL head-to-scope pose (camera
	// root vs ScopeParent — the FTS getparallax idea, fed from the HMD). On axis
	// it is a no-op. Strength 0 disables (and skips the pose math); gain scales
	// how strongly head movement moves the shadow — raise it for a twitchier,
	// less forgiving scope.
	// Strength 1.0 = the shadow goes fully black (a real scope shadow is);
	// lower it only for a deliberately see-through shadow — the first headset
	// look (2026-08-24) called the 0.85 floor out as "you can still make out
	// the render".
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxStrength, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxGain, 1.5);
	// Interpupillary distance in GAME UNITS (1 unit ~ 1.43 cm; 64 mm ~ 4.5).
	// The camera root is the HMD centre, so the aiming eye sits half of this
	// off it; the eye-box tests both eyes and follows whichever is closer to
	// the tube axis.
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxIpdUnits, 4.5);
	// Edge blur: field-curvature softness from edgeBlurStart (disc radius 0..1)
	// out to the rim. 0 disables.
	MAKE_SETTING(fSetting, "TrueScopesVR", edgeBlurStrength, 0.35);
	MAKE_SETTING(fSetting, "TrueScopesVR", edgeBlurStart, 0.7);
	// Chromatic fringe at the rim (red/blue focal split), 0..1. 0 disables.
	MAKE_SETTING(fSetting, "TrueScopesVR", caStrength, 0.35);
	// v0.2.123 — THE GLASS SUITE (encapsulation feedback 2026-08-24: "the render
	// feels apart from the scope mesh... encapsulate it... glassy lens overlay").
	// All live-tunable; every effect is an exact no-op at strength 0 (the Dim()
	// constraint). Field-judgment pending.
	// HOUSING RIM SHADOW — a hard near-black annulus at the disc edge that reads
	// as the ocular tube wall (distinct from the soft vignette). Optical only.
	MAKE_SETTING(fSetting, "TrueScopesVR", rimShadowStrength, 0.9);
	MAKE_SETTING(fSetting, "TrueScopesVR", rimShadowStart, 0.86);
	MAKE_SETTING(fSetting, "TrueScopesVR", rimShadowEnd, 1.0);
	// Center drop in disc units — the shadow intrudes deeper at the top like a
	// real tube interior lit from above. Rides the disc, so it cants with the gun.
	MAKE_SETTING(fSetting, "TrueScopesVR", rimShadowTopBias, 0.05);
	// Shadow center shifts opposite the eye by this factor of the RAW eye offset
	// — the rim behaves as NEAR geometry against the far image.
	MAKE_SETTING(fSetting, "TrueScopesVR", rimShadowParallax, 0.2);
	// GLASS SHEEN — additive view-dependent glint + fresnel rim lift + smudge.
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenStrength, 0.06);
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenWidth, 0.55);
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenTravel, 0.9);
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenFresnel, 0.04);
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenSmudge, 0.35);
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenSmudgeScale, 9.0);
	// v0.2.126 — dark-adaptive glass: multiply the sheen term by up to (1+this)
	// as the composed picture darkens. 0 = off. The field verdict behind it:
	// a black lens reads as a dead screen; real glass keeps surface life.
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenDarkBoost, 3.0);
	// v0.2.129 — how much the RETICLE follows the eye-box clip (real-scope
	// observation: the reticle stays faintly visible inside the blacked-out eye
	// box). 1 = clips with the picture (pre-.129 behavior), 0 = always visible.
	MAKE_SETTING(fSetting, "TrueScopesVR", reticleEyeBoxFollow, 0.35);
	// v0.2.130 — the eye-box clip bottoms out at this flat scatter level
	// instead of pure black (scene-independent: a constant, never the world).
	// This is what makes the dark reticle visible in the black at all.
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxResidual, 0.07);
	// v0.2.131 — the residual adapts to scene brightness (avg picture luminance
	// x this scale, saturated): full glow in daylight, fading toward true black
	// at night so the eye-box never reads as a lit screen in the dark. 0 =
	// constant residual (v0.2.130 behavior).
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxResidualAdapt, 4.0);
	// v0.2.132 - draw deferred-decal group 5 AFTER the opaque G-buffer groups
	// in our resolve (the engine's own order paints walls over placed decals in
	// a resolve-only render). Off = vanilla resolve order.
	MAKE_SETTING(bSetting, "TrueScopesVR", decalGroup5Reorder, true);
	// v0.2.133 - run the engine's deferred-decal stage (bullet holes, screen-
	// space impacts) inside our render, against our scope camera. Twice-
	// adversarially-verified; off = pre-.133 (no shot impacts in the lens).
	MAKE_SETTING(bSetting, "TrueScopesVR", decalStageEnabled, true);
	// v0.2.132 - also drop accumulator group 0x17 (sun glare) from our render:
	// same stale-pass exposure as the v0.2.11 fault class, drawn by the resolve
	// tail. Hardening; off restores pre-.132 behavior.
	MAKE_SETTING(bSetting, "TrueScopesVR", dropSunGlareGroup, true);
	// PARALLAX DEPTH — picture UV shifts against the eye so the image plane
	// reads D game-units BEHIND the lens (1 unit ~ 1.43 cm). 0 = off.
	// Field-judged 2026-08-25 against a REAL scope: "way more parallax than we
	// got" - physically right, the image through a distance-focused scope sits
	// near optical INFINITY (D/(L+D) -> 1). 20 gives ~0.83 at rifle relief.
	MAKE_SETTING(fSetting, "TrueScopesVR", parallaxDepthUnits, 20.0);
	// Raised 0.08 -> 0.15 with the depth bump (2026-08-25); watch the shifted
	// edge for a dark smear band - if seen, this cap is the first knob down.
	MAKE_SETTING(fSetting, "TrueScopesVR", parallaxMaxShift, 0.15);
	MAKE_SETTING(fSetting, "TrueScopesVR", parallaxSmoothing, 0.3);
	MAKE_SETTING(fSetting, "TrueScopesVR", parallaxMinEyeRelief, 4.0);
	// Field-judged against a REAL scope 2026-08-25: the reticle moves 1:1 WITH
	// the image (a scope parallax-adjusted at distance has coincident planes).
	// 0 = rim-anchored (the old default) for anyone who prefers it.
	MAKE_SETTING(fSetting, "TrueScopesVR", reticleParallaxFraction, 1.0);
	// NV phosphor scanlines, 0..1. Judged in the headset 2026-08-24 at 0.8
	// ("that looks really nice") — ships ON at that value.
	MAKE_SETTING(fSetting, "TrueScopesVR", nvScanlines, 0.8);
	// --- pose-based activation (v0.2.116) ------------------------------------
	// Replace the vanilla eye-proximity gate's verdict with our own pose test
	// (PoseGate.h has the mechanism write-up). The vanilla gate's 38-unit
	// distance cap makes pistol scopes impossible to activate at arm's length;
	// ours tests eye→ocular distance, the eye's lateral offset from the actual
	// tube axis (distance-invariant — wide angle up close, narrow at arm's
	// length, like real optics), and a head look cone. Each has an enter/exit
	// pair for spatial hysteresis. DEFAULT OFF in code until judged in the
	// headset; the deployed TOML turns it on for testing.
	MAKE_SETTING(bSetting, "TrueScopesVR", poseGateEnabled, false);
	// Eye→ocular distance band, game units (1 ≈ 1.43 cm). 90 ≈ 1.3 m covers a
	// pistol at full extension with margin; vanilla's cap was 38/40.
	MAKE_SETTING(fSetting, "TrueScopesVR", poseMaxDistance, 90.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", poseExitDistance, 100.0);
	// Eye's perpendicular distance from the tube axis, game units. 6 ≈ 8.6 cm:
	// ~26 deg off-axis at a shouldered rifle (13 units), ~6.9 deg at arm's
	// length (50) — the natural distance-scaled cone.
	MAKE_SETTING(fSetting, "TrueScopesVR", poseMaxLateral, 6.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", poseExitLateral, 9.0);
	// Head orientation: angle between HMD forward and the direction to the
	// ocular. Vanilla used 25/35 against the weapon; ours is against the scope.
	MAKE_SETTING(fSetting, "TrueScopesVR", poseLookConeDegrees, 35.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", poseLookConeExitDegrees, 45.0);
	// Widget presence: true = the scope widget MESHES stay visible the whole
	// time the weapon is drawn (lens FROZEN while the pose is inactive — RT
	// 0x62 persists, so freeze = don't fill; no pop-in). v0.2.118: this is now
	// PLUGIN-OWNED node visibility only — the verdict fed to vanilla is always
	// the pose, so sighted/ScopeMenu/Pip-Boy/FRIK behave identically with this
	// on or off. (The v0.2.116 version routed the VERDICT, which kept the
	// player sighted, ScopeMenu open, FRIK's body collapsed and the Pip-Boy
	// blocked whenever the weapon was drawn — field-diagnosed 2026-08-24.)
	// false = the widget appears/disappears with the pose, vanilla-style.
	MAKE_SETTING(bSetting, "TrueScopesVR", poseWidgetAlways, true);
	// One-shot dim applied to the frozen lens picture on the live→frozen edge,
	// so a stale picture does not read as live. 0..1 multiplier; 1.0 = no dim.
	MAKE_SETTING(fSetting, "TrueScopesVR", poseFrozenDim, 0.55);
	// v0.2.120: hide the widget model's own housing meshes (scope_Hunting:0 /
	// scope_recon:0 in world_scope.nif - the pale speckled ring Bethesda drew
	// around the picture). The REAL weapon's scope provides the housing; the
	// widget only needs its render surfaces. Re-hidden every eligible frame
	// because the engine re-shows them on scope-in edges.
	MAKE_SETTING(bSetting, "TrueScopesVR", hideWidgetHousing, true);
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
	// v0.2.123 — under-aperture sizing: multiplies the derived aperture so the
	// picture disc sits slightly INSIDE the real housing hole and no bright
	// pixel ever touches the seam (encapsulation layer 2). 1.0 = exact fit.
	// Clamped to [0.8, 1.1] at use. widgetScaleOverride bypasses it (bisect tool).
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetApertureScale, 0.97);
	// v0.2.85: look the aperture up PER SCOPE from the node names in the equipped
	// weapon's 3D, instead of using one number for every optic. The shipped table
	// covers the 26 vanilla scopes; [Scopes] in the TOML overrides and extends it.
	// Turn this off to go back to the single widgetApertureRadius above.
	MAKE_SETTING(bSetting, "TrueScopesVR", perScopeAperture, true);
	// v0.2.122 — STALENESS GUARD ceiling: auto-placement refuses a target that is
	// farther than this many units from the eye. Right after a save load the
	// weapon 3D walks fine but its WORLD transforms still hold the pre-placement
	// rig pose (~350 units from the eye in the field logs) until the first
	// skeleton update; an equipped scope can never actually be beyond arm's
	// length + weapon (~60 units). The refusal feeds PresenceFit's bounded retry
	// (v0.2.121), which converges as soon as the transforms are real.
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetPlaceMaxEyeDist, 100.0);
	// Bypass the aperture math and set ScopeParent's scale directly (0 = derive).
	// For bisecting when the derived value looks wrong.
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetScaleOverride, 0.0);
	// Local-space nudge applied on top of the engine's own ScopeParent translation
	// (captured as a baseline, never accumulated). For sliding the shrunken widget
	// onto the real lens once the scale is right.
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetOffsetX, 0.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetOffsetY, 0.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetOffsetZ, 0.0);
	// Put the disc on the optic's ocular face automatically instead of relying on
	// a hand-tuned offset per scope. ON by default since v0.2.98: VR-confirmed on
	// two very different optics (the hunting rifle's tube and the recon scope's
	// flat screen), converging in 2 steps to a residual under 0.01 units on both.
	//
	// It aims at the census-measured face where the scope has a row, and falls
	// back to the rear of the optic's world bounding sphere along the eye axis
	// where it does not — the two differ by ~1-2 units on scopes we can compare,
	// so an unmeasured or modded optic lands close but not exactly.
	//
	// Turning it OFF returns to widgetOffset* / [Scopes] offsets, which is the
	// escape hatch if some optic ends up placed badly. The candidate is computed
	// and reported either way (log line "WIDGET AUTO-PLACE", DevBench /scope
	// "placement"), so it can always be compared against a hand-tuned value.
	MAKE_SETTING(bSetting, "TrueScopesVR", widgetAutoPlace, true);
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

	// v0.2.111: back ON, with the name it earned. The per-zoom imods
	// (zd_ScopeNightVision 0x94636, zd_ScopeTargetingRecon 0x2041b6 - standard
	// scopes carry none) are FULLSCREEN imagespace modifiers authored for the flat
	// game, where scoping owned the whole screen. In VR they tint the world, not
	// the scope (field-confirmed 2026-08-23 rows 3/4). Suppress them globally and
	// recreate the looks INSIDE the lens composite (nv*/recon* knobs below), keyed
	// off the zoom's imod identity. Old TOML key disableScopeBlackout still loads.
	MAKE_SETTING(bSetting, "TrueScopesVR", suppressScopeImods, true);
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
	// v0.2.91: a scope entry carries POSITION as well as size. VR screenshots on
	// 2026-08-10 (20260810134759 / ...134809) showed the disc sitting above and
	// behind a non-hunting-rifle scope, plainly detached from it: widgetOffsetY
	// = -1.8 was tuned for the hunting rifle and is wrong for every other optic.
	// Radius alone can never fix that -- a correctly sized disc in the wrong place
	// still misses the lens.
	//
	// NaN means "not specified, use the global setting", which is distinct from a
	// deliberate 0.0. Offsets legitimately want to be zero, so 0 cannot double as
	// the absent value the way it can for a radius.
	struct ScopeEntry
	{
		double aperture = 0.0;  // 0 = not specified
		double offsetX = std::numeric_limits<double>::quiet_NaN();
		double offsetY = std::numeric_limits<double>::quiet_NaN();
		double offsetZ = std::numeric_limits<double>::quiet_NaN();
		// v0.2.112: height/width of a RECTANGULAR screen optic, 0 = not
		// specified (circular optics and square screens are 1.0 in the built-in
		// table). When < 1, the aperture is the screen's HALF-WIDTH and the
		// composite masks the picture's vertical overshoot, so a wide display
		// (MGScopeThermal, 2.55 x 1.37) shows a wide picture instead of a
		// circle spilling above and below it.
		double aspect = 0.0;
	};

	// v0.2.101: a [Scopes] key may now be a MODEL PATH as well as a node name,
	// because a node name cannot address a modded optic that copied a vanilla
	// mesh's root name without also addressing the vanilla one. Paths mean the
	// key has to survive being written by a human, so it is normalized on BOTH
	// sides -- once when the TOML is loaded, once when a lookup is made:
	//
	//   lower-cased          the path in the ESM is whatever the author typed
	//   backslash -> slash   so "Weapons/HuntingRifle/X.nif" needs no \\ escaping
	//                        in TOML, which is the form people get wrong
	//   leading "meshes/"    dropped; model paths are relative to Meshes already,
	//                        but it is the obvious thing to paste from a file
	//                        browser
	//   trailing "_1.nif"    folded to ".nif" -- the OMOD names the world mesh
	//                        and the engine loads the "_1" first-person twin
	//                        (FUN_14130c910), so both name one optic
	//
	// THESE RULES MUST MATCH mesh_key() in tools/scope-census.py, which produces
	// the built-in table's keys. A divergence would not throw; it would simply
	// never match, and a table that never matches looks exactly like a table
	// whose numbers happen to equal the fallback -- the trap that hid the
	// v0.2.85 lookup being dead for four versions.
	// ONE implementation, in a buffer form, because the caller that matters most
	// runs inside the probe's SEH guard: allocating there would leak a std::string
	// on the fault path that disables probing. The std::string overload below is
	// a wrapper, so the two can never drift apart.
	inline void NormalizeScopeKeyInto(std::string_view a_key, char* a_out, std::size_t a_cap)
	{
		if (!a_out || a_cap == 0) {
			return;
		}
		std::size_t                n = 0;
		constexpr std::string_view meshes = "meshes\\";
		constexpr std::string_view meshesFwd = "meshes/";
		std::size_t                start = 0;
		if (a_key.size() >= meshes.size()) {
			const auto head = a_key.substr(0, meshes.size());
			bool       isMeshes = true;
			for (std::size_t i = 0; i < meshes.size(); ++i) {
				const auto c = static_cast<char>(std::tolower(static_cast<unsigned char>(head[i])));
				if (c != meshes[i] && c != meshesFwd[i]) {
					isMeshes = false;
					break;
				}
			}
			if (isMeshes) {
				start = meshes.size();
			}
		}
		for (std::size_t i = start; i < a_key.size() && n + 1 < a_cap; ++i) {
			const char c = a_key[i];
			a_out[n++] = (c == '\\') ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		a_out[n] = '\0';

		constexpr std::string_view fp = "_1.nif";
		if (n >= fp.size() && std::string_view{ a_out, n }.ends_with(fp)) {
			n -= fp.size();
			// ".nif" is shorter than "_1.nif", so this always fits.
			std::memcpy(a_out + n, ".nif", 5);
		}
	}

	[[nodiscard]] inline std::string NormalizeScopeKey(std::string_view a_key)
	{
		char buf[512] = {};
		NormalizeScopeKeyInto(a_key, buf, sizeof(buf));
		return buf;
	}

	inline std::mutex                                     scopeApertureLock;
	// Keyed by NormalizeScopeKey(...) of whatever the TOML said, so lookups are
	// case- and separator-insensitive for paths and, harmlessly, for node names.
	inline std::map<std::string, ScopeEntry, std::less<>> scopeEntries;

	// The entry for a node name or model path, or an all-unspecified one. Never
	// throws, never blocks for long: both writer (scope-in) and reader (a probe)
	// are rare.
	[[nodiscard]] inline ScopeEntry ScopeEntryFor(std::string_view a_key)
	{
		const auto             norm = NormalizeScopeKey(a_key);
		const std::scoped_lock lock(scopeApertureLock);
		const auto             it = scopeEntries.find(norm);
		return it == scopeEntries.end() ? ScopeEntry{} : it->second;
	}

	// Kept as its own accessor because the aperture is what decides whether a scope
	// counts as "known", and 0 is the unambiguous "no opinion" for it.
	[[nodiscard]] inline double ScopeApertureOverride(std::string_view a_key)
	{
		return ScopeEntryFor(a_key).aperture;
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
		LOAD(camSmoothEnabled);
		LOAD(camSmoothMinCutoffHz);
		LOAD(camSmoothBeta);
		LOAD(camSmoothDCutoffHz);
		LOAD(camSmoothResetMs);
		LOAD(camSmoothSnapDegrees);
		LOAD(camSmoothMaxLagDegrees);
		LOAD(lensPrimeOnPresence);
		LOAD(poseIdleRefreshSeconds);
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
		LOAD(verdictInputEnabled);
		LOAD(forceScopeActive);
		LOAD(lensCompositeEnabled);
		LOAD(reticleEnabled);
		LOAD(reticleRendererName);
		LOAD(reticleUseGlassed);
		LOAD(reticleAlpha);
		LOAD(reticleScaleX);
		LOAD(reticleScaleY);
		LOAD(reticleOffsetX);
		LOAD(reticleOffsetY);
		LOAD(vignetteInner);
		LOAD(vignetteOuter);
		LOAD(vignetteStrength);
		LOAD(vignettePower);
		LOAD(lensTintR);
		LOAD(lensTintG);
		LOAD(lensTintB);
		LOAD(lensExposure);
		LOAD(nvEffectStrength);
		LOAD(nvGain);
		LOAD(reconEffectStrength);
		LOAD(eyeBoxStrength);
		LOAD(eyeBoxGain);
		LOAD(eyeBoxIpdUnits);
		LOAD(edgeBlurStrength);
		LOAD(edgeBlurStart);
		LOAD(caStrength);
		LOAD(rimShadowStrength);
		LOAD(rimShadowStart);
		LOAD(rimShadowEnd);
		LOAD(rimShadowTopBias);
		LOAD(rimShadowParallax);
		LOAD(sheenStrength);
		LOAD(sheenWidth);
		LOAD(sheenTravel);
		LOAD(sheenFresnel);
		LOAD(sheenSmudge);
		LOAD(sheenSmudgeScale);
		LOAD(sheenDarkBoost);
		LOAD(reticleEyeBoxFollow);
		LOAD(eyeBoxResidual);
		LOAD(eyeBoxResidualAdapt);
		LOAD(decalGroup5Reorder);
		LOAD(decalStageEnabled);
		LOAD(dropSunGlareGroup);
		LOAD(parallaxDepthUnits);
		LOAD(parallaxMaxShift);
		LOAD(parallaxSmoothing);
		LOAD(parallaxMinEyeRelief);
		LOAD(reticleParallaxFraction);
		LOAD(nvScanlines);
		LOAD(poseGateEnabled);
		LOAD(poseMaxDistance);
		LOAD(poseExitDistance);
		LOAD(poseMaxLateral);
		LOAD(poseExitLateral);
		LOAD(poseLookConeDegrees);
		LOAD(poseLookConeExitDegrees);
		LOAD(poseWidgetAlways);
		LOAD(poseFrozenDim);
		LOAD(hideWidgetHousing);
		LOAD(widgetFitEnabled);
		LOAD(widgetApertureRadius);
		LOAD(widgetApertureScale);
		LOAD(perScopeAperture);
		LOAD(widgetScaleOverride);
		LOAD(widgetOffsetX);
		LOAD(widgetOffsetY);
		LOAD(widgetOffsetZ);
		LOAD(widgetAutoPlace);
		LOAD(widgetPlaceMaxEyeDist);
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
		LOAD(suppressScopeImods);
		// legacy alias: pre-v0.2.111 TOMLs used the misnomer
		if (const auto legacy = config["TrueScopesVR"]["disableScopeBlackout"]; legacy) {
			if (const auto value = legacy.as<bool>(); value) {
				*suppressScopeImods = value->get();
			}
		}
		LOAD(disableApproachFade);
		LOAD(devbenchEnabled);
		LOAD(devbenchPort);

#undef LOAD

		// [Scopes] -- every key is a node name or a model path, so it cannot be a
		// LOAD() list. Two accepted forms, because the common case deserves the
		// short one:
		//     HuntingScope = 1.267                              (aperture only)
		//     [Scopes.HuntingScope]                             (full entry)
		//     aperture = 1.267
		//     offsetY  = -1.8
		//
		// v0.2.101: prefer the MODEL PATH for anything modded, because a node
		// name may be shared with a vanilla optic and an entry keyed on it would
		// silently retune that one too:
		//     [Scopes."weapons/thermalscope/scopetherm1.nif"]
		//     aperture = 0.9
		// Keys are normalized (NormalizeScopeKey), so case, backslash-vs-slash, a
		// leading "Meshes\" and a "_1" first-person suffix are all accepted.
		{
			std::map<std::string, ScopeEntry, std::less<>> fresh;
			if (const auto* scopes = config["Scopes"].as_table()) {
				for (const auto& [key, value] : *scopes) {
					const std::string name{ key.str() };
					ScopeEntry        e;

					const auto readNumber = [](const toml::node& a_n, double& a_out) {
						if (const auto* f = a_n.as_floating_point()) {
							a_out = f->get();
							return true;
						}
						if (const auto* i = a_n.as_integer()) {
							a_out = static_cast<double>(i->get());
							return true;
						}
						return false;
					};

					if (const auto* sub = value.as_table()) {
						if (const auto* n = sub->get("aperture")) {
							readNumber(*n, e.aperture);
						}
						if (const auto* n = sub->get("offsetX")) {
							readNumber(*n, e.offsetX);
						}
						if (const auto* n = sub->get("offsetY")) {
							readNumber(*n, e.offsetY);
						}
						if (const auto* n = sub->get("offsetZ")) {
							readNumber(*n, e.offsetZ);
						}
						if (const auto* n = sub->get("aspect")) {
							readNumber(*n, e.aspect);
						}
					} else {
						readNumber(value, e.aperture);
					}

					// The composite's screen mask only crops VERTICALLY (no
					// shipped screen is taller than wide), so an aspect above 1
					// cannot do what its author hopes -- refuse it out loud
					// rather than crop the wrong axis silently.
					if (e.aspect != 0.0 && !(e.aspect >= 0.1 && e.aspect <= 1.0)) {
						logger::warn(FMT_STRING("[Scopes] {}: aspect {} ignored (expected 0.1 to 1.0)"), name, e.aspect);
						e.aspect = 0.0;
					}

					// A zero or absurd radius makes the lens vanish or swallow the
					// view, which reads as a broken render rather than a bad setting.
					// Say which entry is wrong instead of silently fitting to it.
					// An entry with offsets and no aperture is legitimate: it means
					// "the built-in radius is fine, the position is not".
					const bool haveOffset = !std::isnan(e.offsetX) || !std::isnan(e.offsetY) || !std::isnan(e.offsetZ);
					if (e.aperture != 0.0 && !(e.aperture > 0.01 && e.aperture < 64.0)) {
						logger::warn(FMT_STRING("[Scopes] {}: aperture {} ignored (expected 0.01 to 64)"), name, e.aperture);
						e.aperture = 0.0;
					}
					if (e.aperture > 0.0 || haveOffset || e.aspect > 0.0) {
						// Stored normalized so a lookup can normalize too and the
						// two always meet, whatever the user typed.
						const auto norm = NormalizeScopeKey(name);
						if (const auto [it, ok] = fresh.emplace(norm, e); !ok) {
							logger::warn(FMT_STRING("[Scopes] {}: duplicate of an earlier entry once "
							                        "normalized to '{}' — the first one wins"),
								name, norm);
						}
					} else {
						logger::warn(FMT_STRING("[Scopes] {}: nothing usable in this entry"), name);
					}
				}
			}
			const std::scoped_lock lock(scopeApertureLock);
			scopeEntries = std::move(fresh);
			if (!scopeEntries.empty()) {
				logger::info(FMT_STRING("[Scopes]: {} per-scope entr(ies) loaded"), scopeEntries.size());
			}
		}

		logger::info(
			FMT_STRING("settings: fillEnabled={} fillEveryNFrames={} forceAlwaysOn={} lensMode={} scopeFovDegrees={} sunEnabled={} suppressScopeImods={} disableApproachFade={}"),
			*fillEnabled, *fillEveryNFrames, *forceAlwaysOn, *lensMode, *scopeFovDegrees, *sunEnabled, *suppressScopeImods, *disableApproachFade);

		if (postLoadHook) {
			postLoadHook();
		}
	}
}
