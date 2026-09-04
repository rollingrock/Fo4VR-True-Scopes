#include "TrueScopes/WidgetLifecycle.h"

#include <cstdio>

namespace
{
	bool Require(bool a_value, const char* a_message)
	{
		if (!a_value) {
			std::fprintf(stderr, "FAILED: %s\n", a_message);
			return false;
		}
		return true;
	}
}

int main()
{
	TrueScopes::WidgetLifecycle::EpochGate gate;
	const auto initial = gate.Current();
	if (!Require(!gate.Presentable(), "fresh widget must be withheld")) {
		return 1;
	}

	gate.MarkFitted(initial);
	if (!Require(gate.Presentable(), "current generation becomes presentable after fit")) {
		return 1;
	}

	const auto weaponSwap = gate.Invalidate();
	if (!Require(weaponSwap != initial, "weapon swap advances generation") ||
		!Require(!gate.Presentable(), "weapon swap immediately withholds old fit")) {
		return 1;
	}

	gate.MarkFitted(initial);
	if (!Require(!gate.Presentable(), "late old-weapon fit cannot unlock new generation")) {
		return 1;
	}

	gate.MarkFitted(weaponSwap);
	if (!Require(gate.Presentable(), "new weapon becomes presentable after its own fit")) {
		return 1;
	}

	const auto saveLoad = gate.Invalidate();
	gate.Withhold();
	if (!Require(saveLoad != weaponSwap, "save-load advances generation") ||
		!Require(!gate.Presentable(), "save-load remains hidden until refit")) {
		return 1;
	}

	gate.MarkFitted(weaponSwap);
	if (!Require(!gate.Presentable(), "pre-load completion cannot unlock rebuilt widget")) {
		return 1;
	}
	gate.MarkFitted(saveLoad);
	if (!Require(gate.Presentable(), "rebuilt widget unlocks only at load generation")) {
		return 1;
	}

	std::puts("widget lifecycle regression tests passed");
	return 0;
}
