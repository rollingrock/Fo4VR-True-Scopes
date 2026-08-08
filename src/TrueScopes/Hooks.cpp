#include "TrueScopes/Hooks.h"

#include "Settings/Settings.h"
#include "TrueScopes/Addresses.h"
#include "TrueScopes/ScopeRender.h"

namespace TrueScopes::Hooks
{
	namespace
	{
		bool g_installed = false;

		// Plugin-owned replacement for the "scope render armed" state that vanilla keeps
		// in BSGraphics::Renderer+3. The real +3 stays 0 forever, so Main::Swap's frame
		// redirect (black main view, rebuild churn, deferred-release hazards) never
		// engages — but the enable switch still edge-triggers its show/hide block off
		// this value exactly like vanilla.
		std::atomic_bool g_scopeActive = false;

		using ImageSpaceManagerCopy_t = void (*)(std::uint32_t a_srcRT, std::uint32_t a_dstRT);

		[[nodiscard]] ImageSpaceManagerCopy_t ImageSpaceCopy()
		{
			static REL::Relocation<ImageSpaceManagerCopy_t> func{ REL::Offset(Addr::kImageSpaceManagerCopy) };
			return func.get();
		}

		// Replaces "call FUN_141d947a0(renderer, on)" — the arm write.
		struct ScopeArmWriteHook
		{
			static void thunk([[maybe_unused]] void* a_renderer, char a_on)
			{
				const bool on = a_on != 0;
				if (g_scopeActive.exchange(on) != on) {
					logger::info(FMT_STRING("scope active -> {}"), on);
					if (on) {
						// live-tuning loop: re-read the TOML on every scope-in
						Settings::load();
						if (*Settings::retryAfterFault) {
							ScopeRender::RetryAfterFault();
						}
					}
				}
				// deliberately NOT writing renderer+3
			}
			static inline REL::Relocation<decltype(&thunk)> func;
		};

		// Replaces "call FUN_141d947b0(renderer)" — the guard's state read.
		struct ScopeStateReadHook
		{
			static char thunk([[maybe_unused]] void* a_renderer)
			{
				return g_scopeActive.load() ? 1 : 0;
			}
			static inline REL::Relocation<decltype(&thunk)> func;
		};

		// Phase 1 fill: while the scope widget is active, copy the main double-wide
		// frame into the lens target every N frames. The engine's own per-draw slot-6
		// bind displays it whenever the widget material draws. Phase 2 replaces the Copy
		// with our own mono world render from PrimaryWeaponScopeCamera into a temp RT,
		// then Copy(temp, 0x62) (recipe: ROUTE_B_STATIC_MAP_2026-08-06.md section 3.2).
		struct RenderFillHook
		{
			static void thunk()
			{
				if (g_installed && *Settings::fillEnabled) {
					// Burst forensics v2 (v0.2.46): the tonemap delivery repaints only
					// its own footprint of RT 0x62 — the rounded top-left crescent is
					// OUTSIDE it and keeps whatever was painted last (the v0.2.22-era
					// "camera-independent rounded boundary", finally explained by the
					// v0.2.45 red leak). That makes the crescent a per-frame status
					// LED: pre-paint the whole lens BLUE before every fill and RED on
					// every gate-paused frame while aiming. During a black burst the
					// crescent color names the writer: BLUE = fills ran and delivered
					// black (post-composite path), RED = eye-gate paused the fill,
					// BLACK = some third party cleared the lens RT.
					static std::uint32_t sinceFill = 1000;
					if (g_scopeActive.load()) {
						static std::uint32_t frame = 0;
						if ((++frame % static_cast<std::uint32_t>(std::max<std::int64_t>(1, *Settings::fillEveryNFrames))) == 0) {
							if (*Settings::diagPauseTint) {
								ScopeRender::TintLens(0.0f, 0.0f, 0.5f);
							}
							sinceFill = 0;
							const bool rendered =
								*Settings::lensMode >= 2 &&  // 2 = normal, 3 = G-buffer diagnostic
								ScopeRender::Available() &&
								ScopeRender::Render();
							if (!rendered && *Settings::lensMode != 0) {
								ImageSpaceCopy()(Addr::kRT_MainFrame, Addr::kRT_ScopeLens);
							}
						} else if (*Settings::diagPauseTint && sinceFill < 300) {
							++sinceFill;
							// Cadence-skipped frame -> GREEN lens.
							ScopeRender::TintLens(0.0f, 1.0f, 0.0f);
						}
					} else if (*Settings::diagPauseTint && sinceFill < 300) {
						++sinceFill;
						// Eye-gate pause during active aiming -> RED lens. The
						// sinceFill window keeps the tint from leaking into ordinary
						// unscoped gameplay (the v0.2.45 always-red artifact).
						ScopeRender::TintLens(1.0f, 0.0f, 0.0f);
					}
				}
				func();
			}
			static inline REL::Relocation<decltype(&thunk)> func;
		};

