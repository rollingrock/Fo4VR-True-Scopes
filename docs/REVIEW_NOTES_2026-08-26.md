# Review notes - 2026-08-26 style pass

Status 2026-08-27: the correctness review ran (22 findings, 16 confirmed after
adversarial verification); 15 fixes are applied in v0.3.13. Deferred with its
own future field test: the scopeOffHoldMs gate rework (making the vanilla-
visible state read the raw gate so an on-edge during the hold cancels it) -
it changes field-proven core gating and was not worth coupling to this
release. Structural items below (RenderImpl split, Settings.h split, LOAD
registry, vcpkg prune, /W4 suppressions, log-tag sweep) remain deferred to
the 1.0 fresh repo.

Findings from the pre-publish style review (three independent passes:
naming/API shape, structure/idioms, hygiene) against CommonLib-plugin and
professional norms, run right after the comment overhaul. Report only -
nothing here was auto-applied. Working punch list for the v0.3.12 cleanup
and the manual code review.


## Should fix before publishing

- **src/TrueScopes/ScopeRender.cpp:98** - Engine data RVAs are re-declared under divergent names instead of coming from TrueScopes::Addr, recreating the exact duplication the kPlayerGlobal comment in Addresses.h says was cured: kRendererRVA (ScopeRender.cpp:98) duplicates Addr::kRendererInstance (Addresses.h:115) in a file that already uses Addr:: ten times; kD3DContextRVA = 0x6235ab0 is defined in both LensComposite.cpp:14 and ScopeRender.cpp:249; 0x5930608 is kPlayerCameraGlobal in PoseGate.cpp:14, kPlayerCameraPtr in ScopeRender.cpp:728, and a bare literal in LensComposite.cpp:307.
  - Suggestion: Hoist every data RVA used by more than one translation unit into TrueScopes::Addr with one canonical name and verification tag (kPlayerCameraSingleton, kD3DImmediateContext, delete kRendererRVA in favor of Addr::kRendererInstance); keep only genuinely single-file function RVAs local.

- **src/TrueScopes/Hooks.h:5** - Install()'s contract comment is several versions stale: it says the function "verifies original bytes at both patch sites" and installs "the one-byte defang of the scope-arm setter" plus the fill hook, and "Returns false (and leaves the game untouched) if any byte check fails." Install() (Hooks.cpp:1289-1435) actually writes ten thunk hooks (arm write is a thunk, not a one-byte patch) plus imod and button-bar byte patches, several of which fail soft after earlier hooks are already written — so both the mechanism and the failure contract described are wrong. This is the plugin's primary API doc and the first thing a reader of a public repo meets.
  - Suggestion: Rewrite the comment to enumerate the real hook set (or point at Addresses.h, which documents each site accurately) and state which site verifications are fatal (return false, nothing written) versus non-fatal (logged, feature degraded).

- **src/TrueScopes/ScopeRender.cpp:1438** - RenderImpl is ~910 lines (1438-2350), tracked internally by 19 RENDER_STEP markers. The file already names its natural seams: kStageNames in ScopeRender.h defines seven stages (setup/lights/accum/sun/resolve/sky/deliver) and the stage stopwatch brackets exactly those regions, yet the code is one monolith. A 900-line function is the single biggest obstacle for outside contributors reading the core of the mod.
  - Suggestion: Extract one static helper per stage (e.g. SetupCameraAndTargets, RunLightFit, AccumulateWorld, ExecSunPass, RunResolve, DrawSky, DeliverLens), each taking a small context struct (base/rtm/renderer/cam/accum). The RENDER_STEP numbers and timer marks move into the helpers unchanged; RenderImpl becomes a ~40-line sequence that reads like the pipeline description in the file-header comment.

- **src/Settings/Settings.h:573** - Settings is an 844-line header-only module: a ~270-line inline load() (573-843), NormalizeScopeKeyInto, and mutable inline globals (std::mutex scopeApertureLock, std::map scopeEntries, std::function postLoadHook) all live in the header, which every TU in the project includes. Every knob-comment tweak recompiles the whole plugin, and non-trivial logic (TOML parsing, key normalization, validation clamps) is compiled into eight TUs. Buffout4 and FRIK both keep parse logic in a .cpp.
  - Suggestion: Split into Settings.h (Setting<T> template, MAKE_SETTING declarations as extern or kept inline, function declarations) and Settings.cpp (load(), NormalizeScopeKeyInto, ScopeEntryFor, the lock and map). Add Settings.cpp to sourcelist.cmake. keynorm/check.py compiles NormalizeScopeKeyInto verbatim, so keep that function's text intact when moving it.

