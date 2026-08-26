#pragma once

// True Scopes as a consumer of alandtse/devbench.
//
// devbench is the general test bench: one tool registry reflected onto both MCP
// and REST, with a cross-plugin C-ABI that lets any other plugin register its own
// tools into that same registry. Registering there puts the scope tool next to
// devbench's `console`, `memory`, `rendertarget` and `measure`, so one agent
// session can equip a weapon, force the scope up, dump the lens and read our
// telemetry without three different clients.
//
// If devbench is not installed, every function here is a silent no-op — a research
// tool must never be a load-order requirement.

namespace DevBenchClient
{
	// Fetch the interface and register our tools. Call from F4SE kPostLoad: the
	// interface is a synchronous dispatch, and devbench's provider is wired up at its
	// own load, so kPostLoad is late enough. Safe to call when devbench is absent.
	void Register();

	// True once the interface was obtained AND at least one tool registered.
	[[nodiscard]] bool Connected();
}
