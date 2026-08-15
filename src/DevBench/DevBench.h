#pragma once

// DevBench — a localhost query server that makes a *running* Fallout 4 VR
// agent-drivable, so a debug iteration no longer costs a rebuild + relaunch +
// headset-on + screenshot cycle.
//
// Architecture is modelled on alandtse/devbench (SKSE). That project is
// GPL-3.0-or-later, so NONE of its source is vendored here — only the shape of
// the idea: one loopback listener, a name->handler registry, JSON out, and a
// health endpoint answered off the game's threads so "hung" is distinguishable
// from "busy".
//
// Tier 1 (this file) is deliberately dependency-free and READ-MOSTLY:
//   * raw Winsock, no httplib / no MCP / no JSON library
//   * inputs arrive as query-string parameters, so nothing needs a JSON parser
//   * outputs are hand-built JSON
//   * the only mutation is setting our own TOML-backed settings
// Drive it with Invoke-RestMethod / curl. An MCP facade can be layered on
// later if this proves its worth; the registry below is where it would attach.
//
// SAFETY: binds 127.0.0.1 only, and refuses to start otherwise. It can read
// arbitrary process memory through /read --- the correct trade for a local dev
// bench. No release gate is needed: this repo is the RESEARCH repo and never
// ships; the production mod will be a brand-new repo rebuilt with only shipping
// code, so this file simply is not copied across. Add endpoints freely.

namespace DevBench
{
	// Start the listener thread. No-op (returns false) when devbenchEnabled is
	// off. Safe to call once, from F4SEPlugin_Load.
	bool Start();

	// Ask the listener to stop and join it. Safe to call if Start() failed.
	void Stop();

	// Port actually bound (0 = not listening).
	std::uint16_t Port();

	// Run one request and return its JSON body, WITHOUT going through HTTP.
	//
	// This exists so the `scope` tool we register into alandtse/devbench over its
	// cross-plugin C-ABI (see src/DevBenchClient/) answers by running the very same
	// handlers this server does. The alternative — a second set of report builders —
	// is two implementations of one answer, and during a migration where both benches
	// are up at once, two answers that can disagree is the exact failure this project
	// keeps paying for.
	//
	// a_path is a route as the HTTP side spells it ("/scope", "/config/set", ...) and
	// a_params are what would have been the query string, already decoded. Unknown
	// routes come back as the same {"ok":false,...} error the listener produces.
	//
	// THREADING is unchanged and is the caller's problem, exactly as it is for the
	// listener thread: handlers take snapshots (mutex-guarded) or request work on the
	// render thread, and none of them touch the 3D directly. devbench invokes tool
	// handlers on ITS listener thread, which is the same contract.
	[[nodiscard]] std::string Invoke(
		std::string_view a_path,
		const std::vector<std::pair<std::string, std::string>>& a_params);
}