- **src/TrueScopes/ScopeRender.cpp:1653** - ScopeRender.cpp calls engine functions through raw hex RVAs inline at ~30 call sites — Fn<SetCurRT_t>(0x1db9dd0) alone is repeated 8+ times — and re-declares kRendererRVA = 0x6239340 and kRTManager = 0x38ac010 locally even though Addresses.h is the documented single source of truth (and already holds kRendererInstance with the same value). ScopeIdent.cpp and LensComposite.cpp both do this right (named kGetInventoryItem-style constants). Updating an address after a game patch means grepping hex literals in this one file.
  - Suggestion: Hoist every RVA used in ScopeRender.cpp into named constexpr constants — either into TrueScopes::Addr alongside the existing documented entries, or a local '--- RVAs ---' block next to the function typedefs (which already carry the addresses in comments). Replace kRendererRVA/kRTManager with Addr::kRendererInstance and a new Addr::kRTManager.

- **src/Settings/Settings.h:440** - The `pillCensus` diagnostic defaults to true, and the shipped TOML (Data/F4SE/Plugins/TrueScopesVR.toml:89-95) sets it true too, under a comment that says 'v0.3.6 diagnostic pair (temporary; will be removed once the pill is confirmed dead)'. Every public user's first scope-in will run a full scene-graph walk and dump every HUD-ish node (plus the whole ScopeParent subtree via PillDumpSubtree) into their log.
  - Suggestion: Default pillCensus to false (or delete the knob entirely now that v0.3.11 kills the pill in the shipped SWF), and remove the 'temporary' diagnostic block from the shipped TOML.

- **src/TrueScopes/Hooks.cpp:429** - The entire runtime pill machinery — PillCensusRun/PillCensusWalk/PillDumpSubtree (~lines 240-780), the kill modes (pillKillMode 1/2, rack-slot holing), and TryHideHoldBreathMovieElement (lines 24-71) — is superseded dead diagnostic code: v0.3.11 zero-scales the pill in the shipped ScopeMenu SWFs, and its own commit message says 'strip after the chord pass'. This is exactly the kind of leftover hunt code a public reader will trip over.
  - Suggestion: Strip the runtime pill hunt (census, kill modes, movie-probe) before tagging the public release, keeping only hideHoldBreathHint's SetUpButtonBar nop if still wanted as belt-and-braces.

- **CMakeLists.txt:3** - VERSION is 0.3.10 but HEAD is the v0.3.11 release commit. Version.h derives MAJOR/MINOR/PATCH from this, so a_info->version (main.cpp:66) and every Buffout crash log will name the wrong build — defeating the comment there about crash logs naming the real build.
  - Suggestion: Bump the CMake VERSION with every tagged release even when only Data/ changed (or add a release-checklist item / CI check that the tag and CMake VERSION agree).

- **src/TrueScopes/LensComposite.cpp:60** - The HLSL/comment block is the repo's worst offender against its own docs/STYLE.md ('no version tags, dates, or session references; no ALL-CAPS emphasis'): version tags v0.2.123/126/129/130/131 at lines 60, 86, 167, 184-191, 212, 229; a field date and quoted user feedback at line 251 ('field 2026-08-25: "the overall screen..."'); caps-for-emphasis throughout ('HOUSING RIM SHADOW', 'DARK-ADAPTIVE', 'SHIFTS OPPOSITE').
  - Suggestion: Rewrite these comments to the STYLE.md form: keep the optics facts and knob semantics, drop the version arc, dates, and shouting (e.g. 'residual scatter scales with average picture luminance from five fixed taps; glass2.x = 0 keeps it constant').

- **src/TrueScopes/ScopeRender.cpp:1653** - Engine RVAs appear as raw literals at every call site — `Fn<SetCurRT_t>(0x1db9dd0)` 36 times, `Fn<CommitTargetsAlt_t>(0x1db9f80)` 12 times, plus inline layout math like `accum + 0x18 + g * 0x678` (lines 503, 1734, 1737). docs/STYLE.md says RVAs get a named constant 'not a magic number at the use site', and Addresses.h already exists as the pattern. A future game-version fix means touching dozens of sites and hoping the typedef comment stays in sync.
  - Suggestion: Hoist each RVA into a constexpr (`kSetCurrentRenderTarget = 0x1db9dd0`, `kAccumGroupBase = 0x18`, `kAccumGroupStride = 0x678`, ...) next to the typedefs or in Addresses.h, and call `Fn<SetCurRT_t>(kSetCurrentRenderTarget)`.


