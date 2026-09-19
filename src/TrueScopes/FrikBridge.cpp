#include "TrueScopes/FrikBridge.h"

#include "Settings/Settings.h"
#include "TrueScopes/Hooks.h"
#include "external/FRIKApiV2.h"

namespace TrueScopes::FrikBridge
{
	namespace
	{
		constexpr const char* kTag = "TrueScopes";

		// Game thread only, both of them - except g_lastLooking, which the render
		// thread reads to debounce the vanilla-gate fallback publish.
		bool             g_registered = false;
		std::atomic_bool g_lastLooking{ false };
		bool g_attempted = false;

		using frik::api::FRIKApiV2;
		using ScopeCapability = FRIKApiV2::ScopeCapability;

		// Blocking menus (Pip-Boy, terminal, ...) are tracked in Hooks for the fill
		// cadence, on both edges. The open edge also tells FRIK the eye is off
		// the tube: a blocking menu opening is where vanilla force-offs the scope
		// through the enable switch's other call sites, which we leave unhooked;
		// with the pose gate off our verdict site stops running there, so a
		// published true would stay true and FRIK would keep the wrist Pip-Boy
		// handler skipped and the scoped damping selected for as long as the
		// menu is up. The next verdict re-arms it. Game thread. Installed whether
		// or not FRIK registered - the cadence needs it either way.
		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::MenuOpenCloseEvent& a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				const char* name = a_event.menuName.c_str();
				if (name && Hooks::SetBlockingMenuOpen(name, a_event.opening) && a_event.opening) {
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
				logger::warn("UI singleton not up at kGameLoaded - blocking-menu sink not installed (menu cadence inert, FRIK not told on menu open)"sv);
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
		InstallMenuSink();
		if (!*Settings::frikProvider) {
			logger::info("FRIK scope provider: disabled by TOML (frikProvider=false) - FRIK keys its scope behaviour on ScopeMenu as before"sv);
			return;
		}
		// Minimum 1 so a FRIK without the scope calls (0.78) still initialises; the
		// scope API is v2.2, gated below on getVersion() >= 2.
		const int err = FRIKApiV2::initialize(1);
		if (err != 0) {
			// 1 = FRIK.dll not loaded, 2/3 = no v2 API, 4 = API older than asked,
			// 5 = FRIK's table smaller than the minimum requires.
			logger::info(FMT_STRING("FRIK scope provider: API v2 init returned {} ({}) - no provider registered, FRIK keys its scope behaviour on ScopeMenu as before"),
				err,
				err == 1 ? "FRIK.dll not loaded"sv :
				err == 2 || err == 3 ? "no FRIK API v2 export"sv :
				err == 4 ? "FRIK API v2 older than this build asks for"sv :
				           "FRIK API table smaller than the minimum"sv);
			return;
		}
		const auto* inst = FRIKApiV2::inst;
		if (!inst || inst->getVersion() < 2 || !inst->setScopeProvider) {
			logger::info("FRIK scope provider: FRIK API v2 present but predates the v2.2 scope calls (needs FRIK 0.79 or later) - no provider registered"sv);
			return;
		}
		// KeepsBodyVisible: our render is the main view with the body in it.
		// PublishesLookingThrough: the pose gate is the signal, not ScopeMenu.
		// Not OwnsScopeCamera: FRIK still writes the scope camera rotation and
		// our per-render translate overwrite sits on top, as it always has.
		// Not OwnsDamping: the damping decision is still open on FRIK's side.
		// PlacesScopeWidget: during a carry the widget must ride the wand of the
		// hand holding the rifle. Under the weapon the engine does not draw it at
		// all, and on the other hand's wand it hangs off a 50-80 unit lever arm
		// that turns every frame of timing slack into a floating disc (both seen
		// in the headset 2026-09-19). Ours to ask for, because the fit re-targets
		// the disc to the live glass every render under whatever parent it has.
		//
		// setScopeProvider validates the WHOLE mask - an unknown bit fails the
		// registration outright, losing the other capabilities with it - so the
		// bit is only sent to a FRIK whose contract carries it. The retry below is
		// the net for a build that reports the version but validates an older mask.
		constexpr std::uint32_t kPlacesScopeWidgetVersion = 4;
		const auto baseCaps = static_cast<std::uint32_t>(ScopeCapability::KeepsBodyVisible) |
		                      static_cast<std::uint32_t>(ScopeCapability::PublishesLookingThrough);
		const bool carryAware = inst->getVersion() >= kPlacesScopeWidgetVersion;
		auto       caps = carryAware
		                      ? baseCaps | static_cast<std::uint32_t>(ScopeCapability::PlacesScopeWidget)
		                      : baseCaps;
		if (!inst->setScopeProvider(kTag, caps)) {
			if (caps == baseCaps) {
				logger::warn("FRIK scope provider: setScopeProvider refused the registration"sv);
				return;
			}
			caps = baseCaps;
			if (!inst->setScopeProvider(kTag, caps)) {
				logger::warn("FRIK scope provider: setScopeProvider refused the registration"sv);
				return;
			}
		}
		if (caps == baseCaps) {
			logger::warn(FMT_STRING("FRIK scope provider: this FRIK (API contract {}) predates PlacesScopeWidget, which arrived in contract {}. "
			                        "A carry will re-parent the scope widget under the weapon, where the engine does not draw it: no lens while the "
			                        "weapon is carried in the other hand. Everything else is unaffected."),
				inst->getVersion(), kPlacesScopeWidgetVersion);
		}
		g_registered = true;
		g_lastLooking.store(false);
		logger::info(FMT_STRING("FRIK scope provider registered: tag {} capabilities 0x{:x} (FRIK {} API v2, contract {})"),
			kTag, caps, inst->getModVersion(), inst->getVersion());
	}

