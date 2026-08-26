#include "TrueScopes/PoseGate.h"

#include "Settings/Settings.h"
#include "TrueScopes/Hooks.h"

namespace TrueScopes::PoseGate
{
	namespace
	{
		// Pose sources - the same ground truth LensComposite's EyeLateral uses:
		// PlayerCamera singleton [base+0x5930608], camera root at +0x20;
		// NiAVObject world rotate +0x70 (NiMatrix3 = 3 rows of NiPoint4,
		// transposed on read: R[r][c] = m[c*4+r]), world translate +0xa0.
		constexpr std::uintptr_t kPlayerCameraGlobal = 0x5930608;
		constexpr std::uintptr_t kCameraRoot = 0x20;
		constexpr std::uintptr_t kWorldRotate = 0x70;
		constexpr std::uintptr_t kWorldTranslate = 0xa0;
		// The engine's "ScopeParent" NiNode (TS_SetupScopeRig hangs the widget off
		// it; created on player-3D load, so it exists before any scope-in).
		constexpr std::uintptr_t kScopeParentInPlayer = 0x7d0;

		std::atomic_bool           g_fillLive{ true };
		std::atomic_bool           g_owned{ false };
		std::atomic<float>         g_dist{ 0.0f };
		std::atomic<float>         g_lateral{ 0.0f };
		std::atomic<float>         g_lookDeg{ 0.0f };
		std::atomic<std::uint32_t> g_evals{ 0 };
		std::atomic<std::uint64_t> g_evalFrame{ 0 };  // game frame of the last pose eval (freshness)
		std::atomic<std::uint64_t> g_siteFrame{ 0 };  // game frame of the last verdict-site call,
		                                              // updated even when the pose gate is disabled -
		                                              // "is the site alive" (weapon drawn, eligible)
		// Hysteresis memory. Game thread only (the verdict site is per-frame from
		// Main::OnIdle), so no atomicity needed for the read-modify-write.
		bool g_liveState = false;

		struct Sample
		{
			float dist;     // |eye → ocular|
			float lateral;  // eye's perpendicular distance from the tube axis line
			float lookDeg;  // HMD forward vs direction to ocular
			bool  valid;
		};

		[[nodiscard]] Sample Compute(std::uintptr_t a_player) noexcept
		{
			Sample s{};
			if (!a_player) {
				return s;
			}
			const auto scopeParent = *reinterpret_cast<std::uintptr_t*>(a_player + kScopeParentInPlayer);
			if (!scopeParent) {
				return s;
			}
			const auto playerCam = *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + kPlayerCameraGlobal);
			if (!playerCam) {
				return s;
			}
			const auto camRoot = *reinterpret_cast<std::uintptr_t*>(playerCam + kCameraRoot);
			if (!camRoot) {
				return s;
			}

			// Tube axis: world direction of ScopeParent's local Y (down-range).
			// With the transposed-on-read convention, world_dir(local Y)[r] =
			// R[r][1] = m[1*4 + r] — raw floats 4..6. (This is also exactly the
			// +0x80/+0x84/+0x88 triple the vanilla gate dots against.)
			const auto* rot = reinterpret_cast<const float*>(scopeParent + kWorldRotate);
			float       axis[3] = { rot[4], rot[5], rot[6] };
			const float axisLen = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
			if (!(axisLen > 1.0e-4f) || !std::isfinite(axisLen)) {
				return s;
			}
			for (auto& a : axis) {
				a /= axisLen;
			}

			// Ocular point = the ScopeParent origin (the vanilla widget anchor,
			// engine-owned, re-read through the player every frame - can never
			// dangle). Deliberately not ScopeIdent::OcularFaceWorld here: that
			// walks node pointers cached at probe time, which dangle for a frame
			// or more after every weapon/mod swap, and this is a per-frame
			// game-thread path with no SEH bracket (the render-thread consumers
			// are all inside RenderGuarded's __try). The two points differ by at
			// most a couple of units on the same eyepiece - noise against the
			// lateral and distance thresholds.
			float       D[3];
			const auto* t = reinterpret_cast<const float*>(scopeParent + kWorldTranslate);
			D[0] = t[0];
			D[1] = t[1];
			D[2] = t[2];

