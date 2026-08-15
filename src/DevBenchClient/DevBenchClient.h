#pragma once

// True Scopes as a CONSUMER of alandtse/devbench.
//
// devbench (C:\repos\devbench) is the general test bench: one tool registry reflected
// onto both MCP and REST, so an agent can drive and measure a running game without
// alt-tabbing. It exposes a cross-plugin C-ABI that lets any other plugin register its
// own tools into that same registry — which, as of 2026-08-15, works on Fallout (the ABI
// was Skyrim-typed until then).
//
// Registering here rather than growing our own server further is the whole point: a mod's
// tools appear next to devbench's `console`, `memory`, `rendertarget` and `measure`, so
// ONE agent session can equip a weapon, force the scope up, dump the lens and read our
// telemetry without three different clients.
//
// This is additive. Our own :8930 listener (src/DevBench/) keeps running unchanged, and
// both answer by calling the same DevBench::Invoke, so they cannot drift apart while the
// migration is in flight. See TOOLING_INTEGRATION_2026-08-15.md in the investigation repo
// for what gets deleted, and when.
//
// If devbench is not installed, every function here is a silent no-op — a research tool
// must never be a load-order requirement.

namespace DevBenchClient
{
	// Fetch the interface and register our tools. Call from F4SE kPostLoad: the
	// interface is a synchronous dispatch, and devbench's provider is wired up at its
	// own load, so kPostLoad is late enough. Safe to call when devbench is absent.
	void Register();

	// True once the interface was obtained AND at least one tool registered.
	[[nodiscard]] bool Connected();
}
