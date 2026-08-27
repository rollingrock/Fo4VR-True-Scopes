#include "TrueScopes/Hooks.h"

#include "Settings/Settings.h"
#include "TrueScopes/Addresses.h"
#include "TrueScopes/ScopeIdent.h"
#include "TrueScopes/LensComposite.h"
#include "TrueScopes/PoseGate.h"
#include "TrueScopes/VerdictInput.h"
#include "TrueScopes/ScopeRender.h"

namespace TrueScopes::Hooks
{
	namespace
	{
		bool g_installed = false;
		bool g_verdictHookInstalled = false;  // pose gate owns the verdict feed

		// Plugin-owned replacement for the "scope render armed" state that vanilla keeps
		// in BSGraphics::Renderer+3. The real +3 stays 0 forever, so Main::Swap's frame
		// redirect (black main view, rebuild churn, deferred-release hazards) never
		// engages — but the enable switch still edge-triggers its show/hide block off
		// this value exactly like vanilla.
		std::atomic_bool g_scopeActive = false;
		std::atomic_bool g_gateRaw = false;                // the eye-gate's latest raw report
		std::atomic<std::uint64_t> g_frames{ 0 };          // game frame count (see Hooks.h)
		std::atomic<std::uint64_t> g_gateOffTick{ 0 };     // tick of the last true->false gate transition