			const auto* camPos = reinterpret_cast<const float*>(camRoot + kWorldTranslate);
			const auto* camRot = reinterpret_cast<const float*>(camRoot + kWorldRotate);
			// Head axes, same convention: world X = m[0..2], world forward (local
			// Y) = m[4..6].
			const float headX[3] = { camRot[0], camRot[1], camRot[2] };
			const float headFwd[3] = { camRot[4], camRot[5], camRot[6] };

			// Both real eyes (camera root is the HMD centre); keep whichever sits
			// closer to the tube axis - the aiming eye, no dominance setting.
			const float halfIpd = 0.5f * static_cast<float>(*Settings::eyeBoxIpdUnits);
			float       bestLat = -1.0f, bestDist = 0.0f;
			for (const float side : { -1.0f, 1.0f }) {
				const float e[3] = { camPos[0] + side * halfIpd * headX[0],
					                 camPos[1] + side * halfIpd * headX[1],
					                 camPos[2] + side * halfIpd * headX[2] };
				const float v[3] = { D[0] - e[0], D[1] - e[1], D[2] - e[2] };
				const float axial = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
				if (!(axial > 0.0f)) {
					// The eye must be behind the ocular, looking down-range - an
					// eye in front of the eyepiece is never "looking through".
					continue;
				}
				const float d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
				const float lat2 = std::max<float>(0.0f, d2 - axial * axial);
				const float lat = std::sqrt(lat2);
				if (!std::isfinite(lat)) {
					continue;
				}
				if (bestLat < 0.0f || lat < bestLat) {
					bestLat = lat;
					bestDist = std::sqrt(d2);
				}
			}
			if (bestLat < 0.0f) {
				// Pose sources are fine - the answer is simply "not looking
				// through" (both eyes in front of, or level with, the ocular).
				s.valid = true;
				s.dist = 1.0e9f;
				s.lateral = 1.0e9f;
				s.lookDeg = 180.0f;
				return s;
			}

			// Look cone: is the head actually oriented at the scope. Measured from
			// the HMD centre — the ± half-IPD parallax is well inside the cone at
			// any distance that passes the other two tests.
			const float dc[3] = { D[0] - camPos[0], D[1] - camPos[1], D[2] - camPos[2] };
			const float dcLen = std::sqrt(dc[0] * dc[0] + dc[1] * dc[1] + dc[2] * dc[2]);
			float       lookDeg = 0.0f;
			if (dcLen > 1.0e-4f) {
				float c = (dc[0] * headFwd[0] + dc[1] * headFwd[1] + dc[2] * headFwd[2]) / dcLen;
				c = std::clamp(c, -1.0f, 1.0f);
				lookDeg = std::acos(c) * 57.29578f;
			}