	namespace
	{
		bool Publish(bool a_looking)
		{
			if (!g_registered || a_looking == g_lastLooking.load()) {
				return false;
			}
			const auto* inst = FRIKApiV2::inst;
			if (!inst || !inst->setLookingThroughScope) {
				return false;
			}
			if (!inst->setLookingThroughScope(kTag, a_looking)) {
				logger::warn(FMT_STRING("FRIK scope provider: setLookingThroughScope({}) refused"), a_looking);
				return false;
			}
			g_lastLooking.store(a_looking);
			return true;
		}
	}

	void PublishLookingThrough(bool a_looking)
	{
		if (Publish(a_looking)) {
			logger::info(FMT_STRING("FRIK looking-through-scope -> {}"), a_looking ? "true"sv : "false"sv);
		}
	}

	void PublishLookingThrough(bool a_looking, float a_dist, float a_lateral, float a_lookDeg)
	{
		if (Publish(a_looking)) {
			logger::info(FMT_STRING("FRIK looking-through-scope -> {} (dist={:.1f} lat={:.2f} look={:.1f}deg)"),
				a_looking ? "true"sv : "false"sv, a_dist, a_lateral, a_lookDeg);
		}
	}

	bool LastLooking()
	{
		return g_lastLooking.load();
	}

	void QueuePublish(bool a_looking, const char* a_why)
	{
		if (!g_registered) {
			return;
		}
		if (auto* tasks = F4SE::GetTaskInterface()) {
			const std::string why = a_why ? a_why : "";
			tasks->AddTask([a_looking, why]() {
				if (Publish(a_looking)) {
					logger::info(FMT_STRING("FRIK looking-through-scope -> {} ({})"),
						a_looking ? "true"sv : "false"sv, why);
				}
			});
		}
	}

	void QueueStandDown()
	{
		if (!g_registered || !g_lastLooking.load()) {
			return;
		}
		if (auto* tasks = F4SE::GetTaskInterface()) {
			tasks->AddTask([]() {
				if (Publish(false)) {
					logger::info("FRIK looking-through-scope -> false (verdict site quiet: holstered or menu)"sv);
				}
			});
		}
	}

	bool OffHandGripping()
	{
		const auto* inst = FRIKApiV2::inst;
		if (!inst || !inst->isOffHandGrippingWeapon) {
			return false;
		}
		return inst->isOffHandGrippingWeapon();
	}

	bool Registered()
	{
		return g_registered;
	}

	namespace
	{
		void OnFrikMessage(F4SE::MessagingInterface::Message* a_msg)
		{
			if (!a_msg) {
				return;
			}
			using Event = FRIKApiV2::LifecycleEvent;
			const auto type = static_cast<Event>(a_msg->type);
			if (type != Event::kSkeletonDestroying && type != Event::kSkeletonReady) {
				return;
			}
			std::uint32_t generation = 0;
			if (a_msg->data && a_msg->dataLen == sizeof(FRIKApiV2::SkeletonLifecycleData)) {
				generation = static_cast<const FRIKApiV2::SkeletonLifecycleData*>(a_msg->data)->generation;
			}
			// Both edges stand the scope down: destroying because the nodes are
			// about to go, ready because what came back is a different body even
			// when the engine reused the addresses. The next live verdict re-arms.
			char reason[64];
			std::snprintf(reason, sizeof(reason), "FRIK skeleton %s (generation %u)",
				type == Event::kSkeletonDestroying ? "destroying" : "ready", generation);
			Hooks::StandDownFor(reason);
		}
	}

	void RegisterLifecycleListener()
	{
		F4SE::GetMessagingInterface()->RegisterListener(OnFrikMessage, FRIKApiV2::FRIK_F4SE_MOD_NAME);
	}
}