## Consider

- **src/Settings/Settings.h:550** - The only mutable namespace-scope globals in the project without the g_ prefix are the three exposed in this public header: postLoadHook (line 449), scopeApertureLock (550), scopeEntries (553) — every .cpp keeps such state g_-prefixed in anonymous namespaces. The raw map + mutex are also part of the public surface even though all access is supposed to go through ScopeEntryFor(), and scopeApertureLock's name is stale: entries now carry offsets and aspect, not just apertures.
  - Suggestion: Either move the map/mutex into Settings.cpp behind the existing accessors, or at minimum rename to the project convention (g_postLoadHook, g_scopeEntries, g_scopeEntriesLock) so header-exposed mutable state is visibly global and the lock names what it guards.

- **src/TrueScopes/ScopeRender.cpp:1653** - Dozens of engine-function RVAs appear as bare hex literals at call sites — Fn<SetCurRT_t>(0x1db9dd0) is repeated seven times (lines 1653-1663), Fn<VanillaLensCopy_t>(0x27b08c0)(0x61, ...) at 2299, plus ~25 more one-off literals (551, 699, 1517, 1618-1744...) — while the same file names other RVAs as constexprs (kRTManager, kIsmInstanceRVA at 97-99) and Addresses.h documents each constant with a [LIVE]/[GHIDRA] verification tag. The mixed style means half the binary contract is greppable and tagged, half is not.
  - Suggestion: Give each engine function one named constexpr next to the existing block at line 97 (kSetCurrentRenderTarget = 0x1db9dd0, kTonemapCopy = 0x27b08c0, ...) with its verification tag, and pass the name at call sites; same for the recurring RT ids 0x61/0x63-0x69 (kRT_ScopeColor, kRT_GBuf0...).

- **src/Settings/Settings.h:402** - sunEnabled (line 145) and sunExecEnabled (402) are two public TOML keys one word apart with dangerously different semantics — the comment itself warns "sunEnabled=false is not equivalent — it drops all of those too and faults the delivery." Nothing in the names conveys that one is the safe user switch and the other a narrow internal stage toggle; once the mod is public these key names are frozen by user TOMLs.
  - Suggestion: Settle before release: keep sunEnabled as the sole user-facing switch and either fold sunExecEnabled's safe semantics into it or rename it to signal its diagnostic nature (e.g. debugSunExec) and group it with the other diagnostics.

- **src/Settings/Settings.h:431** - Settings whose own comments say "Diagnostic" (pillKillMode 431, pillCensus 440 — a one-shot full scene-tree census defaulting to true, verdictInputEnabled 183 — the guided-test chord harness, perfTimers 137) sit in the flat [TrueScopesVR] group indistinguishable by name from shipping user knobs. A public release freezes this namespace, and users will tweak diagnostics they should not.
  - Suggestion: Move diagnostics into their own TOML group (e.g. [Debug]) or give them a shared name prefix now, while renaming is still free; reconsider pillCensus defaulting to true in a release build.

- **src/TrueScopes/ScopeIdent.h:213** - [[nodiscard]] and noexcept are applied ad hoc across the public headers: ScopeIdent.h marks IsNiNode, ScopeBound, OcularFaceWorld, LookupModelPath but not the equally pure ProbeCount, ApertureRadius, FovMult, Get, Table; PoseGate.h and Hooks.h mark no query at all; ScopeRender.h's GetDiagnostics/GetPlacement/GetFovInfo are unmarked while LensPrimeNeeded is noexcept and its neighbor WidgetPresentable is not.
  - Suggestion: Adopt one rule and sweep the headers: [[nodiscard]] on every pure query (the Buffout4/CommonLib norm), noexcept per a stated criterion (e.g. everything invoked inside an SEH bracket or from a hook thunk).

