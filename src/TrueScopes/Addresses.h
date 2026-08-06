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

	// Call site inside Main::DrawWorld_And_UI (+0xd87a80), right after the Pip-Boy
	// local-map block: "call thunk_FUN_14284e950" (accumulator pass-list clear).
	// Runs every frame in the normal (non-redirected) draw path, render side, renderer
	// locked — the same slot the engine uses for its own mid-frame LocalMap render.
	// [LIVE] original bytes: E8 87 70 AC 01
	inline constexpr std::uintptr_t kRenderFillCallSite = 0xd87ca4;

	// ImageSpaceManager::Copy(uint32 srcRT, uint32 dstRT) — self-contained fullscreen
	// RT-to-RT copy (fetches its own singleton; binds dst, samples src, restores state).
	// Vanilla precedent: Main::Swap calls Copy(0, 0x44) for screenshots. [LIVE-exercised]
	inline constexpr std::uintptr_t kImageSpaceManagerCopy = 0x27b0880;

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

	// --- render target indices (logical, via RenderTargetManager remap table) ---

	// Double-wide stereo frame color target (what gets submitted to OpenVR).
	inline constexpr std::uint32_t kRT_MainFrame = 0x4;

	// The scope widget's display target: whatever is written here shows in the lens.
	// Persists between frames while the vanilla redirect is disarmed.
	inline constexpr std::uint32_t kRT_ScopeLens = 0x62;
}
