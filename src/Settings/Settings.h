#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Settings
{
	// Every declared key, for load()'s unknown-key warning - a misspelled TOML
	// key used to be silently ignored. A free function, deliberately: a static
	// member would give each Setting<T> instantiation its own private list.
	[[nodiscard]] inline std::vector<std::string_view>& KnownKeys()
	{
		static std::vector<std::string_view> keys;
		return keys;
	}

	template <class T>
	class Setting
	{
	public:
		using value_type = T;

		Setting(
			std::string_view a_group,
			std::string_view a_key,
			value_type a_default) :
			_group(a_group),
			_key(a_key),
			_value(a_default)
		{
			Settings::KnownKeys().emplace_back(a_key);
		}

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

	// Master switch. false = the plugin installs nothing at all - no hooks, no
	// patches, no render, vanilla scope behavior untouched. Read once at load;
	// restart to change. The shipped ScopeMenu SWFs are loose files and still
	// apply (they only remove the hold-breath pill); delete them from
	// Data/Interface for a fully vanilla install.
	MAKE_SETTING(bSetting, "TrueScopesVR", enabled, true);
	// Master switch for the per-frame lens fill.
	MAKE_SETTING(bSetting, "TrueScopesVR", fillEnabled, true);
	// Fill cadence: 1 = every frame, 2 = every other frame, ... RT 0x62 persists
	// between frames, so low cadence just lowers the lens refresh rate. Cadence 1
	// has shown intermittent black lens frames in content-heavy scenes.
	MAKE_SETTING(iSetting, "TrueScopesVR", fillEveryNFrames, std::int64_t(1));
	// Eye-gate off hysteresis in ms. The vanilla gate flickers off in short
	// windows while aiming, and every off-edge plays the widget's fade-to-black
	// over the lens. Off-edges are only honored after the gate stayed off this
	// long. 0 = vanilla behavior.
	MAKE_SETTING(iSetting, "TrueScopesVR", scopeOffHoldMs, std::int64_t(1500));
	// One-euro damping on the scope render camera's orientation only - never
	// hands, weapon, or vanilla state. Magnification amplifies hand tremor
	// (~6x at 4x zoom); the speed-adaptive filter crushes at-rest tremor
	// without lagging deliberate pans.
	MAKE_SETTING(bSetting, "TrueScopesVR", camSmoothEnabled, false);
	MAKE_SETTING(fSetting, "TrueScopesVR", camSmoothMinCutoffHz, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", camSmoothBeta, 0.5);
	MAKE_SETTING(fSetting, "TrueScopesVR", camSmoothDCutoffHz, 1.0);
	MAKE_SETTING(iSetting, "TrueScopesVR", camSmoothResetMs, std::int64_t(250));
	MAKE_SETTING(fSetting, "TrueScopesVR", camSmoothSnapDegrees, 30.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", camSmoothMaxLagDegrees, 2.0);
	// Fill the lens once when the widget comes up (per equip), so the disc
	// never sits black before the first aim.
	MAKE_SETTING(bSetting, "TrueScopesVR", lensPrimeOnPresence, true);
	// Re-fill the frozen lens every N seconds while the widget is up (0 = off).
	// Each refresh costs one render-length frame hitch.
	MAKE_SETTING(fSetting, "TrueScopesVR", poseIdleRefreshSeconds, 0.0);
	// What fills the lens while scoped: 0 = off, 1 = copy of the main frame,
	// 2 = real mono world render from the scope camera (falls back to 1 on init
	// failure or a render fault).
	MAKE_SETTING(iSetting, "TrueScopesVR", lensMode, std::int64_t(2));
	// Scope render field of view in degrees. 0 = derive it (recommended): the
	// FOV is fixed by the scope's real magnification and the lens geometry,
	//        theta_render = 2*atan( (R / d) / M )
	// with R the lens disc's world radius, d the eye-to-lens distance, and M the
	// weapon's own zoomData fovMult, so a 4x scope is genuinely 4x per weapon
	// with nothing to hand-tune. Recomputed each render on purpose: a real scope
	// keeps its magnification as you move your head back and merely narrows what
	// you can see. Any positive value overrides the derivation; the derived
	// figure is logged every heartbeat either way.
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeFovDegrees, 0.0);
	// Frustum near/far planes for the scope camera. SetCameraFOV takes them as
	// (far, near) and the code passes them in that order; swapping them reverses
	// the projection (farthest-wins depth), and near==far produces a NaN
	// projection and an eternally black render. 15 = the engine's default near;
	// far generous for scoped sightlines.
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeNearClip, 15.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeFarClip, 250000.0);
	// Camera position in weapon-local space (applied to PrimaryWeaponScopeCamera's local
	// translate each render). Push the camera to the objective (front) end of the scope
	// tube so the render looks out of the glass, not at the tube interior. Y = forward
	// along the barrel in weapon space. Tune live: the TOML reloads on every scope-in.
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeCamOffsetX, 0.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeCamOffsetY, 55.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeCamOffsetZ, 0.0);
	// Cull the scope accumulation against the scope camera's own frustum.
	// BSShaderUtil::AccumulateScene does not cull with the BSCullingProcess we
	// hand it: it builds a fresh BSCullingGroup whose SetCamera (0x140638270)
	// derives the clip planes from the combined all-eye frustum at
	// *(NiFrustum**)(camera + 0x200) and the world transform at camera+0x70,
	// not from the per-eye array at camera+0x1a0 that SetCameraFOV fills.
	// SetCameraFOV only rebuilds the combined frustum when eyeCount > 1, and our
	// scope camera is mono, so the combined slot held stale extents and the
	// accumulation culled against a frustum unrelated to the scope. Fix: mirror
	// eye 0 into the combined slot right after SetCameraFOV, and set the culling
	// process's own frustum (NiCullingProcess::SetFrustum, the way the engine's
	// cull helper FUN_141d4dc50 does) for the paths that do read it.
	MAKE_SETTING(bSetting, "TrueScopesVR", cullToScopeFrustum, true);
	// Per-stage timing of our own render: D3D11 timestamp queries on the GPU
	// timeline plus QPC on the CPU, means reported by DevBench /state and the
	// heartbeat. Flip off then on to reset the accumulators for a clean
	// measurement window. Cost when on: 8 timestamp queries per render,
	// collected 2-3 renders later without blocking.
	MAKE_SETTING(bSetting, "TrueScopesVR", perfTimers, false);
	// Clamps the two light-loop counts the deferred resolve iterates
	// (ShadowSceneNode +0x1a8 shadowed, +0x1c0 queued) across our resolve call
	// only, restoring them immediately after. 0 = draw no light volumes at all,
	// -1 = untouched. The heartbeat keeps reporting the true counts (re-read
	// after the restore); `lightsClamp=` in the log marks a clamped run.
	MAKE_SETTING(iSetting, "TrueScopesVR", perfLightsMax, std::int64_t(-1));

	// Scale on the sun's light color for the scope render only (1.0 = engine value).
	// Doubles as a diagnostic: if lowering this does not dim the lens, the overbright
	// artifact is not coming through the light color path.
	MAKE_SETTING(fSetting, "TrueScopesVR", sunBrightnessScale, 1.0);
	// Lens brightness / ambient fill: a flat fog-coloured fill added to the light
	// accumulation before the sun and local lights (the accum pre-clear, scaled
	// from the fog RGB). Raising it lifts the shadows and washes the image out;
	// lowering it deepens contrast. Deliberately a taste / display-calibration
	// knob - headsets and preferences differ. 0.04 matches the unscoped world;
	// 0.30 is noticeably lifted, 1.0 blown out. accumClearAlpha sets the clear's
	// alpha channel independently.
	// Coupled to sunExecEnabled: this fill substitutes for the sun, so a low
	// value with the sun exec off is a near-black lens - move the two together.
	MAKE_SETTING(fSetting, "TrueScopesVR", accumClearScale, 0.04);
	MAKE_SETTING(fSetting, "TrueScopesVR", accumClearAlpha, 1.0);
	// Draw the sky into the lens: accumulate the sky roots if the world
	// accumulation didn't already produce group-0xC passes, then draw group 0xC
	// into 0x61 after the composite (vanilla order: depth-tested, fills only far
	// pixels).
	MAKE_SETTING(bSetting, "TrueScopesVR", skyEnabled, true);
	// Which sky roots to accumulate: 1 = dome only (Sky+0x8), 2 = sun/cloud only,
	// 3 = both. Bisects a faulting root live (the sun/cloud root carries the
	// occlusion-query glare geometry — prime suspect if the sky draw faults).
	MAKE_SETTING(iSetting, "TrueScopesVR", skyRootMask, std::int64_t(3));
	// Unbind the depth-stencil around the lens delivery. The render otherwise
	// restores DS logical 1 (vanilla's post-resolve pattern), which rtm+0x15fc
	// maps to physical 2 = the main VR eye's depth-stencil; the ImageSpace
	// delivery quad then draws stencil-masked to the headset's hidden-area mesh
	// and clips an arc off 0x62.
	MAKE_SETTING(bSetting, "TrueScopesVR", deliveryUnbindDS, true);
	// own delivery pass
	// After the engine tonemap writes the picture into 0x62, run our own
	// fullscreen pixel shader over it: composite the engine's Scaleform reticle
	// (the vanilla reticle quad sits under our picture disc) and apply the glass
	// look. Everything below is live-tunable via DevBench.
	// Controller verdict chords (grip+A yes / grip+B no / grip+trigger skip)
	// for guided test passes; read passively off OpenVR, haptic ack.
	MAKE_SETTING(bSetting, "TrueScopesVR", verdictInputEnabled, false);
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
	// In-lens recreations of the suppressed zoom imods. Strengths are
	// 0..1 master faders over looks derived from the IMAD records themselves:
	// NV: tint (0.31,0.82,0.22)@0.69 + brightness x1.1+0.4 + gain (zd_ScopeNightVision)
	// Recon: desaturate to ~0.3, warm tint (1,0.88,0.84)@0.39, contrast x0.2+0.8
	// (zd_ScopeTargetingRecon). Screen-type scopes also drop the radial vignette
	// for a square soft edge.
	MAKE_SETTING(fSetting, "TrueScopesVR", nvEffectStrength, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", nvGain, 1.6);
	MAKE_SETTING(fSetting, "TrueScopesVR", reconEffectStrength, 1.0);
	// glass effects
	// One switch to make every glass scope act like the recon screen scopes: a
	// static picture in the tube - no eyebox, no parallax, no sheen, no rim
	// parallax, no edge blur, no chromatic fringe. The static vignette and rim
	// ring stay, and NV/recon looks still apply. Equivalent to zeroing those
	// knobs individually; they are ignored while this is on.
	MAKE_SETTING(bSetting, "TrueScopesVR", glassFlatMode, false);
	// All three apply to optical tubes only - screen-type optics (recon/thermal)
	// are displays and skip them.
	//
	// Eye-box / exit pupil: the picture clips and dims as the head moves off the
	// tube axis, computed per frame from the real head-to-scope pose (camera
	// root vs ScopeParent, fed from the HMD). On axis it is a no-op. Strength 0
	// disables (and skips the pose math); 1.0 = the shadow goes fully black,
	// like a real scope shadow. Gain scales how strongly head movement moves
	// the shadow - raise it for a twitchier, less forgiving scope.
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxStrength, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxGain, 0.9);
	// Interpupillary distance in game units (1 unit ~ 1.43 cm; 64 mm ~ 4.5).
	// The camera root is the HMD centre, so the aiming eye sits half of this
	// off it; the eye-box tests both eyes and follows whichever is closer to
	// the tube axis.
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxIpdUnits, 4.5);
	// A real eyebox is distance-dependent: widest at the scope's eye relief,
	// collapsing steeply as the eye moves closer in, forgiving as it backs off
	// (checked against a real scope). The effective gain is
	//     eyeBoxGain * clamp((eyeBoxReliefUnits / eyeRelief)^power, 0.35, 3)
	// - unchanged at the relief distance, tighter closer, looser farther. A
	// fixed gain punished normal eye relief as if the eye were jammed on the
	// ocular, which the first field test called near-unusable. relief 0 = the
	// old fixed gain.
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxReliefUnits, 12.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxDistancePower, 1.6);
	// Edge blur: field-curvature softness from edgeBlurStart (disc radius 0..1)
	// out to the rim. 0 disables.
	MAKE_SETTING(fSetting, "TrueScopesVR", edgeBlurStrength, 0.35);
	MAKE_SETTING(fSetting, "TrueScopesVR", edgeBlurStart, 0.7);
	// Chromatic fringe at the rim (red/blue focal split), 0..1. 0 disables.
	MAKE_SETTING(fSetting, "TrueScopesVR", caStrength, 0.35);
	// glass suite - all live-tunable; every effect is an exact no-op at
	// strength 0.
	// Housing rim shadow: a hard near-black annulus at the disc edge that reads
	// as the ocular tube wall (distinct from the soft vignette). Optical only.
	MAKE_SETTING(fSetting, "TrueScopesVR", rimShadowStrength, 0.9);
	MAKE_SETTING(fSetting, "TrueScopesVR", rimShadowStart, 0.86);
	MAKE_SETTING(fSetting, "TrueScopesVR", rimShadowEnd, 1.0);
	// Center drop in disc units — the shadow intrudes deeper at the top like a
	// real tube interior lit from above. Rides the disc, so it cants with the gun.
	MAKE_SETTING(fSetting, "TrueScopesVR", rimShadowTopBias, 0.05);
	// Shadow center shifts opposite the eye by this factor of the raw eye offset
	// - the rim behaves as near geometry against the far image.
	MAKE_SETTING(fSetting, "TrueScopesVR", rimShadowParallax, 0.2);
	// Glass sheen: additive view-dependent glint + fresnel rim lift + smudge.
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenStrength, 0.06);
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenWidth, 0.55);
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenTravel, 0.9);
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenFresnel, 0.04);
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenSmudge, 0.35);
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenSmudgeScale, 9.0);
	// Dark-adaptive glass: multiply the sheen term by up to (1+this) as the
	// composed picture darkens. 0 = off. A black lens reads as a dead screen;
	// real glass keeps surface life.
	MAKE_SETTING(fSetting, "TrueScopesVR", sheenDarkBoost, 3.0);
	// How much the reticle follows the eye-box clip (on a real scope the
	// reticle stays faintly visible inside the blacked-out eye box). 1 = clips
	// with the picture, 0 = always visible.
	MAKE_SETTING(fSetting, "TrueScopesVR", reticleEyeBoxFollow, 0.35);
	// The eye-box clip bottoms out at this flat scatter level instead of pure
	// black (scene-independent: a constant, never the world). This is what
	// makes the dark reticle visible in the black at all.
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxResidual, 0.0);
	// The residual adapts to scene brightness (avg picture luminance x this
	// scale, saturated): full glow in daylight, fading toward true black at
	// night so the eye-box never reads as a lit screen in the dark. 0 =
	// constant residual.
	MAKE_SETTING(fSetting, "TrueScopesVR", eyeBoxResidualAdapt, 20.0);
	// Draw deferred-decal group 5 after the opaque G-buffer groups in our
	// resolve (the engine's own order paints walls over placed decals in a
	// resolve-only render). Off = vanilla resolve order.
	MAKE_SETTING(bSetting, "TrueScopesVR", decalGroup5Reorder, true);
	// Run the engine's deferred-decal stage (bullet holes) inside our render.
	// A mid-stage fault must not orphan the SSN decal spin lock (that froze the
	// game on the next shot) - the fault path releases it via __finally, and
	// the faulting step is logged.
	MAKE_SETTING(bSetting, "TrueScopesVR", decalStageEnabled, true);
	// Drop accumulator group 0x17 (sun glare) from our render: stale passes
	// drawn by the resolve tail are a fault exposure. Hardening.
	MAKE_SETTING(bSetting, "TrueScopesVR", dropSunGlareGroup, true);
	// Parallax depth: picture UV shifts against the eye so the image plane
	// reads D game units behind the lens (1 unit ~ 1.43 cm). 0 = off. The image
	// through a distance-focused scope sits near optical infinity
	// (D/(L+D) -> 1); 20 gives ~0.83 at rifle relief.
	MAKE_SETTING(fSetting, "TrueScopesVR", parallaxDepthUnits, 20.0);
	// Cap on the UV shift. If a dark smear band shows at the shifted edge, this
	// is the first knob to lower.
	MAKE_SETTING(fSetting, "TrueScopesVR", parallaxMaxShift, 0.15);
	MAKE_SETTING(fSetting, "TrueScopesVR", parallaxSmoothing, 0.3);
	MAKE_SETTING(fSetting, "TrueScopesVR", parallaxMinEyeRelief, 4.0);
	// How much the reticle rides the parallax shift. 1 = moves 1:1 with the
	// image (a scope parallax-adjusted at distance has coincident planes),
	// 0 = rim-anchored for anyone who prefers it.
	MAKE_SETTING(fSetting, "TrueScopesVR", reticleParallaxFraction, 1.0);
	// NV phosphor scanlines, 0..1.
	MAKE_SETTING(fSetting, "TrueScopesVR", nvScanlines, 0.8);
	// pose-based activation
	// Replace the vanilla eye-proximity gate's verdict with our own pose test
	// (PoseGate.h has the mechanism write-up). The vanilla gate's 38-unit
	// distance cap makes pistol scopes impossible to activate at arm's length;
	// ours tests eye-to-ocular distance, the eye's lateral offset from the
	// actual tube axis (distance-invariant - wide angle up close, narrow at
	// arm's length, like real optics), and a head look cone. Each has an
	// enter/exit pair for spatial hysteresis.
	MAKE_SETTING(bSetting, "TrueScopesVR", poseGateEnabled, true);
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
	// The lateral band scales with eye distance when adapt > 0: enter/exit are
	// multiplied by max(1, 1 + adapt * (dist / poseLateralRefDist - 1)), i.e. a
	// constant angular cone beyond the reference distance instead of a constant
	// offset. The fixed band read at rifle range was the field tester's mid-aim
	// dropout at 20+ units. Never tightens below the base values. 0 = fixed.
	MAKE_SETTING(fSetting, "TrueScopesVR", poseLateralDistanceAdapt, 1.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", poseLateralRefDist, 13.0);
	// Skip the look-cone test while the eye is on the tube (lateral below this,
	// game units) - being on the axis is looking through the scope. At 5-12
	// units from the ocular the 35-45 deg band is one or two centimetres of
	// head translation, below VR jitter; it was the dominant flapping source in
	// the field log (six sub-second cycles pinned at the exit angle). 0 = always
	// test.
	MAKE_SETTING(fSetting, "TrueScopesVR", poseLookWaiveLateral, 4.0);
	// Minimum continuous time the enter conditions must hold before the gate
	// re-arms, in ms. Stops sub-second live/frozen cycling on the enter edge;
	// the exit edge stays immediate.
	MAKE_SETTING(iSetting, "TrueScopesVR", poseReArmDwellMs, std::int64_t(250));
	// Widget presence: true = the scope widget meshes stay visible the whole
	// time the weapon is drawn (lens frozen while the pose is inactive — RT
	// 0x62 persists, so freeze = don't fill; no pop-in). This is plugin-owned
	// node visibility only — the verdict fed to vanilla is always the pose, so
	// sighted/ScopeMenu/Pip-Boy/FRIK behave identically with this on or off
	// (routing the verdict instead keeps the player sighted, ScopeMenu open and
	// the Pip-Boy blocked whenever the weapon is drawn).
	// false = the widget appears/disappears with the pose, vanilla-style.
	MAKE_SETTING(bSetting, "TrueScopesVR", poseWidgetAlways, true);
	// One-shot dim applied to the frozen lens picture on the live→frozen edge,
	// so a stale picture does not read as live. 0..1 multiplier; 1.0 = no dim.
	MAKE_SETTING(fSetting, "TrueScopesVR", poseFrozenDim, 0.55);
	// After the dim, the frozen picture keeps fading to near-black over this
	// many seconds. The eyebox used to be the only thing taking the lens dark on
	// eye-exit, and it is baked into the last live frames — with eyeBoxStrength
	// at 0 a frozen lens stayed bright and read as live. 0 = no fade. Lens
	// primes and idle refreshes stay at the plain dim (they are meant to be
	// seen); only a live→frozen edge arms the fade.
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeFrozenFadeSeconds, 1.5);
	// Hide the widget model's own housing meshes (scope_Hunting:0 /
	// scope_recon:0 in world_scope.nif - the pale speckled ring Bethesda drew
	// around the picture). The real weapon's scope provides the housing; the
	// widget only needs its render surfaces. Re-hidden every eligible frame
	// because the engine re-shows them on scope-in edges.
	MAKE_SETTING(bSetting, "TrueScopesVR", hideWidgetHousing, true);
	// widget fit
	// Fit the vanilla VR scope widget to the real scope's lens instead of leaving it
	// at Bethesda's oversized floating disc. The widget mesh hangs off the engine's
	// "ScopeParent" node (player+0x7d0) and its render surface `render_circle:0` is a
	// disc of radius 7.852 centred on that node's origin, so
	//     ScopeParent.scale = widgetApertureRadius / 7.852
	// Shipped scopes measure an ocular radius of 0.76–4.56 (scale 0.097–0.581), i.e.
	// vanilla is 2–6x oversized.
	MAKE_SETTING(bSetting, "TrueScopesVR", widgetFitEnabled, true);
	// Ocular aperture radius, in mesh units, for scopes the plugin does not
	// recognise — modded optics, or a vanilla one whose node name is missing from
	// the built-in table. Default is the hunting rifle's glass shape (measured
	// 1.267). Recognised scopes use their own radius instead; see perScopeAperture.
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetApertureRadius, 1.267);
	// Under-aperture sizing: multiplies the derived aperture so the picture
	// disc sits slightly inside the real housing hole and no bright pixel ever
	// touches the seam. 1.0 = exact fit. Clamped to [0.8, 1.1] at use.
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetApertureScale, 0.97);
	// Look the aperture up per scope from the node names in the equipped
	// weapon's 3D, instead of using one number for every optic. The shipped table
	// covers the 26 vanilla scopes; [Scopes] in the TOML overrides and extends it.
	// Turn this off to go back to the single widgetApertureRadius above.
	MAKE_SETTING(bSetting, "TrueScopesVR", perScopeAperture, true);
	// Staleness guard: auto-placement refuses a target that is farther than
	// this many units from the eye. Right after a save load the weapon 3D walks
	// fine but its world transforms still hold the pre-placement rig pose
	// (~350 units out) until the first skeleton update; an equipped scope can
	// never actually be beyond arm's length + weapon (~60 units). The refusal
	// feeds PresenceFit's bounded retry, which converges as soon as the
	// transforms are real.
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetPlaceMaxEyeDist, 100.0);
	// Local-space nudge applied on top of the engine's own ScopeParent translation
	// (captured as a baseline, never accumulated). For sliding the shrunken widget
	// onto the real lens once the scale is right.
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetOffsetX, 0.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetOffsetY, 0.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", widgetOffsetZ, 0.0);
	// Put the disc on the optic's ocular face automatically instead of relying on
	// a hand-tuned offset per scope. Aims at the census-measured face where the
	// scope has a row, and falls back to the rear of the optic's world bounding
	// sphere along the eye axis where it does not — the two differ by ~1-2 units,
	// so an unmeasured or modded optic lands close but not exactly.
	// Turning it off returns to widgetOffset* / [Scopes] offsets, the escape
	// hatch if some optic ends up placed badly. The candidate is computed and
	// reported either way (log line "WIDGET AUTO-PLACE", DevBench /scope
	// "placement"), so it can always be compared against a hand-tuned value.
	MAKE_SETTING(bSetting, "TrueScopesVR", widgetAutoPlace, true);
	// Skip only the sun's fullscreen BSDFLightDir exec, keeping the accum
	// clear/binds, camera state and pre-resolve G-buffer rebind that share its
	// block (dropping those faults the delivery — the ImageSpace copy needs the
	// camera state — which is why no wider off switch exists). The exec must
	// run after the G-buffer exists and with the accum MRT bound, or it writes
	// NaN into the accumulation (displays black, probes lit). With that
	// ordering it genuinely sun-lights the lens; turning it off visibly
	// darkens it. Coupled to accumClearScale — see the note there.
	MAKE_SETTING(bSetting, "TrueScopesVR", sunExecEnabled, true);
	// Re-arm the renderer on scope-in after a fault (the fault latch is otherwise
	// session-permanent). Lets a faulting config be bisected via TOML edits without
	// restarting the game. The faulting step is logged each time.
	MAKE_SETTING(bSetting, "TrueScopesVR", retryAfterFault, true);
	// DevBench (dev tooling): localhost query server that reads live state,
	// flips the settings above, and tails the log without a rebuild. See
	// src/DevBench/DevBench.h.

	// Suppress the per-zoom imods (zd_ScopeNightVision 0x94636,
	// zd_ScopeTargetingRecon 0x2041b6 - standard scopes carry none). They are
	// fullscreen imagespace modifiers authored for the flat game, where scoping
	// owned the whole screen; in VR they tint the world, not the scope. The
	// looks are recreated inside the lens composite (nv*/recon* knobs), keyed
	// off the zoom's imod identity. Old TOML key disableScopeBlackout still loads.
	MAKE_SETTING(bSetting, "TrueScopesVR", suppressScopeImods, true);
	// Hide the vanilla floating "GRAB Hold Breath" pill next to the scope. The
	// shipped ScopeMenu SWFs remove it (zero-scaled placement); this also nops
	// the ScopeMenu ctor's SetUpButtonBar call. Load-time patch: restart to revert.
	MAKE_SETTING(bSetting, "TrueScopesVR", hideHoldBreathHint, true);
	// Also suppress the eye-approach dimming fade (cosmetic; vanilla feel if left on).
	MAKE_SETTING(bSetting, "TrueScopesVR", disableApproachFade, true);

