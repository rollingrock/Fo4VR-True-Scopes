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
// arbitrary process memory through /read, which is acceptable for a local dev
// bench and unacceptable in a shipped mod --- see `devbenchEnabled` in
// Settings.h and the release note there.

namespace DevBench
{
	// Start the listener thread. No-op (returns false) when devbenchEnabled is
	// off. Safe to call once, from F4SEPlugin_Load.
	bool Start();

	// Ask the listener to stop and join it. Safe to call if Start() failed.
	void Stop();

	// Port actually bound (0 = not listening).
	std::uint16_t Port();
}
