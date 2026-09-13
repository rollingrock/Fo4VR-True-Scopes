#include "TrueScopes/FrikBridge.h"

#include "Settings/Settings.h"
#include "external/FRIKApiV3.h"

namespace TrueScopes::FrikBridge
{
	namespace
	{
		constexpr const char* kTag = "TrueScopes";

		// Game thread only, both of them.
		bool g_registered = false;
		bool g_lastLooking = false;
		bool g_attempted = false;

		using frik::api::FRIKApiV3;
		using ScopeCapability = FRIKApiV3::ScopeCapability;

		// A blocking menu opening is where vanilla force-offs the scope through
		// the enable switch's other call sites, which we leave unhooked; our
		// verdict site then stops running, so a published true would stay true
		// and FRIK would keep the wrist Pip-Boy handler skipped and the scoped
		// damping selected for as long as the menu is up. Publish false on the
		// open edge; the next verdict re-arms it. Game thread.
		[[nodiscard]] bool BlockingMenu(std::string_view a_name) noexcept
		{
			return a_name == "PipboyMenu"sv || a_name == "PauseMenu"sv || a_name == "TerminalMenu"sv ||
			       a_name == "ContainerMenu"sv || a_name == "DialogueMenu"sv || a_name == "BarterMenu"sv ||
			       a_name == "WorkshopMenu"sv || a_name == "LockpickingMenu"sv || a_name == "MessageBoxMenu"sv ||
			       a_name == "ExamineMenu"sv || a_name == "CookingMenu"sv || a_name == "LevelUpMenu"sv ||
			       a_name == "VATSMenu"sv || a_name == "LoadingMenu"sv || a_name == "MainMenu"sv ||
			       a_name == "SleepWaitMenu"sv || a_name == "SPECIALMenu"sv || a_name == "BookMenu"sv;
		}

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::MenuOpenCloseEvent& a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				const char* name = a_event.menuName.c_str();
				if (a_event.opening && name && BlockingMenu(name)) {
					PublishLookingThrough(false);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		MenuSink g_menuSink;
		bool     g_menuSinkInstalled = false;

		void InstallMenuSink()
		{
			if (g_menuSinkInstalled) {
				return;
			}
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				logger::warn("FRIK scope provider: UI singleton not up at kGameLoaded - menu-open stand-down not installed"sv);
				return;
			}
			ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_menuSink);
			g_menuSinkInstalled = true;
		}
	}

	void OnGameLoaded()
	{
		if (g_registered || g_attempted) {
			return;
		}
		g_attempted = true;
		if (!*Settings::frikProvider) {
			logger::info("FRIK scope provider: disabled by TOML (frikProvider=false) - FRIK keys its scope behaviour on ScopeMenu as before"sv);
			return;
		}
		const int err = FRIKApiV3::initialize();
		if (err != 0) {
			// 1 = FRIK.dll not loaded, 2/3 = no v3 API (FRIK older than 0.79),
			// 4 = API older than the header asks, 5 = FRIK's table smaller than ours.
			logger::info(FMT_STRING("FRIK scope provider: API v3 init returned {} ({}) - no provider registered, FRIK keys its scope behaviour on ScopeMenu as before"),
				err,
				err == 1 ? "FRIK.dll not loaded"sv :
				err == 2 || err == 3 ? "FRIK predates API v3 (needs 0.79 or later)"sv :
				err == 4 ? "FRIK API v3 older than this build asks for"sv :
				           "FRIK API table smaller than this header"sv);
			return;
		}
		const auto* inst = FRIKApiV3::inst;
		if (!inst || inst->getVersion() < 3 || !inst->setScopeProvider) {
			logger::info("FRIK scope provider: API present but without the v3.3 scope calls - no provider registered"sv);
			return;
		}
		// KeepsBodyVisible: our render is the main view with the body in it.
		// PublishesLookingThrough: the pose gate is the signal, not ScopeMenu.
		// Not OwnsScopeCamera: FRIK still writes the scope camera rotation and
		// our per-render translate overwrite sits on top, as it always has.
		// Not OwnsDamping: the damping decision is still open on FRIK's side.
		const auto caps = static_cast<std::uint32_t>(ScopeCapability::KeepsBodyVisible) |
		                  static_cast<std::uint32_t>(ScopeCapability::PublishesLookingThrough);
		if (!inst->setScopeProvider(kTag, caps)) {
			logger::warn("FRIK scope provider: setScopeProvider refused the registration"sv);
			return;
		}
		g_registered = true;
		g_lastLooking = false;
		logger::info(FMT_STRING("FRIK scope provider registered: tag {} capabilities 0x{:x} (FRIK {} API v3.{})"),
			kTag, caps, inst->getModVersion(), inst->getVersion());
		InstallMenuSink();
	}

	void PublishLookingThrough(bool a_looking)
	{
		if (!g_registered || a_looking == g_lastLooking) {
			return;
		}
		const auto* inst = FRIKApiV3::inst;
		if (!inst || !inst->setLookingThroughScope) {
			return;
		}
		if (!inst->setLookingThroughScope(kTag, a_looking)) {
			logger::warn(FMT_STRING("FRIK scope provider: setLookingThroughScope({}) refused"), a_looking);
			return;
		}
		g_lastLooking = a_looking;
		logger::info(FMT_STRING("FRIK looking-through-scope -> {}"), a_looking ? "true"sv : "false"sv);
	}

	bool Registered()
	{
		return g_registered;
	}
}