- **src/Settings/Settings.h:597** - Every setting is spelled twice: 108 MAKE_SETTING declarations plus a hand-maintained list of 108 LOAD(...) calls inside load(). The KnownKeys registry catches a misspelled TOML key, but nothing catches a declared setting whose LOAD line was forgotten — that setting silently stays at its default forever, the exact failure mode the KnownKeys machinery was built to kill in the other direction.
  - Suggestion: Finish the registry the Setting ctor already feeds: have the ctor register a type-erased loader (capture 'this', store std::function<void(const toml::table&)> or an ISetting* with a virtual load) alongside the key, and make load() iterate that list. The per-setting LOAD block's value<T>() semantics and warning move into the loader once; the 108-line list and the drift hazard disappear.

- **CMakeLists.txt:218** - /W4 /WX is undermined by seven project-wide suppressions — including /wd4834 (discarded [[nodiscard]] return), /wd4456 (declaration shadowing), /wd4244 and /wd4267 (narrowing conversions), /wd4189 (unused initialized local) — plus an unconditional '#pragma warning(disable: 4100)' at src/PCH.h:8 that applies to all project code, not just headers. Buffout4 ships /W4 /WX with a single benign /wd4324. 4834 and 4456 in particular hide real bugs in exactly the kind of offset-heavy code this plugin is made of.
  - Suggestion: Before publishing, remove the suppressions one at a time and fix what surfaces, keeping at most the narrowing pair (/wd4244 /wd4267) if the raw-offset style makes them too noisy. Replace the PCH-wide 4100 disable with [[maybe_unused]] on the few hook parameters that need it (the code already uses that attribute in ScopeArmWriteHook).

- **src/TrueScopes/ScopeRender.cpp:90** - The Fn<T>(rva) helper (reinterpret_cast of REL::Module::get().base() + rva) is defined verbatim in three files — ScopeRender.cpp:90, ScopeIdent.cpp:80, LensComposite.cpp:43 (the last with a divergent 'noexcept' and template-param name) — while Hooks.cpp uses the idiomatic CommonLib form, REL::Relocation<T>{ REL::Offset(...) }. Two spellings of the same idea, one of them triplicated.
  - Suggestion: Define it once — either a shared 'template <class T> T Fn(std::uintptr_t)' in Addresses.h (it is already the cross-file RE utility header), or standardize on 'static REL::Relocation<T> fn{ REL::Offset(kRva) }; fn.get()' like Hooks.cpp — and delete the two copies.

