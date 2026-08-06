#pragma once

namespace TrueScopes::Hooks
{
	// Verifies original bytes at both patch sites, then installs:
	//   1. the one-byte defang of the scope-arm setter (renderer+3 can never be set,
	//      so the vanilla scoped-frame redirect — black main view, rebuild churn,
	//      deferred-release hazards — never engages)
	//   2. the per-frame fill hook inside Main::DrawWorld_And_UI
	// Returns false (and leaves the game untouched) if any byte check fails.
	bool Install();

	// Called on kGameLoaded: applies forceAlwaysOn to the iScopeEnabled value cell.
	void OnGameLoaded();
}
