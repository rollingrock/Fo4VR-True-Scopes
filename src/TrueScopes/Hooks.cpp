#include "TrueScopes/Hooks.h"

#include "Settings/Settings.h"
#include "TrueScopes/Addresses.h"

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
				if (g_installed && g_scopeActive.load() && *Settings::fillEnabled) {
					static std::uint32_t frame = 0;
					if ((++frame % static_cast<std::uint32_t>(std::max<std::int64_t>(1, *Settings::fillEveryNFrames))) == 0) {
						ImageSpaceCopy()(Addr::kRT_MainFrame, Addr::kRT_ScopeLens);
					}
				}
				func();
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