		// Teardown latch. Nothing in the hooked scope path reports an unequip -
		// the verdict site just stops running, and until the staleness poll fired
		// (~1 s) the per-frame fit/probe machinery kept touching a weapon that was
		// mid-teardown from the render thread. The unequip event (game thread, at
		// the start of teardown) arms this; the verdict site running again (weapon
		// drawn + eligible = the new 3D is complete) clears it. While armed, every
		// render-thread consumer of weapon 3D stands down.
		std::atomic_bool g_teardownLatch{ false };

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
				// Gate hysteresis: the vanilla eye-gate flickers off in 200-900 ms
				// windows while aiming, and every off edge plays the widget's
				// fade-to-black over the lens. On propagates instantly; off only
				// after the gate stayed off for scopeOffHoldMs. The arm write may
				// arrive only on gate edges, so the off timestamp is taken at the
				// true->false transition and the actual deactivation is polled
				// from the per-frame fill hook.
				const bool on = a_on != 0;
				if (on) {
					g_gateRaw.store(true);
					if (!g_scopeActive.exchange(true)) {
						logger::info("scope active -> true"sv);
						// camera-smoothing reset on the scope-in edge (weapon swap
						// may reuse the same camera node, so the dt-gap heuristic
						// alone is not enough)
						ScopeRender::CamSmoothReset();
						// live-tuning loop: re-read the TOML on every scope-in
						Settings::load();
						// ...and re-identify the scope: the weapon or its mods may have
						// changed. Ordered after load() so the probe resolves against
						// the freshly parsed [Scopes] overrides.
						ScopeIdent::Request();
						if (*Settings::retryAfterFault) {
							// cap retries per session - a deterministic fault would
							// otherwise re-fault inside the engine on every scope-in.
							// Three tries separates transient from structural.
							static std::uint32_t s_faultRetries = 0;
							if (s_faultRetries < 3 && ScopeRender::RetryAfterFault()) {
								if (++s_faultRetries == 3) {
									logger::warn("fault-retry cap (3) reached - the latch will hold for the rest of the session"sv);
								}
							}
						}
					}
				} else if (g_gateRaw.load()) {
					// tick before flag: the fill hook reads flag-then-tick, so an
					// observed off must already carry this edge's timestamp — the
					// reverse order lets a poll measure the hold from a stale tick.
					g_gateOffTick.store(static_cast<std::uint64_t>(::GetTickCount64()));
					g_gateRaw.store(false);
				}
				// deliberately not writing renderer+3
			}
			static inline REL::Relocation<decltype(&thunk)> func;
		};

		// Plugin-owned widget presence: keep the widget visible while the weapon
		// is drawn without holding the enable switch on (that also keeps the
		// player sighted and the ScopeMenu open, which collapses FRIK's body and
		// blocks the Pip-Boy). The switch's show/hide is just SetAppCulled - a
		// write of flag bit 0 at node+0x108 - on three nodes: ScopeParent
		// (player+0x7d0) and the WSScopeModel singleton's +0x50/+0x68
		// (FUN_140c8e340). Presence keeps those bits cleared, refreshed after
		// every verdict call in the same call stack, while vanilla's sighted and
		// menu state follows the pose alone.
		std::atomic_bool g_presenceShown{ false };

		void SetWidgetNodesHidden(std::uintptr_t a_player, bool a_hidden)
		{
			const bool keepHousingHidden = !a_hidden && *Settings::hideWidgetHousing;
			__try {
				const auto setBit = [](std::uintptr_t a_node, bool a_hide) {
					if (a_node) {
						auto& flags = *reinterpret_cast<std::uint8_t*>(a_node + 0x108);
						if (a_hide) {
							flags |= 1;
						} else {
							flags &= static_cast<std::uint8_t>(~1u);
						}
					}
				};
				if (a_player) {
					setBit(*reinterpret_cast<std::uintptr_t*>(a_player + 0x7d0), a_hidden);
				}
				const auto model = *reinterpret_cast<std::uintptr_t*>(
					REL::Module::get().base() + Addr::kWidgetModelSingleton);
				if (model) {
					// while hideWidgetHousing is on, presence must not un-cull the
					// active-housing slot (+0x50) - HideWidgetHousing culls it
					// earlier in the same thunk and the show would run last. The
					// fade (+0x68) keeps its flow either way.
					if (!keepHousingHidden) {
						setBit(*reinterpret_cast<std::uintptr_t*>(model + 0x50), a_hidden);
					}
					setBit(*reinterpret_cast<std::uintptr_t*>(model + 0x68), a_hidden);
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			g_presenceShown.store(!a_hidden, std::memory_order_relaxed);
		}

		// housing suppression state, shared by the hide, the restore, and the
		// one-shot log site in the verdict thunk. Game-thread only. The shape
		// pointers are process-lifetime stable (the WSScopeModel clone is built
		// exactly once - see Addresses.h kWidgetModelSingleton) but are still
		// re-validated by name before every use.
		std::uintptr_t   g_housingSp = 0;
		std::uintptr_t   g_housingHunting = 0;
		std::uintptr_t   g_housingRecon = 0;
		float            g_housingSavedHunting = 0.0f;  // NIF-authored local scales,
		float            g_housingSavedRecon = 0.0f;    // captured once per process
		bool             g_housingScaleCaptured = false;
		bool             g_housingZeroLogged = false;
		std::atomic_bool g_housingZeroed{ false };    // log/DevBench visibility only
		std::atomic_bool g_housingRestored{ false };  // one-shot restore log latch
		inline constexpr float kHousingHiddenScale = 1.0e-4f;

		// Widget housing hide: cull scope_Hunting:0 and scope_recon:0 inside
		// world_scope.nif - the real weapon provides the housing, the widget only
		// needs its render surfaces. The engine re-shows these on scope-in edges,
		// so the hide runs every eligible frame, with cached node pointers
		// re-validated by name before every use. A one-shot epsilon local scale
		// makes it permanent: every engine re-show path is flags-only, the
		// placement-refresh virtual is a no-op, the nif has no controller blocks,
		// and the clone is never rebuilt, so nothing restores a zeroed scale. The
		// flag hide covers the frames before the next world-transform propagation.

		void HideWidgetHousing(std::uintptr_t a_player)
		{
			auto& s_sp = g_housingSp;
			auto& s_hunting = g_housingHunting;
			auto& s_recon = g_housingRecon;
			__try {
				const auto nameOf = [](std::uintptr_t a_node) -> const char* {
					if (!a_node) {
						return nullptr;
					}
					const auto entry = *reinterpret_cast<std::uintptr_t*>(a_node + 0x10);
					return entry >= 0x10000 ? reinterpret_cast<const char*>(entry + 0x18) : nullptr;
				};
				const auto sp = a_player ? *reinterpret_cast<std::uintptr_t*>(a_player + 0x7d0) : 0;
				if (!sp) {
					s_sp = s_hunting = s_recon = 0;
					return;
				}
				const char* hn = s_hunting ? nameOf(s_hunting) : nullptr;
				const char* rn = s_recon ? nameOf(s_recon) : nullptr;
				const bool cacheOk = sp == s_sp &&
				                     hn && std::strcmp(hn, "scope_Hunting:0") == 0 &&
				                     rn && std::strcmp(rn, "scope_recon:0") == 0;
				if (!cacheOk) {
					s_sp = sp;
					s_hunting = s_recon = 0;
					// ScopeParent -> world_scope.nif -> shapes. VR NiNode: children
					// array +0x168, loop bound u16 +0x172 (the slot high-water
					// mark NiNode::GetObjectByName 0x141c18500 iterates; null
					// holes legal). +0x174 is the non-null element count and
					// undercounts across holes - not a loop bound.
					const auto spKids = *reinterpret_cast<std::uintptr_t*>(sp + 0x168);
					const auto spCnt = *reinterpret_cast<std::uint16_t*>(sp + 0x172);
					for (std::uint16_t i = 0; spKids && i < spCnt && !s_hunting; ++i) {
						const auto root = *reinterpret_cast<std::uintptr_t*>(spKids + 8ull * i);
						const char* rootName = nameOf(root);
						if (!rootName || std::strcmp(rootName, "world_scope.nif") != 0) {
							continue;
						}
						const auto kids = *reinterpret_cast<std::uintptr_t*>(root + 0x168);
						const auto cnt = *reinterpret_cast<std::uint16_t*>(root + 0x172);
						for (std::uint16_t k = 0; kids && k < cnt; ++k) {
							const auto c = *reinterpret_cast<std::uintptr_t*>(kids + 8ull * k);
							const char* cn = nameOf(c);
							if (!cn) {
								continue;
							}
							if (std::strcmp(cn, "scope_Hunting:0") == 0) {
								s_hunting = c;
							} else if (std::strcmp(cn, "scope_recon:0") == 0) {
								s_recon = c;
							}
						}
					}
				}
				if (s_hunting) {
					*reinterpret_cast<std::uint8_t*>(s_hunting + 0x108) |= 1;
				}
				if (s_recon) {
					*reinterpret_cast<std::uint8_t*>(s_recon + 0x108) |= 1;
				}
				// permanent layer: capture the NIF-authored scales exactly once
				// (requiring both > sentinel and sane refuses a half-zeroed or
				// foreign state), then hold the sentinel. The write is
				// unconditional and idempotent - a couple of float stores per
				// eligible frame, self-healing by construction.
				if (s_hunting && s_recon && !g_housingScaleCaptured) {
					const float h = *reinterpret_cast<const float*>(s_hunting + 0x6c);
					const float r = *reinterpret_cast<const float*>(s_recon + 0x6c);
					if (h > kHousingHiddenScale && h < 1000.0f &&
						r > kHousingHiddenScale && r < 1000.0f) {
						g_housingSavedHunting = h;
						g_housingSavedRecon = r;
						g_housingScaleCaptured = true;
					}
				}
				if (g_housingScaleCaptured) {
					if (s_hunting) {
						*reinterpret_cast<float*>(s_hunting + 0x6c) = kHousingHiddenScale;
					}
					if (s_recon) {
						*reinterpret_cast<float*>(s_recon + 0x6c) = kHousingHiddenScale;
					}
					g_housingZeroed.store(true, std::memory_order_relaxed);
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				s_sp = s_hunting = s_recon = 0;
			}
		}

		// idempotent restore for a live hideWidgetHousing=false toggle: keyed on
		// memory state (cached shapes still name-valid and carrying the sentinel
		// scale), not on bookkeeping, so a fault or missed frame can never strand
		// the housing invisible against the user's setting. Flags are
		// deliberately not touched - un-culling both housings here would show the
		// inactive one too; visibility returns through the normal paths (the
		// presence un-cull next frame, or vanilla's next arm edge) which only
		// ever show the active housing.
		void RestoreWidgetHousing()
		{
			__try {
				const auto nameOf = [](std::uintptr_t a_node) -> const char* {
					if (!a_node) {
						return nullptr;
					}
					const auto entry = *reinterpret_cast<std::uintptr_t*>(a_node + 0x10);
					return entry >= 0x10000 ? reinterpret_cast<const char*>(entry + 0x18) : nullptr;
				};
				if (!g_housingScaleCaptured) {
					return;
				}
				bool        restored = false;
				const char* hn = g_housingHunting ? nameOf(g_housingHunting) : nullptr;
				const char* rn = g_housingRecon ? nameOf(g_housingRecon) : nullptr;
				if (hn && std::strcmp(hn, "scope_Hunting:0") == 0 &&
					*reinterpret_cast<const float*>(g_housingHunting + 0x6c) == kHousingHiddenScale) {
					*reinterpret_cast<float*>(g_housingHunting + 0x6c) = g_housingSavedHunting;
					restored = true;
				}
				if (rn && std::strcmp(rn, "scope_recon:0") == 0 &&
					*reinterpret_cast<const float*>(g_housingRecon + 0x6c) == kHousingHiddenScale) {
					*reinterpret_cast<float*>(g_housingRecon + 0x6c) = g_housingSavedRecon;
					restored = true;
				}
				if (restored) {
					g_housingZeroed.store(false, std::memory_order_relaxed);
					g_housingRestored.store(true, std::memory_order_relaxed);
					g_housingZeroLogged = false;  // a later re-zero logs again
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}
		// The unequip signal. Registered once at kGameDataReady via
		// TESObjectREFR_Events::RegisterForEquip; fires on the game thread from
		// the equip manager before the 3D comes down. Filtered to the player
		// unequipping the weapon the probe identified - armor, grenades and NPC
		// equips pass through.
		struct EquipEventSink : RE::BSTEventSink<RE::TESEquipEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent& a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				const auto player = *reinterpret_cast<std::uintptr_t*>(
					REL::Module::get().base() + Addr::kPlayerGlobal);
				if (!player ||
					reinterpret_cast<std::uintptr_t>(a_event.actor.get()) != player ||
					a_event.equipped) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto weap = ScopeIdent::CurrentWeaponFormID();
				if (!weap || a_event.baseObject != weap) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// Game thread; teardown starts after this returns. Stand everything
				// down now instead of a staleness-poll second from now.
				g_teardownLatch.store(true);
				g_gateRaw.store(false);
				if (g_scopeActive.exchange(false)) {
					LensComposite::RestoreReticleQuad();
				}
				ScopeIdent::InvalidateNodes();
				SetWidgetNodesHidden(player, true);
				logger::info("weapon unequipped - teardown latch armed"sv);
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		EquipEventSink g_equipSink;

		// Pose-based activation. Replaces the per-frame verdict call
		// "call FUN_140efaa60(player, verdict)" inside the vanilla eye-gate
		// (kScopeGateVerdictCallSite; mechanism in PoseGate.h). The verdict flows
		// through the untouched enable switch, whose state read/arm write are
		// already thunked above, so the widget show/hide stays edge-triggered
		// (per-tick show/hide spam exhausts the input layer and crashes) and
		// every force-off path (menus, holster, weapon change) keeps its meaning.
		struct ScopeGateVerdictHook
		{
			static void thunk(void* a_player, char a_verdict)
			{
				const auto player = reinterpret_cast<std::uintptr_t>(a_player);
				// This site running at all means the weapon is drawn and its 3D is
				// complete - the teardown latch's all-clear.
				if (g_teardownLatch.exchange(false)) {
					logger::info("teardown latch cleared (verdict site live)"sv);
				}
				const bool v = PoseGate::OnGateVerdict(player, a_verdict != 0);
				func(a_player, v ? 1 : 0);
				// Serve any pending ident probe here, on the thread that owns the
				// weapon 3D and the inventory - a probe on the render thread raced
				// game-thread teardown through the whole extra-data chain. A
				// scope-in Request lands inside the enable-switch call above, so
				// it is served in the same frame it was made.
				ScopeIdent::RunIfRequested(player);
				// Presence refresh: after the enable switch, same frame, game
				// thread — its hide-edge culled the nodes a few instructions ago
				// and this un-culls them before anything drew. This site only
				// runs while the weapon is drawn, a gun, has-scope, and no
				// blocking menu is open, so presence dies with eligibility (the
				// stale poll hides the nodes ~1 s later).
				if (*Settings::hideWidgetHousing) {
					HideWidgetHousing(player);
					if (!g_housingZeroLogged && g_housingZeroed.load(std::memory_order_relaxed)) {
						g_housingZeroLogged = true;
						logger::info(FMT_STRING("widget housing zero-scaled (hunting {:.3f} recon {:.3f} saved for restore)"),
							g_housingSavedHunting, g_housingSavedRecon);
					}
				} else {
					// put the NIF scales back when the user turns the housing back
					// on (idempotent; see RestoreWidgetHousing)
					RestoreWidgetHousing();
					if (g_housingRestored.exchange(false, std::memory_order_relaxed)) {
						logger::info("widget housing scale restored (hideWidgetHousing off)"sv);
					}
				}
				if (*Settings::poseWidgetAlways) {
					// only once the widget is presentable — the fit has been
					// applied for the current baseline (or the user disabled the
					// fit on purpose). Without this gate a first-drawn weapon
					// shows the giant un-fit band until the first aim;
					// PresenceFit() in the fill hook makes the fit land within a
					// few frames of the draw.
					if (ScopeRender::WidgetPresentable()) {
						SetWidgetNodesHidden(player, false);
					}
				} else if (g_presenceShown.load(std::memory_order_relaxed) && !v) {
					// live-toggled off while pose-inactive: put vanilla's
					// hidden state back once instead of leaving orphan nodes.
					SetWidgetNodesHidden(player, true);
				}
			}
			static inline REL::Relocation<void (*)(void*, char)> func;
		};

		// Housing show filter: the one engine site that un-hides the widget
		// housing (Addresses.h kHousingShowCallSite: the arm switch's edge-gated
		// call into WSScopeModel's show-active-housing-and-fade). Passthrough
		// first — the fade keeps vanilla flow and the arm block's later
		// shader-cache refresh (the lens RT bind gate) is untouched — then
		// re-cull the active housing while hideWidgetHousing is on.
		// Belt-and-suspenders next to the zero-scale hide; also covers a live
		// toggle before the housing node cache has first built.
		struct HousingShowFilterHook
		{
			static void thunk(void* a_model, char a_active)
			{
				func(a_model, a_active);
				if (a_model && *Settings::hideWidgetHousing) {
					Recull(reinterpret_cast<std::uintptr_t>(a_model));
				}
			}
			static void Recull(std::uintptr_t a_model)
			{
				__try {
					if (const auto h = *reinterpret_cast<std::uintptr_t*>(a_model + 0x50)) {
						*reinterpret_cast<std::uint8_t*>(h + 0x108) |= 1;
					}
				} __except (EXCEPTION_EXECUTE_HANDLER) {
				}
			}
			static inline REL::Relocation<void (*)(void*, char)> func;
		};

		// Decal group order (see Addresses.h). The resolve draws deferred-decal
		// group 5 (non-skinned: grime, posters, geometry decals) before the
		// opaque G-buffer groups, so in a resolve-only render the opaque walls
		// paint straight over them. While in the plugin's resolve (and the reorder
		// setting is on) the G5 site defers and the G6 site replays group 5
		// first, so decals land on the walls. Engine frames pass through
		// untouched. The deferred flag is assigned, not accumulated, at the G5
		// site each entry, so a live toggle or an aborted resolve can never leak
		// a stale replay into a later frame.
		std::atomic_bool g_decalG5Deferred{ false };

		struct DecalG5SiteHook
		{
			static void thunk(std::uintptr_t a_accum, std::uintptr_t a_ctx)
			{
				const bool defer = ScopeRender::InOwnResolve() && *Settings::decalGroup5Reorder;
				g_decalG5Deferred.store(defer, std::memory_order_relaxed);
				if (!defer) {
					func(a_accum, a_ctx);
				}
			}
			static inline REL::Relocation<void (*)(std::uintptr_t, std::uintptr_t)> func;
		};
		struct DecalG6SiteHook
		{
			static void thunk(std::uintptr_t a_accum, std::uintptr_t a_ctx)
			{
				if (g_decalG5Deferred.exchange(false, std::memory_order_relaxed)) {
					static REL::Relocation<void (*)(std::uintptr_t, std::uintptr_t)> g5{
						REL::Offset(Addr::kAccumDrawDecalGroup5)
					};
					g5(a_accum, a_ctx);
				}
				func(a_accum, a_ctx);
			}
			static inline REL::Relocation<void (*)(std::uintptr_t, std::uintptr_t)> func;
		};

		// Replaces the whole renderer+4 reader FUN_141d947d0 (5 bytes,
		// movzx+ret): scoped mode is answered only to the plugin's render thread
		// while the bracket is live. Every other thread — including engine jobs
		// racing the mid-frame render — sees "not scoped", so nothing can latch off the
		// transient +4=1. Vanilla never needs a true answer here: the arm write
		// is suppressed, so vanilla scoped mode never engages.
		struct ScopePassReadHook
		{
			static char thunk([[maybe_unused]] void* a_renderer)
			{
				const auto tid = ScopeRender::OwnRenderThread();
				return (tid != 0 && tid == ::GetCurrentThreadId()) ? 1 : 0;
			}
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

		// Per-frame fill: while the scope widget is active, render the mono world
		// view into the lens target every N frames (falling back to a copy of
		// the main double-wide frame). The engine's own per-draw slot-6 bind
		// displays it whenever the widget material draws.
		struct RenderFillHook
		{
			static void thunk()
			{
				// this hook is per-frame and runs regardless of scope state, so it
				// is the game's frame counter. Must stay first and unconditional —
				// an early return below would undercount and bias the measurement.
				g_frames.fetch_add(1, std::memory_order_relaxed);
				// hysteresis poll: honor a gate-off only after it persisted
				// scopeOffHoldMs; an on in between cancels it. When the pose gate
				// owns the verdict its enter/exit thresholds are the hysteresis,
				// so the time-hold applies to the vanilla gate only.
				if (g_installed && g_scopeActive.load() && !g_gateRaw.load()) {
					const auto holdMs = (g_verdictHookInstalled && *Settings::poseGateEnabled)
					                        ? 0ull
					                        : static_cast<std::uint64_t>(std::max<std::int64_t>(0, *Settings::scopeOffHoldMs));
					if (static_cast<std::uint64_t>(::GetTickCount64()) - g_gateOffTick.load() >= holdMs) {
						g_scopeActive.store(false);
						LensComposite::RestoreReticleQuad();
						logger::info("scope active -> false (held)"sv);
					}
				}
				// stale-verdict deactivation: vanilla's per-frame force-off reads
				// the real renderer+3 (always 0 here), so it never reaches this
				// state — and a mid-unequip re-arm from an unhooked equip-path
				// caller can latch the gate true for the whole holstered period,
				// after which the next draw fires no off->on edge and the TOML
				// reload plus scope re-ident silently skip. The verdict site going
				// quiet for ~1 s is the holster/menu signal (it runs per frame
				// while eligible), so force the off edge here.
				if (g_installed && g_verdictHookInstalled &&
					g_scopeActive.load() && PoseGate::SiteStale(90)) {
					g_gateRaw.store(false);
					g_scopeActive.store(false);
					LensComposite::RestoreReticleQuad();
					logger::info("scope active -> false (verdict stale: holstered or menu)"sv);
				}
				// widget presence dies with eligibility too — the verdict site
				// going quiet (holster / blocking menu) is the hide signal. Same
				// raw flag writes the show path uses; idempotent.
				if (g_installed && g_presenceShown.load(std::memory_order_relaxed) &&
					PoseGate::SiteStale(90)) {
					const auto player = *reinterpret_cast<std::uintptr_t*>(
						REL::Module::get().base() + Addr::kPlayerGlobal);
					SetWidgetNodesHidden(player, true);
					logger::info("widget presence -> hidden (verdict stale)"sv);
				}
				// controller verdict chords, polled per frame
				if (g_installed) {
					VerdictInput::Poll();
				}
				// Fallback probe site for the no-weapon-drawn case (DevBench asks
				// with the weapon holstered; the verdict site is dead then and
				// would never serve it). Gated on the site being long dead - a
				// site that stopped seconds ago is a holstered weapon, not one
				// mid-teardown - and on the latch.
				if (g_installed && !g_teardownLatch.load(std::memory_order_relaxed) &&
					PoseGate::SiteStale(300)) {
					if (const auto player = *reinterpret_cast<std::uintptr_t*>(
							REL::Module::get().base() + Addr::kPlayerGlobal)) {
						ScopeIdent::RunIfRequested(player);
					}
				}

				// while plugin-owned presence wants the widget (weapon drawn,
				// verdict site alive), keep the ident + widget fit current even
				// when no live fill runs — a save-load or first draw fits within a
				// few frames instead of waiting for the first aim. Cheap:
				// ApplyWidgetFit no-ops when nothing changed. The staleness bound
				// is tight (~150 ms): this path writes ScopeParent and walks its
				// subtree every frame, and a holstering weapon should stop it as
				// soon as the site goes quiet, not a second later. A false
				// positive from a frame hitch just pauses the fit for a frame.
				if (g_installed && *Settings::poseWidgetAlways &&
					!g_teardownLatch.load(std::memory_order_relaxed) && !PoseGate::SiteStale(15)) {
					ScopeRender::PresenceFit();
				}


				if (g_installed && *Settings::fillEnabled) {
					// last time anything filled the lens (live or prime)
					static std::uint64_t s_lastLensFillTick = 0;
					// pose freeze: while the widget is up but the pose says the eye
					// is not at the tube, the lens freezes — no fill, RT 0x62
					// persists, the last live picture stays. A one-shot dim keeps
					// stale from reading as live; thawing needs no special-case —
					// the next fill overwrites the whole delivery footprint. The
					// dim re-arms only after a live fill actually repainted the
					// lens: arming it in the widget-off branch would multiply the
					// same frozen picture by poseFrozenDim on every widget off/on
					// cycle (menu open/close x4 ~= black lens).
					static bool s_dimPending = false;
					const bool  poseLive = PoseGate::FillLive();
					if (g_scopeActive.load() && poseLive &&
						!g_teardownLatch.load(std::memory_order_relaxed)) {
						s_dimPending = true;
						static std::uint32_t frame = 0;
						if ((++frame % static_cast<std::uint32_t>(std::max<std::int64_t>(1, *Settings::fillEveryNFrames))) == 0) {
							const bool rendered =
								*Settings::lensMode >= 2 &&  // 2 = normal, 3 = G-buffer diagnostic
								ScopeRender::Available() &&
								ScopeRender::Render();
							if (!rendered && *Settings::lensMode != 0) {
								ImageSpaceCopy()(Addr::kRT_MainFrame, Addr::kRT_ScopeLens);
							}
							// a live fill supersedes any pending prime
							ScopeRender::LensPrimeDone();
							s_lastLensFillTick = ::GetTickCount64();
						}
					} else if (g_scopeActive.load() ||
					           (g_presenceShown.load(std::memory_order_relaxed) && *Settings::poseWidgetAlways)) {
						// widget up (armed, or presence-kept after the pose
						// off-edge dropped the arm) -> frozen
						//
						// Lens priming + idle refresh: nothing fills RT 0x62 until
						// the first pose activation, so the disc would sit black
						// until then. Prime = one fill when presence is up and the
						// lens has no live picture (per equip baseline), then dim
						// to the frozen look so it reads as a frozen picture, not
						// a live one. Idle refresh (default off) re-fills the
						// frozen lens every N seconds — costs one render-length
						// frame hitch per refresh, so it is opt-in. Render() is
						// scope-state independent; SiteStale keeps this out of
						// menus/holster beyond the ~1 s grace.
						if (!g_scopeActive.load() && *Settings::lensMode >= 2 &&
							!g_teardownLatch.load(std::memory_order_relaxed) &&
							!PoseGate::SiteStale(15) && ScopeRender::WidgetPresentable() &&
							ScopeRender::Available()) {
							const bool primeWanted =
								*Settings::lensPrimeOnPresence && ScopeRender::LensPrimeNeeded();
							const auto  idleS = static_cast<float>(*Settings::poseIdleRefreshSeconds);
							const bool idleWanted =
								idleS > 0.05f &&
								(::GetTickCount64() - s_lastLensFillTick) >
									static_cast<std::uint64_t>(idleS * 1000.0f);
							if ((primeWanted || idleWanted) && ScopeRender::Render()) {
								ScopeRender::LensPrimeDone();
								s_lastLensFillTick = ::GetTickCount64();
								const auto dim = std::clamp(
									static_cast<float>(*Settings::poseFrozenDim), 0.0f, 1.0f);
								if (dim < 0.999f) {
									ScopeRender::DimFrozenLens(dim);
								}
								if (primeWanted) {
									logger::info("lens primed (presence fill before first aim)"sv);
								}
							}
						}
						if (s_dimPending) {
							s_dimPending = false;
							const auto dim = static_cast<float>(*Settings::poseFrozenDim);
							if (dim < 0.999f) {
								ScopeRender::DimFrozenLens(std::clamp(dim, 0.0f, 1.0f));
							}
						}
					} else {
						// widget off: deliberately do not re-arm the dim — only a
						// live fill does (see above)
					}
				}
				func();
			}
			static inline REL::Relocation<decltype(&thunk)> func;
		};

		// The deferred resolve's two light-accum bind sites (slot 0 = 0x24/0x6a,
		// slot 1 = 0x25/0x6b) pass bind mode 0 = clear-on-apply. While the
		// plugin's resolve call is on the stack, force mode 3 (no clear) so the sun
		// BSDFLightDir pass pre-drawn into 0x6a/0x6b survives for the composite.
		// Engine frames pass through untouched.
		struct ResolveAccumBind0Hook
		{
			static void thunk(std::uintptr_t a_rtm, std::uint32_t a_slot, std::int32_t a_rt, std::uint32_t a_mode)
			{
				// the decal stage runs before the accum bind - the G-buffer set
				// is still bound (the stage reads and writes it), and its exit
				// binding is bitwise what the resolve expects here
				if (ScopeRender::InOwnResolve()) {
					ScopeRender::RunPendingDecalStage();
				}
				func(a_rtm, a_slot, a_rt, ScopeRender::InOwnResolve() ? 3 : a_mode);
				// this is the moment the resolve binds the light-accumulation
				// buffer -- after the G-buffer geometry is drawn and before the
				// light volumes. Run earlier (from Render(), before the resolve)
				// the sun shades a G-buffer that is still the black clear and
				// contributes exactly zero. No-op unless the render deferred one,
				// so engine frames are untouched.
				if (ScopeRender::InOwnResolve()) {
					ScopeRender::RunPendingSunExec();
				}
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

		// pose-gate verdict hook — the per-frame proximity verdict feed into the
		// enable switch. Non-fatal: without it poseGateEnabled is inert and the
		// vanilla eye-gate keeps deciding.
		{
			REL::Relocation<std::uintptr_t> verdictSite{ REL::Offset(Addr::kScopeGateVerdictCallSite) };
			static constexpr std::uint8_t kVerdictOrig[] = { 0xE8, 0x3C, 0x25, 0x00, 0x00 };
			if (VerifyBytes(verdictSite, { kVerdictOrig, 5 }, "eye-gate verdict site"sv)) {
				pstl::write_thunk_call<ScopeGateVerdictHook>(verdictSite.address());
				g_verdictHookInstalled = true;
				logger::info("pose-gate verdict hook installed"sv);
			} else {
				logger::warn("pose-gate verdict hook NOT installed (byte mismatch) — poseGateEnabled will be inert"sv);
			}
		}

		// housing-show filter — the one engine un-hide of the widget housing
		// (arm-switch edge). Non-fatal: without it the zero-scale hide still
		// stands; only the flag-level re-show returns.
		{
			REL::Relocation<std::uintptr_t> housingShow{ REL::Offset(Addr::kHousingShowCallSite) };
			static constexpr std::uint8_t   kHousingShowOrig[] = { 0xE8, 0x49, 0x38, 0xD9, 0xFF };
			if (VerifyBytes(housingShow, { kHousingShowOrig, 5 }, "housing show site"sv)) {
				pstl::write_thunk_call<HousingShowFilterHook>(housingShow.address());
				logger::info("housing-show filter installed"sv);
			} else {
				logger::warn("housing-show filter NOT installed (byte mismatch) — arm edges may re-show housing flags (zero-scale still holds)"sv);
			}
		}

		// decal group-order hooks (see the structs). Non-fatal: without them
		// placed decals stay overwritten in the lens, nothing else changes.
		{
			REL::Relocation<std::uintptr_t> g5site{ REL::Offset(Addr::kResolveDecalG5CallSite) };
			REL::Relocation<std::uintptr_t> g6site{ REL::Offset(Addr::kResolveDecalG6CallSite) };
			static constexpr std::uint8_t   kG5Orig[] = { 0xE8, 0x67, 0xD8, 0x01, 0x00 };
			static constexpr std::uint8_t   kG6Orig[] = { 0xE8, 0x54, 0xD9, 0x01, 0x00 };
			if (VerifyBytes(g5site, { kG5Orig, 5 }, "resolve decal G5 site"sv) &&
				VerifyBytes(g6site, { kG6Orig, 5 }, "resolve decal G6 site"sv)) {
				pstl::write_thunk_call<DecalG5SiteHook>(g5site.address());
				pstl::write_thunk_call<DecalG6SiteHook>(g6site.address());
				logger::info("decal group-order hooks installed (G5 deferred past opaque in own resolve)"sv);
			} else {
				logger::warn("decal group-order hooks NOT installed (byte mismatch)"sv);
			}
		}

		// renderer+4 reader replacement (thread-scoped scoped-mode answer).
		{
			REL::Relocation<std::uintptr_t> passReadFn{ REL::Offset(Addr::kScopePassReadFn) };
			static constexpr std::uint8_t kPassReadOrig[] = { 0x0F, 0xB6, 0x41, 0x04, 0xC3 };
			if (VerifyBytes(passReadFn, { kPassReadOrig, 5 }, "scope-pass reader fn"sv)) {
				F4SE::GetTrampoline().write_branch<5>(passReadFn.address(), ScopePassReadHook::thunk);
				logger::info("scope-pass reader hook installed (renderer+4 answered only to own render thread)"sv);
			} else {
				logger::warn("scope-pass reader hook NOT installed (byte mismatch) — concurrent +4 reads stay possible"sv);
			}
		}

		// Resolve accum-bind hooks (sun pass survival). Non-fatal: without them
		// the sun pre-draw is skipped and nothing else changes.
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

		if (*Settings::suppressScopeImods) {
			REL::Relocation<std::uintptr_t> siteA{ REL::Offset(Addr::kScopeBlackoutImodSiteA) };
			REL::Relocation<std::uintptr_t> siteB{ REL::Offset(Addr::kScopeBlackoutImodSiteB) };
			if (VerifyBytes(siteA, { kImodSiteAOrig, 5 }, "blackout imod site A"sv) &&
				VerifyBytes(siteB, { kImodSiteBOrig, 5 }, "blackout imod site B"sv)) {
				REL::safe_write(siteA.address(), kNullReturnPatch, sizeof(kNullReturnPatch));
				REL::safe_write(siteB.address(), kNullReturnPatch, sizeof(kNullReturnPatch));
				logger::info("scope blackout imod suppressed"sv);
			}
		}
		if (*Settings::hideHoldBreathHint) {
			// the "GRAB Hold Breath" pill is removed in the shipped ScopeMenu SWFs
			// (zero-scaled placement, see Data/Interface); this additionally skips
			// SetUpButtonBar in the ctor so the native bar object is never created
			static constexpr std::uint8_t kBarCallOrig[] = { 0xE8, 0x21, 0xC3, 0xF8, 0xFF };
			REL::Relocation<std::uintptr_t> siteD{ REL::Offset(Addr::kScopeMenuButtonBarCallSite) };
			if (VerifyBytes(siteD, { kBarCallOrig, 5 }, "scope button-hint bar site"sv)) {
				REL::safe_write(siteD.address(), kNopPatch, sizeof(kNopPatch));
				logger::info("hold-breath button hint suppressed"sv);
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

	void RegisterEquipSink()
	{
		static bool s_done = false;
		if (s_done) {
			return;
		}
		s_done = true;
		using Register_t = void (*)(RE::BSTEventSink<RE::TESEquipEvent>*);
		reinterpret_cast<Register_t>(
			REL::Module::get().base() + Addr::kRegisterForEquipSink)(&g_equipSink);
		logger::info("unequip event sink registered (teardown latch)"sv);
	}

	std::uint64_t FrameCount()
	{
		return g_frames.load(std::memory_order_relaxed);
	}

	bool ScopeActive()
	{
		return g_scopeActive.load();
	}

	bool WidgetPresenceShown()
	{
		return g_presenceShown.load(std::memory_order_relaxed);
	}

}
