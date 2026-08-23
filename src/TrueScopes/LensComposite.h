#pragma once

// v0.2.104 — OWN DELIVERY PASS (SESSION_2026-08-23_RETICLE_AND_GLASS.md §4).
//
// After the engine tonemap has written the picture into the lens RT 0x62, we copy
// 0x62 to a private scratch texture and draw ONE fullscreen triangle back into 0x62
// with our own pixel shader, compositing:
//   * the engine's own Scaleform reticle (the ScopeMenu Interface3D renderer's
//     custom RTs: +0x1dc = raw, +0x1d8 = HUD-glassed), so every vanilla overlay
//     (German, Circle, ModernRangefind, NV, Recon ...) comes for free, and
//   * the glass look: radial vignette, tint, exposure — all TOML knobs.
// Test B (2026-08-23) proved the vanilla reticle quad `render_UI:0` is drawn
// UNDER our picture disc; since we now composite the reticle ourselves, that quad
// is hidden while the pass is active (and restored on scope-off).
//
// All D3D state we touch is captured with Get* before and restored with Set*
// after; then the engine's cached-state block is invalidated (it is not told about
// raw D3D changes otherwise) — the FTS pattern.

#include <cstdint>

namespace TrueScopes::LensComposite
{
	struct Inputs
	{
		std::uintptr_t renderer;   // BSGraphics::Renderer (base + 0x6239340)
		std::uintptr_t rtm;        // RenderTargetManager (base + 0x38ac010)
		std::uintptr_t ctx;        // engine per-thread render ctx (DAT_146235ac8 / ac0)
		std::uintptr_t scopeParent;  // player+0x7d0 (may be 0: reticle quad not hidden)
		std::uint32_t  lensLogicalRT;  // 0x62
	};

	// Runs the pass. Returns false (and logs once) when a resource could not be
	// created — the lens then keeps the plain tonemap output. Never throws; the
	// caller's SEH bracket covers the D3D calls.
	bool Run(const Inputs& a_in) noexcept;

	// Un-hide `render_UI:0` (if we hid it). Called on scope-off and when the pass
	// is disabled live.
	void RestoreReticleQuad() noexcept;

	// Diagnostics for DevBench /state.
	struct Diag
	{
		bool         ready;          // shaders + states compiled
		std::int32_t reticleRT;      // logical RT index sampled (-1 = none found)
		std::int32_t reticlePhys;    // physical slot
		std::uint32_t runs;
		std::uint32_t skips;         // Run() returned false
		bool         quadHidden;
		char         rendererName[32];
	};
	Diag GetDiag() noexcept;
}