#undef MAKE_SETTING

	// Installed by DevBench so live overrides survive the scope-in TOML reload.
	// Without it, every /config/set would be silently reverted the next time the
	// player raised the scope.
	inline std::function<void()> postLoadHook;

	// [Scopes] per-scope aperture overrides
	// A free-form table, `NodeName = radius`, keyed by a node name from the
	// equipped weapon's 3D. Not a fixed list of settings, because the whole
	// point is that a user can add an entry for a scope the plugin has never seen
	// -- the log prints the node names whenever a lookup misses, so adding one is
	// copy-paste rather than reverse engineering.
	//
	// Written on the game thread at scope-in, read on the render thread during a
	// probe, hence the lock. Contention is nil (both are rare events); correctness
	// is not, since the map reallocates on reload.
	// An entry carries position as well as size -- a correctly sized disc in the
	// wrong place still misses the lens, and one offset does not fit every optic.
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
		// Height/width of a rectangular screen optic, 0 = not specified
		// (circular optics and square screens are 1.0 in the built-in table).
		// When < 1, the aperture is the screen's half-width and the composite
		// masks the picture's vertical overshoot, so a wide display
		// (MGScopeThermal, 2.55 x 1.37) shows a wide picture instead of a
		// circle spilling above and below it.
		double aspect = 0.0;
	};

	// A [Scopes] key may be a model path as well as a node name, because a node
	// name cannot address a modded optic that copied a vanilla mesh's root name
	// without also addressing the vanilla one. Paths mean the key has to
	// survive being written by a human, so it is normalized on both sides --
	// once when the TOML is loaded, once when a lookup is made:
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
	// These rules must match mesh_key() in tools/scope-census.py, which produces
	// the built-in table's keys. A divergence would not throw; it would simply
	// never match, and a table that never matches looks exactly like a table
	// whose numbers happen to equal the fallback.
	// One implementation, in a buffer form, because the caller that matters most
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

	// Serializes load() itself: the game thread reloads on every scope-in and the
	// devbench listener thread reloads on request, and the ~100 setting writes
	// (one a std::string assignment) are not individually thread-safe.
	inline std::mutex                                     loadLock;
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
		const std::scoped_lock lock(loadLock);
		toml::table config;
		try {
			config = toml::parse_file("Data/F4SE/Plugins/TrueScopesVR.toml"sv);
		} catch (const std::exception& e) {
			logger::warn(FMT_STRING("TrueScopesVR.toml not loaded ({}), using defaults"), e.what());
			return;
		}

