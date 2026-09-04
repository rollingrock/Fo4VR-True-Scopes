#pragma once

#include <cstddef>

namespace TrueScopes::WidgetRotation
{
	// Weapon-to-widget rotation calibration. Identity probes refresh which optic
	// geometry supplies Rw, but they do not replace ScopeParent or its engine
	// baseline; only a real widget lifecycle reset may discard this calibration.
	class Calibration
	{
	public:
		[[nodiscard]] bool Captured() const noexcept
		{
			return captured_;
		}

		void Reset() noexcept
		{
			captured_ = false;
		}

		// Deliberately preserves K. Scope-in requests an identity probe, including
		// when FRIK has already re-aimed the weapon from a two-hand grip. Rebuilding
		// K against the engine's one-hand L0 in that pose snaps the disc off-axis.
		void PreserveAcrossIdentityProbe() noexcept {}

		void Capture(const float (&a_weaponWorld)[9], const float (&a_parentWorld)[9],
			const float (&a_engineLocal)[9]) noexcept
		{
			float parentTimesLocal[9];
			Multiply(a_parentWorld, a_engineLocal, parentTimesLocal);
			MultiplyTransposeLeft(a_weaponWorld, parentTimesLocal, alignment_);
			captured_ = true;
		}

		[[nodiscard]] bool Compute(const float (&a_weaponWorld)[9],
			const float (&a_parentWorld)[9], float (&a_localOut)[9]) const noexcept
		{
			if (!captured_) {
				return false;
			}
			float weaponTimesAlignment[9];
			Multiply(a_weaponWorld, alignment_, weaponTimesAlignment);
			MultiplyTransposeLeft(a_parentWorld, weaponTimesAlignment, a_localOut);
			return true;
		}

	private:
		static void Multiply(const float (&a_a)[9], const float (&a_b)[9],
			float (&a_out)[9]) noexcept
		{
			for (std::size_t r = 0; r < 3; ++r) {
				for (std::size_t c = 0; c < 3; ++c) {
					a_out[r * 3 + c] = a_a[r * 3 + 0] * a_b[0 * 3 + c] +
					                       a_a[r * 3 + 1] * a_b[1 * 3 + c] +
					                       a_a[r * 3 + 2] * a_b[2 * 3 + c];
				}
			}
		}

		// a_a^T * a_b
		static void MultiplyTransposeLeft(const float (&a_a)[9], const float (&a_b)[9],
			float (&a_out)[9]) noexcept
		{
			for (std::size_t r = 0; r < 3; ++r) {
				for (std::size_t c = 0; c < 3; ++c) {
					a_out[r * 3 + c] = a_a[0 * 3 + r] * a_b[0 * 3 + c] +
					                       a_a[1 * 3 + r] * a_b[1 * 3 + c] +
					                       a_a[2 * 3 + r] * a_b[2 * 3 + c];
				}
			}
		}

		bool  captured_ = false;
		float alignment_[9] = {};
	};
}
