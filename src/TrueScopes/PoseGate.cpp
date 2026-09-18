#include "TrueScopes/PoseGate.h"

#include "Settings/Settings.h"
#include "TrueScopes/FrikBridge.h"
#include "TrueScopes/Hooks.h"
#include "TrueScopes/LensComposite.h"
#include "TrueScopes/ScopeIdent.h"

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
		std::atomic_bool           g_looking{ false };  // the narrow FRIK predicate, after dwell
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
		// Re-arm dwell: tick when the enter conditions first held while the gate
		// was off. Game thread only.
		std::uint64_t g_armPendingSince = 0;

		struct Sample
		{
			float dist;     // |eye → ocular|
			float lateral;  // eye's perpendicular distance from the tube axis line
			float lookDeg;  // HMD forward vs direction to ocular
			bool  valid;
			float ocular[3];  // the ocular point the gate judged (ScopeParent world)
			float head[3];    // HMD centre it judged from
			float axis[3];    // tube axis, unit, down-range
			float axial[2];   // per eye (left, right): (ocular - eye) . axis; <= 0 = eye ahead of the eyepiece
			bool  sentinel;   // both eyes ahead of the eyepiece: "not looking through", whatever the distances
		};

		// Where the gate's ocular sits against the scope's real glass: the census
		// face of the live scope shape (ScopeIdent re-reads that node each call).
		// A left carry on 2026-09-18 read dist 53-60 with the eyepiece supposedly
		// at the eye, where a right-hand raise reads 17-22. Either the rifle was
		// not at the eye or ScopeParent was not on the rifle; this number is the
		// difference. Two distances: HMD to the glass, and the gate's point to
		// the glass. Log lines only, never per frame.
		void FaceCheck(const Sample& a_s, char (&a_out)[64]) noexcept
		{
			float face[3] = {};
			if (!ScopeIdent::OcularFaceWorld(face)) {
				std::snprintf(a_out, sizeof(a_out), " face=n/a");
				return;
			}
			const auto d = [](const float (&a)[3], const float (&b)[3]) {
				const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
				return std::sqrt(x * x + y * y + z * z);
			};
			std::snprintf(a_out, sizeof(a_out), " hmd-to-glass=%.1f gate-point-to-glass=%.1f",
				d(a_s.head, face), d(a_s.ocular, face));
		}

		// The raw reads behind Compute, and nothing else: POD frame only so it can
		// sit under SEH. This is a per-frame game-thread path with no other guard,
		// and the nodes it reads are the ones a FRIK camera-rig rebuild - and,
		// since 2026-09-18, a carry re-parenting ScopeParent at frame start and
		// restoring it on skeleton release - replaces under it. A freed node that
		// still reads as memory yields garbage the finite checks reject; one that
		// no longer does faulted the game. Now it reads as "no sample".
		struct RawPose
		{
			float axis[3];    // ScopeParent world rotate, local Y (floats 4..6)
			float ocular[3];  // ScopeParent world translate
			float camPos[3];  // camera root world translate
			float headX[3];   // camera root world rotate, floats 0..2
			float headFwd[3]; // camera root world rotate, floats 4..6
		};

		static bool ReadRawPose(std::uintptr_t a_player, RawPose& a_out) noexcept
		{
			__try {
				const auto scopeParent = *reinterpret_cast<std::uintptr_t*>(a_player + kScopeParentInPlayer);
				if (!scopeParent) {
					return false;
				}
				const auto playerCam = *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + kPlayerCameraGlobal);
				if (!playerCam) {
					return false;
				}
				const auto camRoot = *reinterpret_cast<std::uintptr_t*>(playerCam + kCameraRoot);
				if (!camRoot) {
					return false;
				}
				const auto* rot = reinterpret_cast<const float*>(scopeParent + kWorldRotate);
				const auto* t = reinterpret_cast<const float*>(scopeParent + kWorldTranslate);
				const auto* camPos = reinterpret_cast<const float*>(camRoot + kWorldTranslate);
				const auto* camRot = reinterpret_cast<const float*>(camRoot + kWorldRotate);
				for (std::size_t k = 0; k < 3; ++k) {
					a_out.axis[k] = rot[4 + k];
					a_out.ocular[k] = t[k];
					a_out.camPos[k] = camPos[k];
					a_out.headX[k] = camRot[k];
					a_out.headFwd[k] = camRot[4 + k];
				}
				for (const float* v : { a_out.axis, a_out.ocular, a_out.camPos, a_out.headX, a_out.headFwd }) {
					for (std::size_t k = 0; k < 3; ++k) {
						if (!std::isfinite(v[k])) {
							return false;
						}
					}
				}
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		[[nodiscard]] Sample Compute(std::uintptr_t a_player) noexcept
		{
			Sample s{};
			if (!a_player) {
				return s;
			}
			RawPose raw{};
			if (!ReadRawPose(a_player, raw)) {
				return s;
			}

			// Tube axis: world direction of ScopeParent's local Y (down-range).
			// With the transposed-on-read convention, world_dir(local Y)[r] =
			// R[r][1] = m[1*4 + r] — raw floats 4..6. (This is also exactly the
			// +0x80/+0x84/+0x88 triple the vanilla gate dots against.)
			float axis[3] = { raw.axis[0], raw.axis[1], raw.axis[2] };
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
			const float D[3] = { raw.ocular[0], raw.ocular[1], raw.ocular[2] };
			const float* camPos = raw.camPos;
			// Head axes, same convention: world X = m[0..2], world forward (local
			// Y) = m[4..6].
			const float headX[3] = { raw.headX[0], raw.headX[1], raw.headX[2] };
			const float headFwd[3] = { raw.headFwd[0], raw.headFwd[1], raw.headFwd[2] };

			// Both real eyes (camera root is the HMD centre). The eye the lens has
			// latched for this scope episode wins (LensComposite::AimingEyeSide,
			// or the forced eyeBoxEye); before the first fill of an episode, the
			// nearer one. Gate and lens must agree or the gate drops the picture
			// the lens is still centred on.
			const float halfIpd = 0.5f * static_cast<float>(*Settings::eyeBoxIpdUnits);
			float       bestLat = -1.0f, bestDist = 0.0f;
			float       sideLat[2] = { -1.0f, -1.0f }, sideDist[2] = {};
			for (std::size_t k = 0; k < 3; ++k) {
				s.ocular[k] = D[k];
				s.head[k] = camPos[k];
				s.axis[k] = axis[k];
			}
			for (const float side : { -1.0f, 1.0f }) {
				const float e[3] = { camPos[0] + side * halfIpd * headX[0],
					                 camPos[1] + side * halfIpd * headX[1],
					                 camPos[2] + side * halfIpd * headX[2] };
				const float v[3] = { D[0] - e[0], D[1] - e[1], D[2] - e[2] };
				const float axial = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
				s.axial[side > 0.0f ? 1 : 0] = axial;
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
				sideLat[side > 0.0f ? 1 : 0] = lat;
				sideDist[side > 0.0f ? 1 : 0] = std::sqrt(d2);
				if (bestLat < 0.0f || lat < bestLat) {
					bestLat = lat;
					bestDist = std::sqrt(d2);
				}
			}
			if (const int latched = LensComposite::AimingEyeSide(); latched != 0) {
				const int i = latched > 0 ? 1 : 0;
				if (sideLat[i] >= 0.0f) {
					bestLat = sideLat[i];
					bestDist = sideDist[i];
				}
			}
			if (bestLat < 0.0f) {
				// Pose sources are fine - the answer is simply "not looking
				// through" (both eyes in front of, or level with, the ocular).
				s.valid = true;
				s.sentinel = true;
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
			for (std::size_t k = 0; k < 3; ++k) {
				s.ocular[k] = D[k];
				s.head[k] = camPos[k];
			}
			return s;
		}
	}

	namespace
	{
		// The fill hook's sample of the same pose, taken after every mod's frame
		// update; the site and the fill hook share the game thread (measured
		// 2026-09-17), so a plain copy is enough.
		Sample        g_frameEnd{};
		std::uint64_t g_frameEndFrame = 0;
	}

	void SampleAtFrameEnd(std::uintptr_t a_player)
	{
		if (!*Settings::poseGateEnabled || !a_player) {
			return;
		}
		g_frameEnd = Compute(a_player);
		g_frameEndFrame = Hooks::FrameCount();
	}

	bool OnGateVerdict(std::uintptr_t a_player, bool a_vanillaVerdict)
	{
		g_siteFrame.store(Hooks::FrameCount(), std::memory_order_relaxed);
		if (!*Settings::poseGateEnabled) {
			g_owned.store(false, std::memory_order_relaxed);
			g_looking.store(false, std::memory_order_relaxed);
			g_fillLive.store(true, std::memory_order_relaxed);
			return a_vanillaVerdict;
		}
		const Sample site = Compute(a_player);
		// The frame-end sample is the previous frame's; stale after a hitch.
		const bool frameEndFresh = g_frameEnd.valid && Hooks::FrameCount() - g_frameEndFrame <= 2;
		if (site.valid && frameEndFresh && site.sentinel != g_frameEnd.sentinel) {
			// The two reads of one pose disagree on whether the eyes are behind
			// the eyepiece: something moved the weapon between the engine's arm
			// pass and the end of the frame. Once per second.
			static std::uint64_t s_tick = 0;
			const auto           now = ::GetTickCount64();
			if (now - s_tick >= 1000) {
				s_tick = now;
				logger::info(FMT_STRING("pose gate: site and frame-end samples disagree - site axial L/R {:.1f}/{:.1f} axis ({:.2f},{:.2f},{:.2f}) "
				                        "ocular ({:.1f},{:.1f},{:.1f}); frame-end axial L/R {:.1f}/{:.1f} axis ({:.2f},{:.2f},{:.2f}) ocular ({:.1f},{:.1f},{:.1f}) "
				                        "dist {:.1f} lat {:.2f} look {:.1f}; head ({:.1f},{:.1f},{:.1f}); judging the {}"),
					site.axial[0], site.axial[1], site.axis[0], site.axis[1], site.axis[2], site.ocular[0], site.ocular[1], site.ocular[2],
					g_frameEnd.axial[0], g_frameEnd.axial[1], g_frameEnd.axis[0], g_frameEnd.axis[1], g_frameEnd.axis[2],
					g_frameEnd.ocular[0], g_frameEnd.ocular[1], g_frameEnd.ocular[2],
					g_frameEnd.dist, g_frameEnd.lateral, g_frameEnd.lookDeg, site.head[0], site.head[1], site.head[2],
					*Settings::poseSampleAtFrameEnd ? "frame-end sample"sv : "site sample (poseSampleAtFrameEnd off)"sv);
			}
		}
		const Sample s = (*Settings::poseSampleAtFrameEnd && frameEndFresh) ? g_frameEnd : site;
		g_evals.fetch_add(1, std::memory_order_relaxed);
		g_evalFrame.store(Hooks::FrameCount(), std::memory_order_relaxed);
		if (!s.valid) {
			// Pose sources missing (no rig / no camera yet): behave exactly like
			// the un-hooked game rather than guessing.
			g_owned.store(false, std::memory_order_relaxed);
			g_looking.store(false, std::memory_order_relaxed);
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
		float       latMax = band(*Settings::poseMaxLateral, *Settings::poseExitLateral);
		const float lookMax = band(*Settings::poseLookConeDegrees, *Settings::poseLookConeExitDegrees);
		// Constant angular cone beyond the reference distance instead of a
		// constant lateral offset (see the setting). Never tightens up close.
		{
			const auto adapt = static_cast<float>(*Settings::poseLateralDistanceAdapt);
			const auto refD = static_cast<float>(*Settings::poseLateralRefDist);
			if (adapt > 0.0f && refD > 1.0f && std::isfinite(s.dist)) {
				latMax *= (std::max)(1.0f, 1.0f + adapt * (s.dist / refD - 1.0f));
			}
		}
		// An eye on the tube axis is looking through the scope regardless of
		// what the head-forward angle says - at close range that angle is
		// dominated by head-translation noise. The waiver keeps a wider bound
		// while live (its own hysteresis, like every other axis).
		const auto  lookWaive = static_cast<float>(*Settings::poseLookWaiveLateral);
		const float waiveEff = was ? lookWaive * 1.25f : lookWaive;
		const bool  lookOk = (lookWaive > 0.0f && s.lateral < waiveEff) || s.lookDeg < lookMax;
		bool rawLive = s.dist < dMax && s.lateral < latMax && lookOk;
		// Exit debounce: the tests read a ScopeParent transform the render
		// thread rewrites every tracked frame, so a single out-of-band sample
		// can be a torn read - and the enter dwell below turns any spurious
		// exit into a quarter-second frozen lens. Two consecutive misses
		// (~22 ms) drop the gate; one does not.
		{
			static std::uint32_t s_exitStreak = 0;
			if (was && !rawLive) {
				if (++s_exitStreak < 2) {
					rawLive = true;
				}
			} else {
				s_exitStreak = 0;
			}
		}
		bool live = rawLive;
		// Enter-edge dwell: conditions must hold continuously before re-arming.
		// The exit edge stays two-sample (above).
		if (rawLive && !was) {
			const auto dwellMs = static_cast<std::uint64_t>(
				(std::max)(std::int64_t(0), *Settings::poseReArmDwellMs));
			if (dwellMs > 0) {
				const auto now = static_cast<std::uint64_t>(::GetTickCount64());
				if (g_armPendingSince == 0) {
					g_armPendingSince = now;
				}
				if (now - g_armPendingSince < dwellMs) {
					live = false;
				}
			}
		} else {
			g_armPendingSince = 0;
		}
		if (live != was) {
			char face[64];
			FaceCheck(s, face);
			if (s.sentinel) {
				logger::info(
					FMT_STRING("pose gate live -> {} (both eyes ahead of the eyepiece: axial L/R {:.1f}/{:.1f}, axis ({:.2f},{:.2f},{:.2f}){})"),
					live, s.axial[0], s.axial[1], s.axis[0], s.axis[1], s.axis[2], face);
			} else {
				logger::info(
					FMT_STRING("pose gate live -> {} (dist={:.1f} lat={:.2f} look={:.1f}deg{})"),
					live, s.dist, s.lateral, s.lookDeg, face);
			}
		}
		g_liveState = live;

		// One line when the gate keeps refusing on a drawn weapon (~5 s of evals
		// without ever going live this draw) - without it, nothing in the log
		// says why no scope appears.
		{
			static std::uint32_t s_sinceLive = 0;
			static bool          s_warned = false;
			static std::uint64_t s_lastEval = 0;
			const auto nowF = Hooks::FrameCount();
			if (s_lastEval && nowF > s_lastEval && nowF - s_lastEval > 90) {
				s_sinceLive = 0;  // the site was quiet - a new draw episode
				s_warned = false;
			}
			s_lastEval = nowF;
			if (live) {
				s_sinceLive = 0;
				s_warned = false;
			} else if (!s_warned && ++s_sinceLive >= 450) {
				s_warned = true;
				char face[64];
				FaceCheck(s, face);
				logger::info(
					FMT_STRING("pose gate: {} evals this draw without going live (last dist={:.1f} "
					           "lat={:.2f} look={:.1f}deg vs enter {:.0f}/{:.1f}/{:.0f}{}) - tune "
					           "poseMax*/poseLookCone* if this weapon should activate"),
					s_sinceLive, s.dist, s.lateral, s.lookDeg, dMax, latMax, lookMax, face);
			}
		}

		g_dist.store(s.dist, std::memory_order_relaxed);
		g_lateral.store(s.lateral, std::memory_order_relaxed);
		g_lookDeg.store(s.lookDeg, std::memory_order_relaxed);
		g_owned.store(true, std::memory_order_relaxed);
		g_fillLive.store(live, std::memory_order_relaxed);
		// FRIK (0.79+) keys its scope behaviour on this once we are its provider.
		// Game thread, which is what its contract asks. The render gate is wide
		// on purpose (it decides whether the render is worth doing, and goes live
		// at the hip); FRIK is told the narrower truth - eye near the axis and
		// looking down the tube - with its own hysteresis so a flap at the
		// threshold does not become a damping switch per frame. 0 on either knob
		// = publish the render gate as-is.
		{
			static bool s_frikLooking = false;
			const auto  latMax = static_cast<float>(*Settings::frikLookingLateral);
			const auto  coneMax = static_cast<float>(*Settings::frikLookingConeDegrees);
			bool        looking = live;
			if (live && latMax > 0.0f) {
				looking = looking && s.lateral < (s_frikLooking ? latMax * 1.25f : latMax);
			}
			if (live && coneMax > 0.0f) {
				looking = looking && s.lookDeg < (s_frikLooking ? coneMax + 5.0f : coneMax);
			}
			// Dwell: a candidate state has to hold for frikLookingDwellMs before it is
			// published, in both directions. Hysteresis alone let an exit/enter/exit
			// through in 33 ms at the threshold edge; FRIK switches damping per flip.
			static bool          s_pending = false;
			static std::uint64_t s_pendingSince = 0;
			const auto           now = static_cast<std::uint64_t>(::GetTickCount64());
			const auto           dwellMs = static_cast<std::uint64_t>((std::max)(std::int64_t(0), *Settings::frikLookingDwellMs));
			if (looking == s_frikLooking) {
				s_pendingSince = 0;
			} else {
				if (s_pendingSince == 0 || s_pending != looking) {
					s_pending = looking;
					s_pendingSince = now;
				}
				if (now - s_pendingSince >= dwellMs) {
					s_frikLooking = looking;
					g_looking.store(looking, std::memory_order_relaxed);
					s_pendingSince = 0;
					FrikBridge::PublishLookingThrough(looking, s.dist, s.lateral, s.lookDeg);
				}
			}
		}

		// The verdict fed to vanilla is always the pose. Feeding a perpetual
		// "true" keeps the player sighted the whole time the weapon is drawn -
		// the enable switch's ActorState call drives the sighted state and
		// sighted opens ScopeMenu. Widget permanence is plugin-owned node
		// visibility in Hooks.cpp (WidgetPresence), which vanilla state never
		// sees. (FRIK used to collapse the body and block the Pip-Boy on
		// ScopeMenu; as our provider it no longer does, but the vanilla reasons
		// stand.)
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

	bool SiteEverRan()
	{
		return g_siteFrame.load(std::memory_order_relaxed) != 0;
	}

	bool Owns()
	{
		return g_owned.load(std::memory_order_relaxed);
	}

	bool LookingThrough()
	{
		return g_looking.load(std::memory_order_relaxed);
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