// value<T>() instead of as<T>() so an integer literal fills a float setting
// (nvGain = 2 used to be silently ignored), and a genuinely wrong type warns
// instead of no-oping.
#define LOAD(a_setting)                                                               \
	if (const auto tweak = config[a_setting.group()][a_setting.key()]; tweak) {     \
		if (const auto value = tweak.value<decltype(a_setting)::value_type>(); value) {  \
			*a_setting = *value;                                                          \
		} else {                                                                      \
			logger::warn(                                                                 \
				FMT_STRING("TrueScopesVR.toml: '{}' has the wrong type - keeping default"),  \
				a_setting.key());                                                             \
		}                                                                             \
	}

		LOAD(enabled);
		LOAD(fillEnabled);
		LOAD(fillEveryNFrames);
		LOAD(glassFlatMode);
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
		LOAD(lensMode);
		// Diagnostic lens modes (3-8: G-buffer/accum taps, no-delivery) are
		// removed; only 0 = off, 1 = frame copy, 2 = real render ship. Clamp so a
		// stale TOML cannot select a removed mode.
		if (*lensMode < 0 || *lensMode > 2) {
			*lensMode = 2;
		}
		LOAD(scopeFovDegrees);
		LOAD(scopeNearClip);
		LOAD(scopeFarClip);
		// near==far builds a NaN projection = eternally black lens (documented
		// at the setting). Refuse the pair rather than render nothing.
		if (!(*scopeFarClip > *scopeNearClip) || *scopeNearClip <= 0.0) {
			logger::warn(
				FMT_STRING("TrueScopesVR.toml: scopeNearClip {} / scopeFarClip {} invalid - using 15/250000"),
				*scopeNearClip, *scopeFarClip);
			*scopeNearClip = 15.0;
			*scopeFarClip = 250000.0;
		}
		LOAD(scopeCamOffsetX);
		LOAD(scopeCamOffsetY);
		LOAD(scopeCamOffsetZ);
		LOAD(cullToScopeFrustum);
		LOAD(perfTimers);
		LOAD(perfLightsMax);
		LOAD(sunBrightnessScale);
		LOAD(accumClearScale);
		LOAD(accumClearAlpha);
		LOAD(skyEnabled);
		LOAD(skyRootMask);
		LOAD(deliveryUnbindDS);
		LOAD(verdictInputEnabled);
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
		LOAD(eyeBoxReliefUnits);
		LOAD(eyeBoxDistancePower);
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
		LOAD(poseLateralDistanceAdapt);
		LOAD(poseLateralRefDist);
		LOAD(poseLookWaiveLateral);
		LOAD(poseReArmDwellMs);
		LOAD(poseWidgetAlways);
		LOAD(poseFrozenDim);
		LOAD(scopeFrozenFadeSeconds);
		LOAD(hideWidgetHousing);
		LOAD(widgetFitEnabled);
		LOAD(widgetApertureRadius);
		LOAD(widgetApertureScale);
		LOAD(perScopeAperture);
		LOAD(widgetOffsetX);
		LOAD(widgetOffsetY);
		LOAD(widgetOffsetZ);
		LOAD(widgetAutoPlace);
		LOAD(widgetPlaceMaxEyeDist);
		LOAD(retryAfterFault);
		LOAD(sunExecEnabled);
		LOAD(suppressScopeImods);
		LOAD(hideHoldBreathHint);
		// Name every unrecognized key once, so a typo is a log line instead of a
		// silent no-op. disableScopeBlackout is a known legacy alias.
		if (const auto* tbl = config["TrueScopesVR"].as_table(); tbl) {
			for (const auto& [key, node] : *tbl) {
				const std::string_view k = key.str();
				if (k == "disableScopeBlackout"sv) {
					continue;
				}
				const auto& known = KnownKeys();
				if (std::find(known.begin(), known.end(), k) == known.end()) {
					logger::warn(FMT_STRING("TrueScopesVR.toml: unknown key '{}' ignored"), k);
				}
			}
		}

		if (const auto legacy = config["TrueScopesVR"]["disableScopeBlackout"]; legacy) {
			if (const auto value = legacy.as<bool>(); value) {
				*suppressScopeImods = value->get();
			}
		}
		LOAD(disableApproachFade);

#undef LOAD

		// [Scopes] -- every key is a node name or a model path, so it cannot be a
		// LOAD() list. Two accepted forms, because the common case deserves the
		// short one:
		//     HuntingScope = 1.267                              (aperture only)
		//     [Scopes.HuntingScope]                             (full entry)
		//     aperture = 1.267
		//     offsetY  = -1.8
		//
		// Prefer the model path for anything modded, because a node name may be
		// shared with a vanilla optic and an entry keyed on it would
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

					// The composite's screen mask only crops vertically (no
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
			FMT_STRING("settings: fillEnabled={} fillEveryNFrames={} lensMode={} scopeFovDegrees={} suppressScopeImods={} disableApproachFade={}"),
			*fillEnabled, *fillEveryNFrames, *lensMode, *scopeFovDegrees, *suppressScopeImods, *disableApproachFade);

		if (postLoadHook) {
			postLoadHook();
		}
	}
}
