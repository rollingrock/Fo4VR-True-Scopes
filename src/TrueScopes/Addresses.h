#pragma once

// All offsets are RVAs against Fallout4VR.exe 1.2.72 (image base 0x140000000, no ASLR).
//
// Verification tags:
//   [LIVE]   byte-verified in the running game, 2026-08-06 x64dbg session
//            (SESSION_2026-08-06_ROUTE_B_LIVE.md in the investigation repo)
//   [GHIDRA] verified against dump code bytes (which match the live process exactly)
//
// IMPORTANT: Ghidra's data labels for high .data/.bss in the unpacked dump are
// systematically wrong (section layout mismatch in the imported image). Every data RVA
// here comes from RIP-displacement decoding of code bytes or a live memory read — never
// from a bare DAT_ label. See ROUTE_B_STATIC_MAP_2026-08-06.md section 0.

namespace TrueScopes::Addr
{
	// --- code (patch/hook sites) ---

	// Inside the scope enable switch FUN_140efaa60 — the ONLY reader of iScopeEnabled
	// and the ONLY code path that arms the vanilla scoped-frame redirect.
	//
	// The switch uses BSGraphics::Renderer+3 as its on/off state memory:
	//   on = (iScopeEnabled==2) || (iScopeEnabled==1 && isScoped)
	//   if (on != ReadFlag3(renderer)) { WriteFlag3(renderer, on); <show/hide block> }
	//
	// We hook BOTH call sites: the write stores to plugin state instead of renderer+3
	// (so the frame redirect never engages), and the read returns that plugin state
	// (so the switch stays edge-triggered — the show/hide block runs on transitions
	// only). v0.1.0 defanged the writer alone, which made the guard never satisfy and
	// re-ran the block (incl. Pip-Boy menu messaging) every tick → input-layer
	// exhaustion → null-deref crash in the ScopeMenu ctor (crash-2026-08-06-21-02-53).

	// call FUN_141d947b0 (read renderer+3) — the guard's state read.
	// [GHIDRA] original bytes: E8 F4 9C E9 00
	inline constexpr std::uintptr_t kScopeStateReadCallSite = 0xefaab7;

	// call FUN_141d947a0 (write renderer+3) — the arm write, sole call site in the binary.
	// [GHIDRA] original bytes: E8 CD 9C E9 00
	inline constexpr std::uintptr_t kScopeArmWriteCallSite = 0xefaace;

	// Inside the vanilla eye-gate TS_Player_UpdateScopeEyeGate (FUN_140ef7b00, per
	// frame on the game thread from Main::OnIdle): the ONE call that feeds the
	// per-frame proximity VERDICT to the enable switch —
	//     movzx edx, bl ; mov rcx, rdi ; call FUN_140efaa60
	// The verdict upstream = look cone (fHmdToWeaponAngleEnter/ExitScopeConeDegrees
	// 25/35) AND weapon-forward cone (fWeaponAngleEnter/ExitScopeConeDegrees 7/15,
	// widened by (fScopeWeaponAngleWideningFactor 60 / dist)^2) AND eye↔scope
	// distance (fWeaponDistEnterScope/Exit 38/40 — the pistol-at-arm's-length
	// blocker), with player+0x12a1 bit 8 ("Steady" hold-breath) forcing true.
	// v0.2.116 thunks this call and routes the verdict through PoseGate. The other
	// FUN_140efaa60 call sites (menus-open/holster force-off, equip paths) are
	// deliberately NOT hooked — their force-off meaning is kept.
	// [GHIDRA] original bytes: E8 3C 25 00 00 (verified 2026-08-24)
	inline constexpr std::uintptr_t kScopeGateVerdictCallSite = 0xef851f;

	// FUN_141d947d0 — the renderer+4 (scoped-pass) READER, exactly 5 bytes:
	// "0F B6 41 04 C3" (movzx eax, byte [rcx+4]; ret). Entry-hooked in v0.2.48 so
	// scoped mode is answered ONLY to our render thread while our bracket is live —
	// concurrent engine threads observing our transient +4=1 is the black-burst
	// suspect (phantom late-frame scoped actions on the lens RT).
	inline constexpr std::uintptr_t kScopePassReadFn = 0x1d947d0;

