#include "TrueScopes/Hooks.h"

#include "Settings/Settings.h"
#include "TrueScopes/Addresses.h"

namespace TrueScopes::Hooks
{
	namespace
	{
		bool g_installed = false;

		using ImageSpaceManagerCopy_t = void (*)(std::uint32_t a_srcRT, std::uint32_t a_dstRT);

		[[nodiscard]] ImageSpaceManagerCopy_t ImageSpaceCopy()
		{
			static REL::Relocation<ImageSpaceManagerCopy_t> func{ REL::Offset(Addr::kImageSpaceManagerCopy) };
			return func.get();
		}

		// Phase 1 fill: copy the main double-wide frame into the lens target every N
		// frames. The engine's own per-draw slot-6 bind displays it whenever the widget
		// material draws. Phase 2 replaces the Copy with our own mono world render from
		// PrimaryWeaponScopeCamera into a temp RT, then Copy(temp, 0x62)
		// (recipe: ROUTE_B_STATIC_MAP_2026-08-06.md section 3.2).
		struct RenderFillHook
		{
			static void thunk()
			{
				if (g_installed && *Settings::fillEnabled) {
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
		REL::Relocation<std::uintptr_t> armSetter{ REL::Offset(Addr::kScopeArmSetter) };
		REL::Relocation<std::uintptr_t> fillSite{ REL::Offset(Addr::kRenderFillCallSite) };

		// mov [rcx+3], dl; ret — the arm setter
		static constexpr std::uint8_t kSetterOrig[] = { 0x88, 0x51, 0x03, 0xC3 };
		// call thunk_FUN_14284e950
		static constexpr std::uint8_t kFillSiteOrig[] = { 0xE8, 0x87, 0x70, 0xAC, 0x01 };

		if (!VerifyBytes(armSetter, { kSetterOrig, 4 }, "scope-arm setter"sv)) {
			return false;
		}
		if (!VerifyBytes(fillSite, { kFillSiteOrig, 5 }, "render fill site"sv)) {
			return false;
		}

		// 1. Defang the arm: renderer+3 stays 0 forever. The rest of the vanilla enable
		//    switch keeps working (widget show/hide, material cache, Pip-Boy handling) —
		//    only the frame redirect is severed.
		REL::safe_write(armSetter.address(), static_cast<std::uint8_t>(0xC3));
		logger::info(FMT_STRING("scope-arm setter defanged at {:016X}"), armSetter.address());

		// 2. Fill hook, every frame in the normal draw path.
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

		// Phase 1 needs always-on: with the arm defanged, the vanilla enable switch has
		// no working on/off state memory (it reads renderer+3 as "current state"), so
		// eye-gated hide transitions would strand the widget. Always-on sidesteps that
		// until the phase-1.5 enable-switch replacement lands.
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