		// The deferred resolve's two light-accum bind sites (slot 0 = 0x24/0x6a, slot 1 =
		// 0x25/0x6b) pass bind mode 0 = clear-on-apply. While OUR resolve call is on the
		// stack we force mode 3 (no clear) so the sun BSDFLightDir pass we pre-drew into
		// 0x6a/0x6b survives for the composite. Engine frames pass through untouched.
		struct ResolveAccumBind0Hook
		{
			static void thunk(std::uintptr_t a_rtm, std::uint32_t a_slot, std::int32_t a_rt, std::uint32_t a_mode)
			{
				func(a_rtm, a_slot, a_rt, ScopeRender::InOwnResolve() ? 3 : a_mode);
			}
			static inline REL::Relocation<decltype(&thunk)> func;
		};
		struct ResolveAccumBind1Hook
		{
			static void thunk(std::uintptr_t a_rtm, std::uint32_t a_slot, std::int32_t a_rt, std::uint32_t a_mode)
			{
				func(a_rtm, a_slot, a_rt, ScopeRender::InOwnResolve() ? 3 : a_mode);
			}
			static inline REL::Relocation<decltype(&thunk)> func;
		};

		[[nodiscard]] bool VerifyBytes(
			const REL::Relocation<std::uintptr_t>& a_target,
			std::span<const std::uint8_t> a_expected,
			std::string_view a_what)
		{
			const auto* p = reinterpret_cast<const std::uint8_t*>(a_target.address());
			if (!std::equal(a_expected.begin(), a_expected.end(), p)) {
				logger::critical(
					FMT_STRING("byte check FAILED at {} ({:016X}): found {:02X} {:02X} {:02X} {:02X} {:02X} — wrong game version? Leaving the game untouched."),
					a_what, a_target.address(), p[0], p[1], p[2], p[3], p[4]);
				return false;
			}
			return true;
		}
	}