	// Call site inside Main::DrawWorld_And_UI (+0xd87a80), right after the Pip-Boy
	// local-map block: "call thunk_FUN_14284e950" (accumulator pass-list clear).
	// Runs every frame in the normal (non-redirected) draw path, render side, renderer
	// locked — the same slot the engine uses for its own mid-frame LocalMap render.
	// [LIVE] original bytes: E8 87 70 AC 01
	inline constexpr std::uintptr_t kRenderFillCallSite = 0xd87ca4;

	// The three vanilla scope imagespace-modifier trigger sites (all call
	// ImageSpaceModifierInstanceForm::Trigger @ +0x37cf50). A and B fire the weapon
	// zoomData imod at full strength on scope-in — the vanilla WORLD BLACKOUT. C ramps
	// the approach fade with eye-scope alignment. With the frame redirect disarmed these
	// are pure cosmetics; A/B must be suppressed for world+scope simultaneously.
	// A/B patch: 33 C0 90 90 90 (xor eax,eax + nops — return value is refcount-stored,
	// so eax must be null, not garbage). C patch: 5x 90 (return unused).
	// [GHIDRA] A bytes: E8 D0 49 7B FF   B bytes: E8 7F 4E 7B FF   C bytes: E8 2F 49 48 FF
	inline constexpr std::uintptr_t kScopeBlackoutImodSiteA = 0xbc857b;  // ScopeMenu show callback
	inline constexpr std::uintptr_t kScopeBlackoutImodSiteB = 0xbc80cc;  // ScopeMenu::ProcessMessage
	inline constexpr std::uintptr_t kScopeApproachFadeSite = 0xef861c;   // aim controller proximity fade

	// ImageSpaceManager::Copy(uint32 srcRT, uint32 dstRT) — self-contained fullscreen
	// RT-to-RT copy (fetches its own singleton; binds dst, samples src, restores state).
	// Vanilla precedent: Main::Swap calls Copy(0, 0x44) for screenshots. [LIVE-exercised]
	inline constexpr std::uintptr_t kImageSpaceManagerCopy = 0x27b0880;

	// The deferred resolve FUN_1427ff8b0 opens its lighting phase by binding the light
	// accumulation MRT with bind mode 0 (clear-on-apply): slot 0 = RT 0x24/0x6a, slot 1 =
	// RT 0x25/0x6b (scope set under renderer+4). Because the SUN's BSDFLight "Dir" pass is
	// drawn by the per-frame pre-world stage (FUN_142846d60, queued job FUN_142849990) and
	// NOT by the resolve, our own scope render has to pre-draw the sun into 0x6a/0x6b —
	// and these two clears would wipe it. We hook both call sites and force bind mode 3
	// (no clear) only while our render's resolve call is on the stack; every engine call
	// (main frame) passes through unchanged.
	// [GHIDRA] bind0 bytes: E8 00 A0 5B FF   bind1 bytes: E8 D1 9F 5B FF
	//          (both → BSGraphics::RenderTargetManager::SetCurrentRenderTarget @ +0x1db9dd0)
	inline constexpr std::uintptr_t kResolveAccumBind0CallSite = 0x27ffdcb;  // slot 0, 0x24/0x6a
	inline constexpr std::uintptr_t kResolveAccumBind1CallSite = 0x27ffdfa;  // slot 1, 0x25/0x6b

	// --- data ---

	// Value cell of the 3-state INI setting iScopeEnabled:VR (0=off, 1=eye-gated,
	// 2=always-on). Read in exactly ONE place: the enable switch. [LIVE]
	inline constexpr std::uintptr_t kIScopeEnabledValue = 0x37d02d8;

	// BSGraphics::Renderer instance. Flag bytes: +1 stereo, +2 alt-render,
	// +3 scope-armed (must stay 0), +4 scope-pass-active, +5 skip-accumulation. [LIVE]
	inline constexpr std::uintptr_t kRendererInstance = 0x6239340;

	// Scope-widget shader property cache. Set by the enable switch; compared per-draw
	// inside FUN_1428cf8c0 @ +0x28d1558: if (pass->property == this) bind RT 0x62's SRV
	// to texture slot 6. The bind is UNCONDITIONAL apart from this pointer equality. [LIVE]
	inline constexpr std::uintptr_t kWidgetMaterialCache = 0x689b440;

