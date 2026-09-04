#include "TrueScopes/WidgetRotation.h"

#include <cmath>
#include <cstdio>

namespace
{
	constexpr float kPi = 3.14159265358979323846f;

	bool Require(bool a_value, const char* a_message)
	{
		if (!a_value) {
			std::fprintf(stderr, "FAILED: %s\n", a_message);
			return false;
		}
		return true;
	}

	void Multiply(const float (&a_a)[9], const float (&a_b)[9], float (&a_out)[9])
	{
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 3; ++c) {
				a_out[r * 3 + c] = a_a[r * 3 + 0] * a_b[0 * 3 + c] +
				                       a_a[r * 3 + 1] * a_b[1 * 3 + c] +
				                       a_a[r * 3 + 2] * a_b[2 * 3 + c];
			}
		}
	}

	void RotationX(float a_degrees, float (&a_out)[9])
	{
		const float a = a_degrees * kPi / 180.0f;
		const float c = std::cos(a);
		const float s = std::sin(a);
		const float m[9] = { 1.0f, 0.0f, 0.0f, 0.0f, c, -s, 0.0f, s, c };
		for (int i = 0; i < 9; ++i) {
			a_out[i] = m[i];
		}
	}

	void RotationZ(float a_degrees, float (&a_out)[9])
	{
		const float a = a_degrees * kPi / 180.0f;
		const float c = std::cos(a);
		const float s = std::sin(a);
		const float m[9] = { c, -s, 0.0f, s, c, 0.0f, 0.0f, 0.0f, 1.0f };
		for (int i = 0; i < 9; ++i) {
			a_out[i] = m[i];
		}
	}

	bool Near(const float (&a_actual)[9], const float (&a_expected)[9], float a_epsilon = 1.0e-5f)
	{
		for (int i = 0; i < 9; ++i) {
			if (std::fabs(a_actual[i] - a_expected[i]) > a_epsilon) {
				return false;
			}
		}
		return true;
	}
}

int main()
{
	float parent[9];
	float engineLocal[9];
	RotationZ(17.0f, parent);
	RotationX(-8.0f, engineLocal);

	// At the engine baseline the weapon-derived frame and widget world frame
	// agree, so the captured alignment is identity.
	float weaponOneHand[9];
	Multiply(parent, engineLocal, weaponOneHand);

	TrueScopes::WidgetRotation::Calibration calibration;
	calibration.Capture(weaponOneHand, parent, engineLocal);
	if (!Require(calibration.Captured(), "baseline capture must arm tracking")) {
		return 1;
	}

	// Sequence A: scope first, then engage the offhand. The widget follows the
	// weapon-relative pitch introduced by the two-hand grip.
	float twoHandDelta[9];
	float weaponTwoHand[9];
	float expectedLocal[9];
	float actualLocal[9];
	RotationX(31.0f, twoHandDelta);
	Multiply(weaponOneHand, twoHandDelta, weaponTwoHand);
	Multiply(engineLocal, twoHandDelta, expectedLocal);
	if (!Require(calibration.Compute(weaponTwoHand, parent, actualLocal),
			"captured calibration must compute a tracked rotation") ||
		!Require(Near(actualLocal, expectedLocal),
			"scope-then-two-hand must keep the disc on the weapon")) {
		return 1;
	}

	// Sequence B (the field regression): engage two-hand first, then cross the
	// arm edge. The resulting identity probe must not rebuild K against L0.
	calibration.PreserveAcrossIdentityProbe();
	if (!Require(calibration.Compute(weaponTwoHand, parent, actualLocal),
			"identity probe must leave calibration usable") ||
		!Require(Near(actualLocal, expectedLocal),
			"two-hand-then-scope must preserve the pre-probe facing")) {
		return 1;
	}

	// Demonstrate the old failure mode: recapturing at the two-hand pose while
	// feeding the engine's one-hand L0 forces the immediate output back to L0.
	TrueScopes::WidgetRotation::Calibration oldBehavior;
	oldBehavior.Capture(weaponTwoHand, parent, engineLocal);
	if (!Require(oldBehavior.Compute(weaponTwoHand, parent, actualLocal),
			"old behavior model must compute") ||
		!Require(Near(actualLocal, engineLocal),
			"recapture model must reproduce the one-hand snap") ||
		!Require(!Near(actualLocal, expectedLocal),
			"one-hand snap must differ from the tracked two-hand facing")) {
		return 1;
	}

	calibration.Reset();
	if (!Require(!calibration.Compute(weaponTwoHand, parent, actualLocal),
			"real widget lifecycle reset must discard calibration")) {
		return 1;
	}

	std::puts("widget rotation regression tests passed");
	return 0;
}