	bool Install()
	{
		REL::Relocation<std::uintptr_t> stateReadSite{ REL::Offset(Addr::kScopeStateReadCallSite) };
		REL::Relocation<std::uintptr_t> armWriteSite{ REL::Offset(Addr::kScopeArmWriteCallSite) };
		REL::Relocation<std::uintptr_t> fillSite{ REL::Offset(Addr::kRenderFillCallSite) };

		// call FUN_141d947b0 (read renderer+3)
		static constexpr std::uint8_t kStateReadOrig[] = { 0xE8, 0xF4, 0x9C, 0xE9, 0x00 };
		// call FUN_141d947a0 (write renderer+3)
		static constexpr std::uint8_t kArmWriteOrig[] = { 0xE8, 0xCD, 0x9C, 0xE9, 0x00 };
		// call thunk_FUN_14284e950
		static constexpr std::uint8_t kFillSiteOrig[] = { 0xE8, 0x87, 0x70, 0xAC, 0x01 };

		if (!VerifyBytes(stateReadSite, { kStateReadOrig, 5 }, "scope-state read site"sv)) {
			return false;
		}
		if (!VerifyBytes(armWriteSite, { kArmWriteOrig, 5 }, "scope-arm write site"sv)) {
			return false;
		}
		if (!VerifyBytes(fillSite, { kFillSiteOrig, 5 }, "render fill site"sv)) {
			return false;
		}

		// Reroute the enable switch's state memory to the plugin: renderer+3 is never
		// written (redirect can't engage), and the show/hide block stays edge-triggered.
		pstl::write_thunk_call<ScopeStateReadHook>(stateReadSite.address());
		pstl::write_thunk_call<ScopeArmWriteHook>(armWriteSite.address());
		logger::info("enable-switch state hooks installed"sv);

		// Fill hook, every frame in the normal draw path.
		pstl::write_thunk_call<RenderFillHook>(fillSite.address());
		logger::info(FMT_STRING("render fill hook installed at {:016X}"), fillSite.address());

		// Resolve accum-bind hooks (sun pass survival). Non-fatal: without them the sun
		// pre-draw is skipped and everything else behaves like v0.2.25.
		{
			REL::Relocation<std::uintptr_t> bind0{ REL::Offset(Addr::kResolveAccumBind0CallSite) };
			REL::Relocation<std::uintptr_t> bind1{ REL::Offset(Addr::kResolveAccumBind1CallSite) };
			static constexpr std::uint8_t kBind0Orig[] = { 0xE8, 0x00, 0xA0, 0x5B, 0xFF };
			static constexpr std::uint8_t kBind1Orig[] = { 0xE8, 0xD1, 0x9F, 0x5B, 0xFF };
			const bool ok =
				VerifyBytes(bind0, { kBind0Orig, 5 }, "resolve accum bind 0"sv) &&
				VerifyBytes(bind1, { kBind1Orig, 5 }, "resolve accum bind 1"sv);
			if (ok) {
				pstl::write_thunk_call<ResolveAccumBind0Hook>(bind0.address());
				pstl::write_thunk_call<ResolveAccumBind1Hook>(bind1.address());
				logger::info("resolve accum-bind hooks installed (sun pass enabled)"sv);
			}
			ScopeRender::SetSunBindHooksInstalled(ok);
		}

		// Vanilla scope imod suppression (cosmetic-only calls now that the redirect is
		// disarmed). Verified before patching; failure here is non-fatal.
		static constexpr std::uint8_t kImodSiteAOrig[] = { 0xE8, 0xD0, 0x49, 0x7B, 0xFF };
		static constexpr std::uint8_t kImodSiteBOrig[] = { 0xE8, 0x7F, 0x4E, 0x7B, 0xFF };
		static constexpr std::uint8_t kFadeSiteOrig[] = { 0xE8, 0x2F, 0x49, 0x48, 0xFF };
		// xor eax,eax + 3 nops — the Trigger return value is refcount-stored by the
		// caller, so it must be nulled, not left as garbage.
		static constexpr std::uint8_t kNullReturnPatch[] = { 0x33, 0xC0, 0x90, 0x90, 0x90 };
		static constexpr std::uint8_t kNopPatch[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };

		if (*Settings::disableScopeBlackout) {
			REL::Relocation<std::uintptr_t> siteA{ REL::Offset(Addr::kScopeBlackoutImodSiteA) };
			REL::Relocation<std::uintptr_t> siteB{ REL::Offset(Addr::kScopeBlackoutImodSiteB) };
			if (VerifyBytes(siteA, { kImodSiteAOrig, 5 }, "blackout imod site A"sv) &&
				VerifyBytes(siteB, { kImodSiteBOrig, 5 }, "blackout imod site B"sv)) {
				REL::safe_write(siteA.address(), kNullReturnPatch, sizeof(kNullReturnPatch));
				REL::safe_write(siteB.address(), kNullReturnPatch, sizeof(kNullReturnPatch));
				logger::info("scope blackout imod suppressed"sv);
			}
		}
		if (*Settings::disableApproachFade) {
			REL::Relocation<std::uintptr_t> siteC{ REL::Offset(Addr::kScopeApproachFadeSite) };
			if (VerifyBytes(siteC, { kFadeSiteOrig, 5 }, "approach fade site"sv)) {
				REL::safe_write(siteC.address(), kNopPatch, sizeof(kNopPatch));
				logger::info("approach fade suppressed"sv);
			}
		}

		g_installed = true;
		return true;
	}

	void OnGameLoaded()
	{
		if (!g_installed) {
			return;
		}

		// Optional: force iScopeEnabled to 2 (always-on). With the edge-triggered state
		// hooks this is a convenience, not a requirement — the vanilla default (1,
		// eye-gated) now works correctly.
		if (*Settings::forceAlwaysOn) {
			REL::Relocation<std::uint32_t*> scopeEnabled{ REL::Offset(Addr::kIScopeEnabledValue) };
			*scopeEnabled.get() = 2;
			logger::info("iScopeEnabled forced to 2 (always-on)"sv);
		}

		REL::Relocation<std::uint8_t*> renderer{ REL::Offset(Addr::kRendererInstance) };
		logger::info(
			FMT_STRING("renderer flags on load: +1={} +2={} +3={} +4={} +5={}"),
			renderer.get()[1], renderer.get()[2], renderer.get()[3], renderer.get()[4], renderer.get()[5]);
	}
}