	// WSScopeModel singleton pointer cell (DAT_145acbf58; ctor FUN_140c8da70 stores
	// it). Field map (Ghidra 2026-08-24, TS_WSScopeModel_InitShapes 0x140c8ddb0):
	// +0x10 root (process-lifetime clone of world_scope.nif; init latch byte +0x19
	// is cleared only in the ctor, so the clone is NEVER rebuilt — cached child
	// pointers are stable for the whole session), +0x38 ACTIVE render shape,
	// +0x40 render_circle:0, +0x48 render_square:0, +0x50 ACTIVE housing,
	// +0x58 scope_Hunting:0, +0x60 scope_recon:0, +0x68 ACTIVE screenFade,
	// +0x70 recon_screenFade:0, +0x78 hunting_screenFade:0, +0x80 render_UI:0.
	// The placement-refresh virtual (vtbl slot 2, 0x140c8dc50) is a literal no-op
	// for this model: nothing in the engine writes the child shapes' LOCAL
	// transforms post-load, and world_scope.nif ships zero controller blocks —
	// which is what makes the v0.2.121 zero-scale housing hide permanent. [LIVE]
	inline constexpr std::uintptr_t kWidgetModelSingleton = 0x5acbf58;

	// The ONE call site in the whole binary that un-hides the widget housing:
	// inside the enable switch FUN_140efaa60 (TS_Player_ScopedModeArmSwitch),
	// edge-gated on the arm state, calling FUN_140c8e340
	// (TS_WSScopeModel_ShowActiveHousingAndFade = SetAppCulled on model+0x50
	// ACTIVE housing and +0x68 ACTIVE screenFade — decompiled 2026-08-24, xref
	// sweep confirmed single site). Fires on every scope-raise edge, which is
	// why a poked flag hide "survived until an aim edge" in the field.
	// v0.2.121 thunks it: passthrough (fade + the arm block's later
	// shader-cache refresh keep vanilla flow), then re-cull the housing while
	// hideWidgetHousing is on.
	// [GHIDRA] original bytes: E8 49 38 D9 FF (verified 2026-08-24)
	inline constexpr std::uintptr_t kHousingShowCallSite = 0xefaaf2;

	// --- deferred-decal ground truth (Ghidra deep-dive 2026-08-25) ---
	// LORE CORRECTION: v0.2.11's "decal group" label was WRONG - group 0x11 is
	// SKY (BSSkyShaderProperty files 0x11 dome/sun/moons, 0x12 clouds, 0x17 sun
	// glare). The real deferred-decal accumulator groups are 5 (non-skinned:
	// placed grime/posters and geometry decals on statics) and 6 (skinned:
	// blood on actors). The resolve draws group 5 at +0x27ff9b4 BEFORE the
	// opaque G-buffer groups 2/1/3/4 and group 6 at +0x27ffa27 AFTER them - in
	// a resolve-only render (ours) that order paints wall decals first and the
	// walls over them. v0.2.132 call-site hooks both sites and, while
	// g_inOwnResolve, defers group 5 until the group-6 site. Both helpers take
	// (accum, ctx) verbatim.
	// [GHIDRA] G5 site bytes: E8 67 D8 01 00; G6 site bytes: E8 54 D9 01 00
	// (both byte-verified 2026-08-25).
	inline constexpr std::uintptr_t kResolveDecalG5CallSite = 0x27ff9b4;
	inline constexpr std::uintptr_t kResolveDecalG6CallSite = 0x27ffa27;
	inline constexpr std::uintptr_t kAccumDrawDecalGroup5 = 0x281d220;
	inline constexpr std::uintptr_t kAccumDrawDecalGroup6 = 0x281d380;
	// NOTE: bullet holes are NOT passes at all - BSDFDecal objects (SSN+0x218)
	// drawn by the dedicated DrawWorld stage FUN_142845cc0 our render never
	// runs. That stage's plugin invocation is designed and under adversarial
	// re-verification; groups 5/6 above only cover geometry/placed decals.

	// --- render target indices (logical, via RenderTargetManager remap table) ---

	// Double-wide stereo frame color target (what gets submitted to OpenVR).
	inline constexpr std::uint32_t kRT_MainFrame = 0x4;

	// The scope widget's display target: whatever is written here shows in the lens.
	// Persists between frames while the vanilla redirect is disarmed.
	inline constexpr std::uint32_t kRT_ScopeLens = 0x62;
}