- **src/main.cpp:7** - InitializeLog and MessageHandler are file-local helpers defined at global scope with external linkage — the only internal functions in the project not wrapped in an anonymous namespace (every TrueScopes/*.cpp and DevBench.cpp gets this right). They can collide at link time with any other TU and advertise themselves as API.
  - Suggestion: Wrap both in 'namespace { ... }' (Buffout4's main.cpp pattern), leaving only the two extern "C" exports at global scope.

- **src/TrueScopes/Hooks.cpp:1063** - RenderFillHook::thunk is ~170 lines (1063-1241) of unrelated per-frame duties inlined into one hook body: frame counting, gate-off hysteresis, stale-verdict deactivation, widget presence, pill/housing management, and the fill dispatch. Each concern already has its own comment block, but none has its own function, so the control flow between the early-return conditions is hard to audit.
  - Suggestion: Keep the thunk as a short ordered dispatcher — g_frames increment, then calls to PollGateHysteresis(), PollVerdictStaleness(), UpdateWidgetPresence(), DispatchFill() — with the existing comments moving onto the helpers in the anonymous namespace.

- **src/TrueScopes/ScopeRender.cpp:2798** - DecalStageImpl is defined at namespace scope after the anonymous namespace closes (line 2389), giving an internal helper external linkage, and it is not declared in ScopeRender.h — so it is neither internal nor part of the module's API. Everything else internal to this file correctly lives in the anonymous namespace.
  - Suggestion: Move DecalStageImpl (and its constants) into the anonymous namespace, forward-declaring it there if RunPendingDecalStage's placement requires it — or mark it static if reordering is undesirable.

- **src/Settings/Settings.h:8** - No project header is self-contained: Settings.h uses std::vector/std::mutex/std::map/std::function, ScopeRender.h uses std::uintptr_t/std::size_t, Hooks.h uses std::uint64_t, Addresses.h uses std::uintptr_t — all with zero #include lines, compiling only because CMake force-includes PCH.h into every TU. Any consumer outside this build (or a future PCH slimming) breaks, and IWYU-style hygiene is the CommonLib-plugin norm.
  - Suggestion: Add the direct standard includes each header actually uses (<cstdint>, <vector>, <string_view>, <mutex>, <map>, <functional>, ...). Cheap now, and it makes the eventual fresh-repo 1.0 extraction painless.

- **vcpkg.json:19** - Template leftovers heading to the public repo: vcpkg.json declares dependencies nothing uses (catch2, rapidcsv, args, srell, frozen, nowide, robin-hood-hashing, rsm-mmio at minimum — no test suite, no CSV/regex/CLI parsing in src/) and carries a stale "version-string": "0.1.0" against CMake's 0.3.10; CMakeLists.txt sets Boost_USE_STATIC_RUNTIME/Boost_USE_STATIC_LIBS (lines 56/101) with no Boost dependency and defines F4SE_SUPPORT_XBYAK (line 87) with no xbyak usage in the plugin. Every consumer building from source pays the vcpkg install cost for all of it.
  - Suggestion: Prune vcpkg.json to spdlog + tomlplusplus plus whatever CommonLibF4's manifest actually requires (build clean to verify), sync version-string with the CMake VERSION (or drop it), delete the Boost_ variables, and drop F4SE_SUPPORT_XBYAK if a build without it succeeds.

- **src/main.cpp:24** - Log level is hardcoded to spdlog::level::trace with flush_on(trace) — every message synchronously flushed to disk in a VR title — and there is no user-facing verbosity knob among the 109 settings. Combined with the per-300-render heartbeats and one-shot diagnostics, public users get large, ever-growing logs with no way to quiet them.
  - Suggestion: Default to spdlog::level::info, flush_on(warn or err), and expose a logLevel TOML setting (the Buffout4/CommonLib-plugin convention).

- **src/TrueScopes/ScopeRender.cpp:1967** - Log message style is inconsistent across the plugin: ALL-CAPS subsystem tags ('SUN EXEC', 'WIDGET FIT', 'SCOPE IDENT:', 'DECAL STAGE:', 'PERF TIMERS:') beside lowercase ones ('pillcensus:', 'scope active -> true'); em-dash vs hyphen mixed even within main.cpp (line 68 vs 102); and raw Ghidra names in user-visible text ('SUN EXEC {} — FUN_142891040 returned {}').
  - Suggestion: Pick one convention — 'Subsystem: sentence-case message' with ASCII dashes — sweep the FMT_STRING literals once, and keep FUN_ addresses in comments rather than log text (users paste these logs into Nexus posts).

- **src/TrueScopes/Hooks.cpp:18** - Comment states 'FO4 menus are Scaleform AS3, so paths root at "root.", not "_root."' — the v0.3.11 commit message records the opposite conclusion (the movie is AS2, __setProp naming, and the _root/_visible spelling was correct). A factually wrong engine claim in a comment will mislead the next reader more than no comment.
  - Suggestion: Correct the comment to the AS2 finding (or delete the whole block along with the superseded movie-probe machinery it documents).

- **src/DevBench/DevBench.h:11** - Stale self-documentation in the shipped dev-tooling: the header says 'this is dev tooling, not shipping code' yet it compiles into the public DLL unconditionally; DevBench.cpp:431's route index says 'feed them to /read' though the /read route was removed in the de-bloat; and /verdict and /attach are handled (DevBench.cpp:1198-1199) but missing from the self-describing index (lines 428-438).
  - Suggestion: Refresh the header comment and the /index route list to match what actually ships; if it genuinely is not meant to ship, put it behind a CMake option instead.

- **Data/Interface/README_SWF_EDITS.md:29** - This dev note sits inside the mod's Data tree, so verbatim packaging puts it in users' game Data/Interface folder — complete with local machine paths ('C:\tools\ba2extract\fo4vr_interface\...'), references to a private investigation repo ('SESSION_2026-08-26_V03_CLOSEOUT.md Addendum 5'), and heavy caps emphasis.
  - Suggestion: Move the note to docs/ (it is good provenance documentation) or exclude it in the packaging step, and strip the local paths / private-repo pointers from whatever ships.


## Nits

- **src/Settings/Settings.h:246** - Boolean feature keys are inconsistently suffixed: eight plugin features use ...Enabled (fillEnabled, widgetFitEnabled, poseGateEnabled, lensCompositeEnabled, sunEnabled, skyEnabled, decalStageEnabled, verdictInputEnabled) while other feature toggles are bare noun/verb phrases — perScopeAperture (246), widgetAutoPlace, cullToScopeFrustum, retryAfterFault, deliveryUnbindDS, decalGroup5Reorder, poseWidgetAlways. (The disable/suppress/hide verbs for removing vanilla behavior read as a deliberate, coherent family and are fine.)
  - Suggestion: Before the names freeze, either give the stragglers the Enabled suffix (perScopeApertureEnabled, autoPlaceEnabled...) or document the convention in the shipped TOML so the mix reads as intent rather than drift.

- **src/TrueScopes/OneEuro.h:24** - namespace OneEuro is the project's only top-level utility namespace — every other component lives under TrueScopes:: (or the established Settings/DevBench roots). A generic top-level name in a plugin codebase invites collision if the header is ever copied alongside other utility code, and breaks the reader's namespace map.
  - Suggestion: Nest it as TrueScopes::OneEuro (the four call sites in ScopeRender.cpp are the only consumers).

- **src/TrueScopes/ScopeIdent.h:115** - Header self-containment is inconsistent: PoseGate.h, LensComposite.h and VerdictInput.h dutifully include <cstdint>, but ScopeIdent.h uses std::numeric_limits (115) and std::span (233) with zero includes, and Hooks.h/Addresses.h/ScopeRender.h use std:: integer types bare — all silently riding the force-included PCH.
  - Suggestion: Pick one convention: either rely on the PCH everywhere and drop the three headers' std includes, or make every header compile stand-alone (add <limits>, <span>, <cstdint> where used). The half-and-half state reads as accident.

- **src/TrueScopes/Addresses.h:201** - kRT_MainFrame / kRT_ScopeLens (201, 205) are the only constants in Addresses.h with an embedded underscore; everything else is pure kCamelCase (kScopeStateReadCallSite, kRendererInstance...). ScopeRender.cpp then continues the underscore family ad hoc (kDS_None) while leaving its other RT ids as bare literals.
  - Suggestion: Either fold to the file's own style (kMainFrameRT, kScopeLensRT) or commit to kRT_/kDS_ as a deliberate id-family prefix and use it for all render-target/depth-stencil ids the code names.

- **src/TrueScopes/ScopeRender.cpp:264** - kSegCount is defined as kMarkCount - 1 with the comment "== kStageCount in the header" — a sync invariant enforced only by prose. ScopeRender.cpp includes its own header, where kStageCount and kStageNames are inline constexpr, so adding a stage there and forgetting this file compiles clean and silently mistimes every stage.
  - Suggestion: Derive instead of documenting: constexpr std::uint32_t kMarkCount = kStageCount + 1; and delete kSegCount in favor of kStageCount (or static_assert(kSegCount == kStageCount)).

- **cmake/sourcelist.cmake:9** - The file lists are misfiled: sourcelist.cmake puts three headers in ${sources} (PoseGate.h, VerdictInput.h, LensComposite.h) while headerlist.cmake omits them plus ScopeIdent.h and OneEuro.h. Harmless to the build, but the IDE source_group tree — the first thing a new contributor sees — misplaces five files, and the lists no longer document the module inventory.
  - Suggestion: Move the three .h entries to headerlist.cmake and add ScopeIdent.h and OneEuro.h there, so ${headers} is the complete header inventory and ${sources} is .cpp-only.

- **src/DevBench/DevBench.cpp:1224** - User-visible startup log carries version-history narrative: 'the :8930 HTTP listener was retired in v0.2.138' — meaningless to a public user and against the repo's own no-version-tags rule (which STYLE.md applies to source; the same logic applies to shipped log text).
  - Suggestion: Shorten to "devbench routes ready (served via the 'scope' tool on the devbench host)" and let git carry the retirement history.

- **src/TrueScopes/ScopeRender.cpp:6** - Dead includes left over from the de-bloat: <fstream> here has no stream usage anywhere in the file (the BMP dump code it served is gone); likewise DevBench.cpp still includes <WS2tcpip.h> (line 3) and <thread> (line 8) though the socket listener and its thread were removed in v0.2.138.
  - Suggestion: Drop <fstream> from ScopeRender.cpp and <WS2tcpip.h>/<thread> from DevBench.cpp.