			s.dist = bestDist;
			s.lateral = bestLat;
			s.lookDeg = lookDeg;
			s.valid = std::isfinite(s.dist) && std::isfinite(s.lateral) && std::isfinite(s.lookDeg);
			return s;
		}
	}

	bool OnGateVerdict(std::uintptr_t a_player, bool a_vanillaVerdict)
	{
		g_siteFrame.store(Hooks::FrameCount(), std::memory_order_relaxed);
		if (!*Settings::poseGateEnabled) {
			g_owned.store(false, std::memory_order_relaxed);
			g_fillLive.store(true, std::memory_order_relaxed);
			return a_vanillaVerdict;
		}
		const auto s = Compute(a_player);
		g_evals.fetch_add(1, std::memory_order_relaxed);
		g_evalFrame.store(Hooks::FrameCount(), std::memory_order_relaxed);
		if (!s.valid) {
			// Pose sources missing (no rig / no camera yet): behave exactly like
			// the un-hooked game rather than guessing.
			g_owned.store(false, std::memory_order_relaxed);
			g_fillLive.store(true, std::memory_order_relaxed);
			return a_vanillaVerdict;
		}

		const bool was = g_liveState;
		// Effective exit = max(exit, enter): the hysteresis invariant. Editing a
		// pair one knob at a time over DevBench can momentarily invert it
		// (exit < current < enter), which oscillates live/frozen every eval.
		// Clamped, any edit order produces at most one clean transition.
		const auto band = [was](double a_enter, double a_exit) {
			return static_cast<float>(was ? std::max<double>(a_exit, a_enter) : a_enter);
		};
		const float dMax = band(*Settings::poseMaxDistance, *Settings::poseExitDistance);
		const float latMax = band(*Settings::poseMaxLateral, *Settings::poseExitLateral);
		const float lookMax = band(*Settings::poseLookConeDegrees, *Settings::poseLookConeExitDegrees);
		const bool  live = s.dist < dMax && s.lateral < latMax && s.lookDeg < lookMax;
		if (live != was) {
			logger::info(
				FMT_STRING("pose gate live -> {} (dist={:.1f} lat={:.2f} look={:.1f}deg)"),
				live, s.dist, s.lateral, s.lookDeg);
		}
		g_liveState = live;

		g_dist.store(s.dist, std::memory_order_relaxed);
		g_lateral.store(s.lateral, std::memory_order_relaxed);
		g_lookDeg.store(s.lookDeg, std::memory_order_relaxed);
		g_owned.store(true, std::memory_order_relaxed);
		g_fillLive.store(live, std::memory_order_relaxed);

		// The verdict fed to vanilla is always the pose. Feeding a perpetual
		// "true" keeps the player sighted the whole time the weapon is drawn -
		// the enable switch's ActorState call drives the sighted state, sighted
		// opens ScopeMenu, and FRIK reacts to ScopeMenu by collapsing the body
		// root and blocking all Pip-Boy interaction. Widget permanence is
		// plugin-owned node visibility in Hooks.cpp (WidgetPresence), which
		// vanilla state never sees.
		return live;
	}

	bool VerdictStale(std::uint64_t a_maxFrames)
	{
		if (!g_owned.load(std::memory_order_relaxed)) {
			return false;
		}
		const auto last = g_evalFrame.load(std::memory_order_relaxed);
		const auto now = Hooks::FrameCount();
		return now > last && now - last > a_maxFrames;
	}

	bool SiteStale(std::uint64_t a_maxFrames)
	{
		// Unlike VerdictStale this needs no pose ownership: it answers "has the
		// verdict site run recently" (weapon drawn + eligible), which is what
		// plugin-owned widget presence keys its hide on - it must work the same
		// with the pose gate disabled.
		const auto last = g_siteFrame.load(std::memory_order_relaxed);
		const auto now = Hooks::FrameCount();
		return last != 0 && now > last && now - last > a_maxFrames;
	}

	bool FillLive()
	{
		if (!g_owned.load(std::memory_order_relaxed)) {
			return g_fillLive.load(std::memory_order_relaxed);
		}
		// Freshness guard: the verdict site stops running the moment the weapon
		// is holstered or a blocking menu opens (vanilla eligibility), and the
		// last verdict would otherwise stick - an unhooked equip-path caller
		// re-arms the widget during unequip, and a stale `live` keeps the fill
		// running on a holstered weapon. If no eval landed within the last
		// ~half second of frames, the lens freezes.
		const auto last = g_evalFrame.load(std::memory_order_relaxed);
		const auto now = Hooks::FrameCount();
		if (now > last && now - last > 45) {
			return false;
		}
		return g_fillLive.load(std::memory_order_relaxed);
	}

	Diag GetDiag()
	{
		Diag d{};
		d.enabled = *Settings::poseGateEnabled;
		d.owned = g_owned.load(std::memory_order_relaxed);
		// The effective answer (freshness guard included), not the raw stored
		// flag, which can read true while the fill is frozen by staleness.
		d.fillLive = FillLive();
		d.dist = g_dist.load(std::memory_order_relaxed);
		d.lateral = g_lateral.load(std::memory_order_relaxed);
		d.lookDeg = g_lookDeg.load(std::memory_order_relaxed);
		d.evals = g_evals.load(std::memory_order_relaxed);
		return d;
	}
}
