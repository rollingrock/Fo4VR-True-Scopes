#pragma once

// DevBench - the plugin's diagnostic routes (config, state, addresses, log
// tail, scope probe, OMOD enumeration), so a debug question does not cost a
// rebuild + relaunch + headset-on cycle.
//
// Modelled on alandtse/devbench, which is GPL-3.0-or-later, so none of its
// source is vendored here - only the shape of the idea: a name->handler
// registry answering hand-built JSON, no JSON library, inputs pre-decoded as
// key/value pairs. Some routes read arbitrary process memory; this is dev
// tooling, not shipping code.

namespace DevBench
{
	// One-time init (uptime baseline + the settings post-load hook). There is no
	// HTTP listener; every route is served through the 'scope' tool registered on
	// the devbench host (:8931) via DevBenchClient. Call once, from
	// F4SEPlugin_Load.
	void Init();

	// Run one request and return its JSON body. The `scope` tool registered into
	// devbench (see src/DevBenchClient/) answers by running these handlers, so
	// there is exactly one implementation of every answer.
	//
	// a_path is a route in the old HTTP spelling ("/scope", "/config/set", ...) and
	// a_params are the decoded query parameters. Unknown routes come back as the
	// usual {"ok":false,...} error.
	//
	// Threading is the caller's problem: handlers take snapshots (mutex-guarded) or
	// request work on the render thread, and none of them touch the 3D directly.
	// devbench invokes tool handlers on its listener thread.
	[[nodiscard]] std::string Invoke(
		std::string_view a_path,
		const std::vector<std::pair<std::string, std::string>>& a_params);
}
