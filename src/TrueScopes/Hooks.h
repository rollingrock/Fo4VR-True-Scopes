#pragma once

#include <cstdint>

namespace TrueScopes::Hooks
{
	// Verifies original bytes at both patch sites, then installs:
	//   1. the one-byte defang of the scope-arm setter (renderer+3 can never be set,
	//      so the vanilla scoped-frame redirect — black main view, rebuild churn,
	//      deferred-release hazards — never engages)
	//   2. the per-frame fill hook inside Main::DrawWorld_And_UI
	// Returns false (and leaves the game untouched) if any byte check fails.
	bool Install();

	// Register the unequip event sink (the teardown latch's arm signal). Once,
	// at kGameDataReady - the event source singleton exists by then.
	void RegisterEquipSink();


	// Count of per-frame fill-hook invocations, i.e. the game's frame count.
	// Advances whether or not the scope is up: ScopeRender's own `renders` counter
	// only ticks while scoped, so it cannot measure the scope-down baseline.
	// Frames/second from two reads of this is the game frame rate in any state.
	std::uint64_t FrameCount();

	// True while the scope is considered active (post-hysteresis). Lets a perf sample
	// record which state it was taken in instead of trusting the operator's notes.
	bool ScopeActive();

	// Is the plugin-owned widget presence currently showing the nodes.
	bool WidgetPresenceShown();
}
