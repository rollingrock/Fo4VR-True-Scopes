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
		// (v0.3.2's GFx _visible=false on the ScopeMenu movie was DELETED in
		// v0.3.4: the pill is scene geometry - a HUDGlassFlat quad under
		// PrimaryUIAttachNode, see SetHoldBreathPillHidden - and the movie path
		// it sticky-set never carries it. The set "succeeded" and did nothing,
		// field-proven 2026-08-26.)

		bool g_installed = false;
		bool g_verdictHookInstalled = false;  // v0.2.116: pose gate owns the verdict feed

		// Plugin-owned replacement for the "scope render armed" state that vanilla keeps
		// in BSGraphics::Renderer+3. The real +3 stays 0 forever, so Main::Swap's frame
		// redirect (black main view, rebuild churn, deferred-release hazards) never
		// engages — but the enable switch still edge-triggers its show/hide block off
		// this value exactly like vanilla.
		std::atomic_bool g_scopeActive = false;
		std::atomic_bool g_gateRaw = false;                // the eye-gate's latest raw report
		std::atomic<std::uint64_t> g_frames{ 0 };          // v0.2.70: game frame count (see Hooks.h)
		std::atomic<std::uint64_t> g_gateOffTick{ 0 };     // tick of the last true->false gate transition

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
				// v0.2.50 GATE HYSTERESIS — THE BLACK-BURST FIX. The vanilla eye-gate
				// flickers off in 200-900ms windows while aiming/moving; every OFF
				// edge plays the ScopeMenu widget's fade-to-black over the lens
				// picture area (reticle above it stays; crescent outside it stays) =
				// the "black bursts" chased through v0.2.41-49 (all render-side
				// stages proven lit by three-point GPU readback; the black was never
				// in the texture). ON propagates instantly; OFF only after the gate
				// stayed off for scopeOffHoldMs — blips are swallowed, a real
				// scope-lower still closes the widget, just a beat later.
				// v0.2.51 rework: correct whether the arm write arrives per-frame or
				// only on gate EDGES (v0.2.50 measured the hold from the last ON
				// *call* — a no-op if calls are edge-triggered). The OFF timestamp
				// is taken at the true->false TRANSITION only, and the actual
				// deactivation is polled from the per-frame fill hook.
				const bool on = a_on != 0;
				if (on) {
					g_gateRaw.store(true);
					if (!g_scopeActive.exchange(true)) {
						logger::info("scope active -> true"sv);
						// v0.2.124: deterministic camera-smoothing reset on the
						// scope-in edge (weapon swap may reuse the same camera
						// node, so the dt-gap heuristic alone is not enough).
						ScopeRender::CamSmoothReset();
						// live-tuning loop: re-read the TOML on every scope-in
						Settings::load();
						// ...and re-identify the scope: the weapon or its mods may have
						// changed since we last looked. Ordered after load() so the probe
						// resolves against the freshly parsed [Scopes] overrides.
						ScopeIdent::Request();
						if (*Settings::retryAfterFault) {
							// v0.3 hardening: cap retries per session - a DETERMINISTIC fault
							// would otherwise re-fault inside the engine on every scope-in
							// forever. Three tries separates transient from structural.
							static std::uint32_t s_faultRetries = 0;
							if (s_faultRetries < 3 && ScopeRender::RetryAfterFault()) {
								if (++s_faultRetries == 3) {
									logger::warn("fault-retry cap (3) reached - the latch will hold for the rest of the session"sv);
								}
							}
						}
					}
				} else if (g_gateRaw.exchange(false)) {
					g_gateOffTick.store(static_cast<std::uint64_t>(::GetTickCount64()));
				}
				// deliberately NOT writing renderer+3
			}
			static inline REL::Relocation<decltype(&thunk)> func;
		};

		// v0.2.118 — PLUGIN-OWNED WIDGET PRESENCE. The user wants the scope
		// widget visible the whole time the weapon is drawn (no pop-in), but
		// feeding the enable switch a perpetual "on" (v0.2.116) also kept the
		// player SIGHTED → ScopeMenu open → FRIK collapsed the body and blocked
		// the Pip-Boy (field-diagnosed 2026-08-24). The widget itself is plain
		// world geometry: the enable switch's show/hide lines are exactly
		// SetAppCulled on THREE nodes — ScopeParent (player+0x7d0) and the
		// WSScopeModel singleton's +0x50/+0x68 (FUN_140c8e340, decompiled: two
		// vfunc+0x180 calls, nothing else) — and NiAVObject::SetAppCulled is a
		// write of flag bit 0 at +0x108 (the same bit LensComposite's reticle
		// quad hide/restore has manipulated raw since v0.2.104). So presence =
		// keeping those three bits cleared ourselves, refreshed after every
		// verdict call in the same call stack (vanilla's own hide-edge can't
		// even blink it), while every piece of vanilla STATE — sighted,
		// ScopeMenu, bit 4, Pip-Boy, FRIK — follows the pose alone.
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
					// v0.2.121: while hideWidgetHousing is on, presence must NOT
					// un-cull the ACTIVE-HOUSING slot (+0x50). v0.2.120's pale-ring
					// defect was this very line: HideWidgetHousing culled the ring
					// and this un-culled it ten lines later in the same thunk, every
					// presentable frame — the plugin fought itself and the show ran
					// last. The fade (+0x68) keeps its flow either way.
					if (!keepHousingHidden) {
						setBit(*reinterpret_cast<std::uintptr_t*>(model + 0x50), a_hidden);
					}
					setBit(*reinterpret_cast<std::uintptr_t*>(model + 0x68), a_hidden);
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			g_presenceShown.store(!a_hidden, std::memory_order_relaxed);
		}

		// v0.2.120/121 — housing suppression state, shared by the hide, the
		// restore, and the one-shot log site in the verdict thunk. Game-thread
		// only. The shape pointers are process-lifetime stable (the WSScopeModel
		// clone is built exactly once — see Addresses.h kWidgetModelSingleton) but
		// are still re-validated by NAME before every use.
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

		// v0.2.120 — hide the widget model's own housing meshes (`scope_Hunting:0`
		// and `scope_recon:0` inside world_scope.nif — the pale speckled ring the
		// field screenshots showed floating on the real scope). The real weapon
		// provides the housing; the widget only needs its render surfaces. The
		// engine re-shows these on scope-in edges, so this runs every eligible
		// frame; node pointers are cached and re-validated by NAME before every
		// use (the LensComposite reticle-quad pattern — a stale pointer that still
		// reads as memory must never be trusted to be the same object).
		// v0.2.121 adds the permanent layer: a one-shot epsilon LOCAL SCALE on both
		// housing shapes ("never render"). Ghidra-proven safe: every re-show path
		// in the binary is flags-only, WSScopeModel's placement-refresh virtual is
		// a literal no-op, world_scope.nif ships zero controller blocks, and the
		// clone is never rebuilt — nothing can restore a zeroed scale. The flag
		// hide stays as belt-and-suspenders and covers the frames before the next
		// world-transform propagation.
		// v0.3.3/4 - THE HOLD-BREATH PILL, found by live scene walk 2026-08-26: a
		// HUDGlassFlat:0 text quad (+ HUDShadowFlat:0, both instanced from
		// Interface/Objects/HUDGlassFlat.nif) under an OrderedRenderingNode on a
		// point node (named "Point002" in the shipped rig) attached DIRECTLY to
		// PrimaryUIAttachNode (player+0x700) - a SIBLING of ScopeParent, which is
		// why neither the ScopeMenu movie work (v0.3.1/2) nor the widget housing
		// hide ever touched it. Matched structurally (subtree contains
		// HUDGlassFlat:0), NOT by the generic point name; the wrist HUD and menu
		// wands instance the same glass mesh, so every known sibling is skipped by
		// name first. Flags-only cull per eligible frame; cleared when the knob
		// goes off (live A/B like the housing). Left culled on scope-off by
		// design: the pill is scope-context UI, invisible unscoped either way.
		//
		// v0.3.4 - WHY v0.3.3 NEVER FOUND IT, and the engine ground truth. The
		// walk read the NiNode children fields off EVERY child; on a BSTriShape
		// leaf those offsets are garbage, the read faulted, and the function-level
		// __except silently aborted the WHOLE search - every frame, with no log
		// (the only field signal was the found-line's ABSENCE). Ground truth from
		// NiNode::GetObjectByName (0x141c18500) itself:
		//   - children base +0x168, loop bound u16 +0x172 (slot high-water mark;
		//     NULL HOLES ARE LEGAL and the engine null-skips them. +0x174 is the
		//     non-null element count - what v0.3.3 read - which UNDERCOUNTS across
		//     holes, and this rack is exactly where holes appear: quickLoot and
		//     favorites attach/detach at runtime);
		//   - per-child dispatch through vtable slot +0x188, i.e. the engine's
		//     own recursion gate IS the ScopeIdent::IsNiNode test (exact per the
		//     2026-08-26 sweep: all 20 NiNode-family classes share the slot,
		//     nothing overrides it).
		// So v0.3.4 recurses only into real NiNodes, uses the engine's bound,
		// SEH-isolates each candidate scan (one bad subtree costs one sibling,
		// never the search), and LOGS a persistent miss or fault one-shot -
		// silence is no longer an outcome.
		std::uintptr_t g_pillNode = 0;
		std::uint32_t  g_pillMissScans = 0;   // consecutive find-loop misses; reset on find
		bool           g_pillMissLogged = false;
		float          g_pillSavedScale = 0.0f;  // engine-authored local scale (+0x6c), captured once
		bool           g_pillScaleCaptured = false;
		// v0.3.6 - rack-slot hole state (pillKillMode 2). The children array and
		// slot index the carrier was found at; both re-validated against live
		// memory before every poke (the array can be reallocated by the engine).
		std::uintptr_t g_pillRackKids = 0;
		std::uint16_t  g_pillRackSlot = 0;
		bool           g_pillHoled = false;
		bool           g_pillCensusDone = false;

		bool SubtreeHasHudGlass(std::uintptr_t a_node, int a_depth)
		{
			if (!a_node || a_depth > 2) {
				return false;
			}
			const auto entry = *reinterpret_cast<std::uintptr_t*>(a_node + 0x10);
			if (entry >= 0x10000 &&
				std::strcmp(reinterpret_cast<const char*>(entry + 0x18), "HUDGlassFlat:0") == 0) {
				return true;
			}
			if (!ScopeIdent::IsNiNode(a_node)) {
				// leaf (BSTriShape etc.): +0x168/+0x172 are not children fields
				// here - reading them was the v0.3.3 fault
				return false;
			}
			const auto kids = *reinterpret_cast<std::uintptr_t*>(a_node + 0x168);
			const auto cnt = *reinterpret_cast<std::uint16_t*>(a_node + 0x172);
			for (std::uint16_t i = 0; kids && i < cnt && i < 32; ++i) {
				const auto c = *reinterpret_cast<std::uintptr_t*>(kids + 8ull * i);
				if (c && SubtreeHasHudGlass(c, a_depth + 1)) {
					return true;
				}
			}
			return false;
		}

		// SEH isolation per candidate: one surprising subtree must cost ONE
		// sibling's scan, never the whole search (v0.3.3's function-level handler
		// turned a single bad read into a silent permanent miss). Tri-state so a
		// faulting subtree is DISTINGUISHABLE from a genuinely glass-free one in
		// the rack diagnostics: 1 = carries the glass, 0 = does not, -1 = FAULTED.
		int SubtreeHasHudGlassSafe(std::uintptr_t a_node)
		{
			__try {
				return SubtreeHasHudGlass(a_node, 0) ? 1 : 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}

		// --- v0.3.6 pill census (diagnostic; Settings::pillCensus) ---------------
		// Both v0.3.4 (cull flag) and v0.3.5 (epsilon local scale) left the pill
		// rendering, so either the carrier's world transform is engine-written per
		// frame (parent scale moot) or the latched node is not the pill at all.
		// This one-shot walk settles WHERE the pill actually is: climb from the
		// wand rack to the scene root, then sweep the whole tree logging every
		// HUD/Hint/Glass/Breath/Button-named node with world position, world scale
		// and distance to ScopeParent. A floating quad next to the scope cannot
		// hide from a position sweep, whichever subtree it hangs in.

		// bounded copy of a raw engine name: never walks past kMax (the review's
		// %.48s lesson - an unterminated name must not fault the instrument)
		void PillCopyName(const char* a_src, char (&a_out)[49])
		{
			int k = 0;
			for (; k < 48; ++k) {
				const char ch = a_src[k];
				if (!ch) {
					break;
				}
				a_out[k] = ch;
			}
			a_out[k] = '\0';
		}

		bool PillNameMatches(const char* a_boundedName)
		{
			static constexpr const char* kWords[] = { "hud", "hint", "glass", "breath", "hold", "button" };
			for (const char* w : kWords) {
				for (const char* p = a_boundedName; *p; ++p) {
					const char* a = p;
					const char* b = w;
					while (*a && *b) {
						const char lc = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
						if (lc != *b) {
							break;
						}
						++a;
						++b;
					}
					if (!*b) {
						return true;
					}
				}
			}
			return false;
		}

		// is a_p a node whose children array contains a_node? SEH-isolated so one
		// garbage candidate costs one probe, never the climb.
		bool PillIsParentOf(std::uintptr_t a_p, std::uintptr_t a_node)
		{
			__try {
				if (!a_p || a_p < 0x10000 || (a_p & 7) != 0 || !ScopeIdent::IsNiNode(a_p)) {
					return false;
				}
				const auto kids = *reinterpret_cast<std::uintptr_t*>(a_p + 0x168);
				const auto cnt = *reinterpret_cast<std::uint16_t*>(a_p + 0x172);
				for (std::uint16_t i = 0; kids && i < cnt && i < 512; ++i) {
					if (*reinterpret_cast<std::uintptr_t*>(kids + 8ull * i) == a_node) {
						return true;
					}
				}
				return false;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// EMPIRICAL parent discovery: the VR NiAVObject parent offset is not
		// pinned ground truth, so instead of trusting one, a candidate qword IS
		// the parent iff its children array contains this node - self-validating
		// per use, no new static assumption.
		std::uintptr_t PillGuessParent(std::uintptr_t a_node)
		{
			static constexpr std::uintptr_t kTries[] = { 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50 };
			for (const auto off : kTries) {
				std::uintptr_t p = 0;
				__try {
					p = *reinterpret_cast<std::uintptr_t*>(a_node + off);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					p = 0;
				}
				if (p && PillIsParentOf(p, a_node)) {
					return p;
				}
			}
			return 0;
		}

		void PillCensusWalk(std::uintptr_t a_node, int a_depth, std::uint32_t& a_visited,
			std::uint32_t& a_hits, const float (&a_scope)[3], bool a_haveScope)
		{
			if (!a_node || a_depth > 16 || a_visited >= 6000 || a_hits >= 40) {
				return;
			}
			++a_visited;
			const auto entry = *reinterpret_cast<std::uintptr_t*>(a_node + 0x10);
			if (entry >= 0x10000) {
				char nb[49];
				PillCopyName(reinterpret_cast<const char*>(entry + 0x18), nb);
				if (nb[0] && PillNameMatches(nb)) {
					++a_hits;
					const float x = *reinterpret_cast<float*>(a_node + 0xa0);
					const float y = *reinterpret_cast<float*>(a_node + 0xa4);
					const float z = *reinterpret_cast<float*>(a_node + 0xa8);
					const float ws = *reinterpret_cast<float*>(a_node + 0xac);
					float d = -1.0f;
					if (a_haveScope) {
						const float dx = x - a_scope[0], dy = y - a_scope[1], dz = z - a_scope[2];
						d = std::sqrt(dx * dx + dy * dy + dz * dz);
					}
					logger::info(FMT_STRING("pillcensus: '{}' node=0x{:x} wpos=({:.1f},{:.1f},{:.1f}) wscale={:.4f} d2scope={:.1f}"),
						nb, a_node, x, y, z, ws, d);
				}
			}
			if (!ScopeIdent::IsNiNode(a_node)) {
				return;
			}
			const auto kids = *reinterpret_cast<std::uintptr_t*>(a_node + 0x168);
			const auto cnt = *reinterpret_cast<std::uint16_t*>(a_node + 0x172);
			if (cnt > 512) {
				return;
			}
			for (std::uint16_t i = 0; kids && i < cnt; ++i) {
				const auto c = *reinterpret_cast<std::uintptr_t*>(kids + 8ull * i);
				if (c) {
					PillCensusWalk(c, a_depth + 1, a_visited, a_hits, a_scope, a_haveScope);
				}
			}
		}

		void PillCensusRun(std::uintptr_t a_player)
		{
			if (g_pillCensusDone) {
				return;
			}
			g_pillCensusDone = true;
			__try {
				const auto rig = a_player ? *reinterpret_cast<std::uintptr_t*>(a_player + 0x700) : 0;
				if (!rig) {
					logger::warn("pillcensus: no rig - aborted"sv);
					return;
				}
				float scope[3] = {};
				bool  haveScope = false;
				const auto sp = *reinterpret_cast<std::uintptr_t*>(a_player + 0x7d0);
				if (sp) {
					scope[0] = *reinterpret_cast<float*>(sp + 0xa0);
					scope[1] = *reinterpret_cast<float*>(sp + 0xa4);
					scope[2] = *reinterpret_cast<float*>(sp + 0xa8);
					haveScope = true;
				}
				std::uintptr_t root = rig;
				char           chain[384];
				int            cl = 0;
				int            hops = 0;
				for (; hops < 24; ++hops) {
					const auto p = PillGuessParent(root);
					if (!p) {
						break;
					}
					root = p;
					const auto pe = *reinterpret_cast<std::uintptr_t*>(p + 0x10);
					char nb[49];
					nb[0] = '\0';
					if (pe >= 0x10000) {
						PillCopyName(reinterpret_cast<const char*>(pe + 0x18), nb);
					}
					const int n = std::snprintf(chain + cl, sizeof(chain) - cl, " <- '%s'", nb[0] ? nb : "(unnamed)");
					if (n > 0 && cl + n < static_cast<int>(sizeof(chain))) {
						cl += n;
					}
				}
				chain[cl] = '\0';
				logger::info(FMT_STRING("pillcensus: root=0x{:x} after {} hops: rig{}"), root, hops, chain);
				std::uint32_t visited = 0, hits = 0;
				PillCensusWalk(root, 0, visited, hits, scope, haveScope);
				logger::info(FMT_STRING("pillcensus: done visited={} hits={} scope=({:.1f},{:.1f},{:.1f})"),
					visited, hits, scope[0], scope[1], scope[2]);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("pillcensus: FAULTED (any output above is partial)"sv);
			}
		}

		void SetHoldBreathPillHidden(std::uintptr_t a_player, bool a_hidden)
		{
			__try {
				const auto nameOf = [](std::uintptr_t a_node) -> const char* {
					if (!a_node) {
						return nullptr;
					}
					const auto entry = *reinterpret_cast<std::uintptr_t*>(a_node + 0x10);
					return entry >= 0x10000 ? reinterpret_cast<const char*>(entry + 0x18) : nullptr;
				};
				const auto rig = a_player ? *reinterpret_cast<std::uintptr_t*>(a_player + 0x700) : 0;
				if (!rig) {
					g_pillNode = 0;
					return;
				}
				// cache is valid only while the node still carries the HUD glass subtree
				if (g_pillNode && SubtreeHasHudGlassSafe(g_pillNode) != 1) {
					if (g_pillHoled) {
						// the node died while detached: restoring a dead pointer
						// into the rack is worse than leaking one small node
						g_pillHoled = false;
						logger::warn("pill node invalidated while holed - restore abandoned (one rack node leaked)"sv);
					}
					g_pillNode = 0;
				}
				if (!g_pillNode) {
					// rack snapshot for the miss log: 'name'(verdict) per sibling.
					// %.48s BOUNDS THE READ (C11 7.21.6.1: a precision stops the
					// scan even with no NUL) - an unterminated engine name that
					// passes the head checks must not let the DIAGNOSTIC re-create
					// the v0.3.3 whole-search abort. Truncation is marked, never
					// silent: a partial rack that looks complete misleads exactly
					// the investigation this log exists to serve.
					char rack[384];
					int  rackLen = 0;
					bool rackFull = false;
					const auto note = [&rack, &rackLen, &rackFull](const char* a_name, const char* a_verdict) {
						if (rackFull) {
							return;
						}
						const int n = std::snprintf(rack + rackLen, sizeof(rack) - rackLen,
							"'%.48s'(%s) ", a_name, a_verdict);
						if (n > 0 && rackLen + n < static_cast<int>(sizeof(rack))) {
							rackLen += n;
						} else {
							rackFull = true;
						}
					};
					const auto kids = *reinterpret_cast<std::uintptr_t*>(rig + 0x168);
					const auto cnt = *reinterpret_cast<std::uint16_t*>(rig + 0x172);
					for (std::uint16_t i = 0; kids && i < cnt && i < 32; ++i) {
						const auto c = *reinterpret_cast<std::uintptr_t*>(kids + 8ull * i);
						if (!c) {
							continue;  // null hole - legal, the engine null-skips too
						}
						const char* cn = nameOf(c);
						if (!cn) {
							note("(unnamed)", "?");
							continue;
						}
						// the wrist HUD and menu wands instance the same glass
						// mesh - skip every known sibling by name and only then
						// match structurally
						if (std::strcmp(cn, "ScopeParent") == 0 ||
							std::strcmp(cn, "world_primaryWand.nif") == 0 ||
							std::strncmp(cn, "favorites", 9) == 0 ||
							std::strncmp(cn, "ws_", 3) == 0 ||
							std::strncmp(cn, "world_", 6) == 0) {
							note(cn, "skip");
							continue;
						}
						const int v = SubtreeHasHudGlassSafe(c);
						if (v == 1) {
							g_pillNode = c;
							g_pillRackKids = kids;  // for pillKillMode 2 (re-validated per poke)
							g_pillRackSlot = i;
							g_pillMissScans = 0;  // the counter measures CONSECUTIVE misses
							static bool s_foundLogged = false;
							if (!s_foundLogged) {
								s_foundLogged = true;
								logger::info(FMT_STRING("hold-breath pill node found: '{}' (slot {} of {}) under PrimaryUIAttachNode"), cn, i, cnt);
							}
							break;
						}
						if (v < 0) {
							// a per-candidate fault is the v0.3.3 failure class -
							// isolated now, but it must never masquerade as a
							// benign "no-glass" verdict in the diagnostics
							static bool s_candFaultLogged = false;
							if (!s_candFaultLogged) {
								s_candFaultLogged = true;
								logger::warn(FMT_STRING("pill scan: subtree of '{}' faults (isolated; sibling skipped)"), cn);
							}
						}
						note(cn, v < 0 ? "FAULT" : "no-glass");
					}
					if (!g_pillNode) {
						// a persistent miss must be VISIBLE: v0.3.3's only field
						// signal was an absent log line. ~120 CONSECUTIVE eligible
						// frames is a scoped session well past rack construction
						// (transient pre-attach misses reset on every find).
						if (a_hidden && !g_pillMissLogged && ++g_pillMissScans >= 120) {
							g_pillMissLogged = true;
							rack[rackLen] = '\0';
							logger::warn(FMT_STRING("hold-breath pill NOT found after {} scans; PrimaryUIAttachNode rack ({} slots{}): {}{}"),
								g_pillMissScans, cnt, cnt > 32 ? ", scan capped at 32" : "", rack, rackFull ? "...(truncated)" : "");
						}
					}
				}
				if (g_pillNode) {
					// v0.3.6 - pillKillMode 2: RACK-SLOT HOLE, the engine's own hide
					// idiom for wand-rack UI (null holes are legal per the decompiled
					// NiNode::GetObjectByName). A node not in the tree cannot draw,
					// whoever owns its transform - the discriminator flags and scale
					// could not be. The slot is only ever poked when the live array
					// still matches what the find recorded; restore only into a slot
					// that is still null. Live-settable for the in-headset A/B.
					const bool wantHole = a_hidden && *Settings::pillKillMode == 2;
					const auto liveKids = *reinterpret_cast<std::uintptr_t*>(rig + 0x168);
					if (g_pillHoled && !wantHole) {
						if (liveKids == g_pillRackKids &&
							*reinterpret_cast<std::uintptr_t*>(g_pillRackKids + 8ull * g_pillRackSlot) == 0) {
							*reinterpret_cast<std::uintptr_t*>(g_pillRackKids + 8ull * g_pillRackSlot) = g_pillNode;
							g_pillHoled = false;
							logger::info("pill rack slot restored"sv);
						} else {
							g_pillHoled = false;
							logger::warn("pill rack slot changed while holed - restore abandoned"sv);
						}
					} else if (wantHole && !g_pillHoled) {
						if (liveKids == g_pillRackKids &&
							*reinterpret_cast<std::uintptr_t*>(g_pillRackKids + 8ull * g_pillRackSlot) == g_pillNode) {
							*reinterpret_cast<std::uintptr_t*>(g_pillRackKids + 8ull * g_pillRackSlot) = 0;
							g_pillHoled = true;
							static bool s_holeLogged = false;
							if (!s_holeLogged) {
								s_holeLogged = true;
								logger::info(FMT_STRING("pill rack slot {} holed (pillKillMode 2)"), g_pillRackSlot);
							}
						}
					}
					auto* flags = reinterpret_cast<std::uint8_t*>(g_pillNode + 0x108);
					auto* scale = reinterpret_cast<float*>(g_pillNode + 0x6c);
					if (a_hidden) {
						*flags |= 1;
						// v0.3.5 - FIELD RESULT 2026-08-26: the flag alone does NOT
						// hide the pill (node found, bit set every eligible frame,
						// pill still rendered - chord "no"). The wand rack's own
						// visibility idiom is attach/detach (hence the null holes),
						// so its draw path plausibly never consults AppCulled - or
						// re-shows after our hook. The housing answer (v0.2.121) is
						// ordering-immune: epsilon LOCAL scale -> degenerate quad,
						// invisible to every render path that consumes world
						// transforms, regardless of who wins the flag fight.
						// Captured once (sanity-bounded), re-applied idempotently
						// per eligible frame, restored live on knob-off. The flag
						// stays as belt-and-suspenders.
						if (!g_pillScaleCaptured) {
							const float s = *scale;
							if (s > kHousingHiddenScale && s < 1000.0f) {
								g_pillSavedScale = s;
								g_pillScaleCaptured = true;
							}
						}
						if (g_pillScaleCaptured) {
							*scale = kHousingHiddenScale;
							static bool s_zeroLogged = false;
							if (!s_zeroLogged) {
								s_zeroLogged = true;
								logger::info(FMT_STRING("hold-breath pill zero-scaled (saved {:.3f} for restore)"), g_pillSavedScale);
							}
						}
					} else {
						*flags &= static_cast<std::uint8_t>(~1u);
						// restore keyed on MEMORY state (sentinel still present), the
						// RestoreWidgetHousing discipline - a missed frame can never
						// strand the pill invisible against the user's setting
						if (g_pillScaleCaptured && *scale == kHousingHiddenScale) {
							*scale = g_pillSavedScale;
						}
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				g_pillNode = 0;
				static bool s_faultLogged = false;
				if (!s_faultLogged) {
					s_faultLogged = true;
					logger::warn("hold-breath pill search faulted (one-shot log; the search retries every eligible frame)"sv);
				}
			}
		}

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
					// ScopeParent -> world_scope.nif -> shapes (VR NiNode: children
					// +0x168, loop bound u16 +0x172 - the slot high-water mark
					// NiNode::GetObjectByName itself iterates, null holes legal.
					// v0.3.4: was +0x174 (non-null element count), which walked
					// short of any child past a hole; both agree on a contiguous
					// array, which is why the 2026-08-24 live check passed.)
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
				// v0.2.121 — the permanent layer (see the function comment). Capture
				// the NIF-authored scales exactly once (they never change; requiring
				// both > sentinel and sane refuses a half-zeroed or foreign state),
				// then hold the sentinel. The write is unconditional and idempotent:
				// a couple of float stores per eligible frame, self-healing by
				// construction.
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

		// v0.2.121 — idempotent restore for a live hideWidgetHousing=false toggle:
		// keyed on MEMORY state (cached shapes still name-valid AND carrying the
		// sentinel scale), not on our own bookkeeping, so a fault or missed frame
		// can never strand the housing invisible against the user's setting. Flags
		// are deliberately NOT touched: un-culling both housings here would show
		// the INACTIVE one too; visibility returns through the normal paths (the
		// presence un-cull next frame, or vanilla's next arm edge) which only ever
		// show the active housing.
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
		// v0.2.116 — POSE-BASED ACTIVATION. Replaces the per-frame verdict call
		// "call FUN_140efaa60(player, verdict)" inside the vanilla eye-gate
		// (kScopeGateVerdictCallSite; mechanism in PoseGate.h). Our verdict flows
		// through the untouched enable switch, whose state read/arm write are
		// already thunked above — so the widget show/hide stays edge-triggered
		// (single-fire; the v0.1.0 per-tick-spam crash class cannot recur) and
		// every force-off path (menus, holster, weapon change) keeps its meaning.
		struct ScopeGateVerdictHook
		{
			static void thunk(void* a_player, char a_verdict)
			{
				const auto player = reinterpret_cast<std::uintptr_t>(a_player);
				const bool v = PoseGate::OnGateVerdict(player, a_verdict != 0);
				func(a_player, v ? 1 : 0);
				// Presence refresh (v0.2.118): AFTER the enable switch, same
				// frame, game thread — its hide-edge culled the nodes a few
				// instructions ago and this un-culls them before anything drew.
				// This site only runs while the weapon is drawn, a gun,
				// has-scope, and no blocking menu is open, so presence dies
				// with eligibility (the stale poll hides the nodes ~1 s later).
				SetHoldBreathPillHidden(player, *Settings::hideHoldBreathHint);
				// v0.3.6 diagnostic: one-shot scene census once the scope is truly
				// up (the pill exists by then; see Settings::pillCensus)
				if (*Settings::pillCensus && g_scopeActive.load(std::memory_order_relaxed)) {
					PillCensusRun(player);
				}
				if (*Settings::hideWidgetHousing) {
					HideWidgetHousing(player);
					if (!g_housingZeroLogged && g_housingZeroed.load(std::memory_order_relaxed)) {
						g_housingZeroLogged = true;
						logger::info(FMT_STRING("widget housing zero-scaled (hunting {:.3f} recon {:.3f} saved for restore)"),
							g_housingSavedHunting, g_housingSavedRecon);
					}
				} else {
					// v0.2.121: live A/B — put the NIF scales back when the user
					// turns the housing back on (idempotent; see the function).
					RestoreWidgetHousing();
					if (g_housingRestored.exchange(false, std::memory_order_relaxed)) {
						logger::info("widget housing scale restored (hideWidgetHousing off)"sv);
					}
				}
				if (*Settings::poseWidgetAlways) {
					// v0.2.119: only once the widget is PRESENTABLE — the fit has
					// been applied for the current baseline (or the user disabled
					// the fit on purpose). Without this gate a first-drawn weapon
					// showed Bethesda's giant un-fit band until the first aim
					// (field screenshot 20260824151656). PresenceFit() in the fill
					// hook makes the fit land within a few frames of the draw.
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

		// v0.2.121 — HOUSING SHOW FILTER. The ONE engine site that un-hides the
		// widget housing (Addresses.h kHousingShowCallSite: the arm switch's
		// edge-gated call into WSScopeModel's show-active-housing-and-fade).
		// Passthrough first — the fade keeps vanilla flow and the arm block's
		// later shader-cache refresh (the lens RT bind gate) is untouched — then
		// re-cull the ACTIVE housing while hideWidgetHousing is on. With the
		// zero-scale hide this is belt-and-suspenders; it also covers a live
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

		// v0.2.132 - DECAL GROUP ORDER FIX (Ghidra 2026-08-25; see Addresses.h).
		// The resolve draws deferred-decal group 5 (non-skinned: grime, posters,
		// geometry decals) BEFORE the opaque G-buffer groups, so in our
		// resolve-only render the opaque walls paint straight over them. While
		// g_inOwnResolve (and the reorder setting is on) the G5 site defers and
		// the G6 site replays group 5 first - decals land ON the walls, exactly
		// where vanilla's frame ordering effectively puts them. Engine frames
		// pass through untouched. The deferred flag is ASSIGNED (not
		// accumulated) at the G5 site each entry, so a live toggle or an
		// aborted resolve can never leak a stale replay into a later frame.
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

		// Replaces the WHOLE renderer+4 reader FUN_141d947d0 (5 bytes, movzx+ret):
		// scoped mode is answered only to our render thread while our bracket is
		// live. Every other thread — including engine jobs racing our mid-frame
		// render — sees "not scoped", so no phantom scoped-path actions can latch
		// off our transient +4=1 (black-burst suspect #5). Vanilla never needs a
		// true answer here: the arm write is suppressed, so vanilla scoped mode
		// never engages.
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

		// Phase 1 fill: while the scope widget is active, copy the main double-wide
		// frame into the lens target every N frames. The engine's own per-draw slot-6
		// bind displays it whenever the widget material draws. Phase 2 replaces the Copy
		// with our own mono world render from PrimaryWeaponScopeCamera into a temp RT,
		// then Copy(temp, 0x62) (recipe: ROUTE_B_STATIC_MAP_2026-08-06.md section 3.2).
		struct RenderFillHook
		{
			static void thunk()
			{
				// v0.2.70 perf instrument: this hook is per-frame and runs regardless of
				// scope state, so it is the game's frame counter. Must stay first and
				// unconditional — an early return below would otherwise undercount and
				// silently bias the very measurement it exists to make.
				g_frames.fetch_add(1, std::memory_order_relaxed);
				// v0.2.51 hysteresis poll (runs every frame): honor a gate-OFF only
				// after it persisted scopeOffHoldMs; an ON in between cancels it.
				// v0.2.116: when the pose gate owns the verdict, its enter/exit
				// threshold pairs ARE the hysteresis, and a time-hold here would
				// just replay the enable switch's off-block every frame for the
				// hold window — the time-hold applies to the vanilla gate only.
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
				// v0.2.117 — stale-verdict deactivation (review finding). Vanilla's
				// per-frame force-off reads the REAL renderer+3 (always 0 for us),
				// so it never reaches our state — and a mid-unequip re-arm from an
				// unhooked equip-path caller can latch g_gateRaw+g_scopeActive true
				// for the whole holstered period. Then the next draw fires no
				// OFF->ON edge and the TOML reload + scope re-ident silently skip.
				// The verdict site going quiet for ~1 s IS the holster/menu signal
				// (it runs per frame while eligible), so force the off edge here.
				if (g_installed && g_verdictHookInstalled && *Settings::poseGateEnabled &&
					g_scopeActive.load() && PoseGate::VerdictStale(90)) {
					g_gateRaw.store(false);
					g_scopeActive.store(false);
					LensComposite::RestoreReticleQuad();
					logger::info("scope active -> false (verdict stale: holstered or menu)"sv);
				}
				// v0.2.118: widget presence dies with eligibility too — the verdict
				// site going quiet (holster / blocking menu) is the hide signal.
				// Same raw flag writes the show path uses; idempotent.
				if (g_installed && g_presenceShown.load(std::memory_order_relaxed) &&
					PoseGate::SiteStale(90)) {
					const auto player = *reinterpret_cast<std::uintptr_t*>(
						REL::Module::get().base() + Addr::kPlayerGlobal);
					SetWidgetNodesHidden(player, true);
					logger::info("widget presence -> hidden (verdict stale)"sv);
				}
				// v0.2.109: controller verdict chords, polled per frame.
				if (g_installed) {
					VerdictInput::Poll();
				}
				// v0.2.106: run a pending scope-ident probe from the per-frame hook,
				// not only from RenderImpl. RenderImpl runs only while the scope is
				// RAISED, so headless (null driver, no scope-in ever) a /scope?probe=1
				// waited its 3 s and returned last cycle's latched answer — the
				// "probed:false, weapon 0x0" trap hit on 2026-08-23. This thread IS
				// the render thread the probe expects, the request flag makes the two
				// call sites idempotent, and an un-requested frame costs one atomic
				// read. Makes the whole ident chain testable with no headset AND no
				// scope raise: additem + equipitem + /scope?probe=1.
				if (g_installed) {
					if (const auto player = *reinterpret_cast<std::uintptr_t*>(
							REL::Module::get().base() + Addr::kPlayerGlobal)) {
						ScopeIdent::RunIfRequested(player);
					}
				}

				// v0.2.119: while plugin-owned presence wants the widget (weapon
				// drawn, verdict site alive), keep the ident + widget fit current
				// even when no live fill runs — a save-load or first draw fits
				// within a few frames instead of waiting for the first aim.
				// Cheap: ApplyWidgetFit no-ops when nothing changed.
				if (g_installed && *Settings::poseWidgetAlways && !PoseGate::SiteStale(90)) {
					ScopeRender::PresenceFit();
				}


				if (g_installed && *Settings::fillEnabled) {
					// v0.2.125: last time anything filled the lens (live or prime).
					static std::uint64_t s_lastLensFillTick = 0;
					// v0.2.116 — POSE FREEZE. While the widget is up but the pose
					// says the eye is not at the tube, the lens FREEZES: no fill,
					// RT 0x62 persists, so the last live picture stays. A one-shot
					// dim keeps stale from reading as live; thawing needs no
					// special-case — the next fill overwrites the whole delivery
					// footprint. v0.2.117 (review finding): the dim re-arms ONLY
					// after a live fill actually repainted the lens — arming it in
					// the widget-off branch let every widget off/on cycle while
					// still frozen multiply the same picture by poseFrozenDim
					// (menu open/close x4 ~= black lens).
					static bool s_dimPending = false;
					const bool  poseLive = PoseGate::FillLive();
					if (g_scopeActive.load() && poseLive) {
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
							// v0.2.125: a live fill supersedes any pending prime.
							ScopeRender::LensPrimeDone();
							s_lastLensFillTick = ::GetTickCount64();
						}
					} else if (g_scopeActive.load() ||
					           (g_presenceShown.load(std::memory_order_relaxed) && *Settings::poseWidgetAlways)) {
						// widget up (armed, or presence-kept after the pose
						// off-edge dropped the arm — v0.2.118) -> frozen
						//
						// v0.2.125 — LENS PRIMING + IDLE REFRESH. Field 21:07: the
						// placement chain converged 314 ms after load, but nothing
						// ever fills RT 0x62 until the first pose activation, so
						// the disc sat BLACK for 33 s ("didn't see the lens
						// start"). Prime = one fill when presence is up and the
						// lens has no live picture (per equip baseline), then dim
						// to the frozen look so it reads as a frozen picture, not
						// a live one. Idle refresh (default off) re-fills the
						// frozen lens every N seconds — costs one render-length
						// frame hitch per refresh, so it is an opt-in A/B knob.
						// Render() is scope-state independent (verified: no
						// g_scopeActive reads in ScopeRender.cpp); SiteStale keeps
						// this out of menus/holster beyond the ~1 s grace.
						if (!g_scopeActive.load() && *Settings::lensMode >= 2 &&
							!PoseGate::SiteStale(90) && ScopeRender::WidgetPresentable() &&
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
						// widget off: deliberately do NOT re-arm the dim — only a
						// live fill does (see the v0.2.117 note above).
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
				// v0.2.133: the decal stage runs BEFORE the accum bind - the
				// G-buffer set is still bound (the stage reads AND writes it),
				// and its exit binding is bitwise what the resolve expects here.
				if (ScopeRender::InOwnResolve()) {
					ScopeRender::RunPendingDecalStage();
				}
				func(a_rtm, a_slot, a_rt, ScopeRender::InOwnResolve() ? 3 : a_mode);
				// v0.2.78: this is the moment the resolve binds the light-accumulation
				// buffer -- AFTER the G-buffer geometry is drawn and BEFORE the light
				// volumes. That ordering is the entire fix for §3.1: run from Render()
				// (before the resolve) the sun shades a G-buffer that is still the black
				// clear, so it contributes exactly zero. No-op unless the render deferred
				// one, so engine frames and the old placement are untouched.
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

		// v0.2.116: pose-gate verdict hook — the per-frame proximity verdict feed
		// into the enable switch. Non-fatal: without it poseGateEnabled is inert
		// and the vanilla eye-gate keeps deciding (v0.2.115 behavior).
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

		// v0.2.121: housing-show filter — the one engine un-hide of the widget
		// housing (arm-switch edge). Non-fatal: without it the zero-scale hide
		// still stands; only the flag-level re-show returns.
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

		// v0.2.132: decal group-order hooks (see the structs). Non-fatal: without
		// them placed decals stay overwritten in the lens, nothing else changes.
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

		// v0.2.48: renderer+4 reader replacement (thread-scoped scoped-mode answer).
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
			// v0.3.1 - the "GRAB Hold Breath" pill is the ScopeMenu movie's
			// ButtonHintBar; skipping SetUpButtonBar in the ctor is the standard
			// no-bar menu configuration (see Addr::kScopeMenuButtonBarCallSite).
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
