#pragma once

#include <cstddef>
#include <cstring>

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

		// An aimed capture was taken with the scope armed and the eye on the tube;
		// a provisional one only with the weapon still. Provisional K is re-taken at
		// every adoption (the pre-cache behaviour, which is fine at load); aimed K
		// is cached for the scope and never re-learned. One provisional capture
		// made permanent by the cache tilted a whole session (2026-09-17).
		[[nodiscard]] bool Aimed() const noexcept
		{
			return aimed_;
		}

		void Reset() noexcept
		{
			captured_ = false;
			aimed_ = false;
		}

		// Deliberately preserves K. Scope-in requests an identity probe, including
		// when FRIK has already re-aimed the weapon from a two-hand grip. Rebuilding
		// K against the engine's one-hand L0 in that pose snaps the disc off-axis.
		void PreserveAcrossIdentityProbe() noexcept {}

		// K is the mesh-to-disc rotation: a constant per weapon and grip. Restoring a
		// cached K across an adoption is what keeps a carry edge, a FRIK rewrite or a
		// Pip-Boy transit from re-learning it against a local that is our own previous
		// write and a relation that has not settled yet.
		void Restore(const float (&a_alignment)[9]) noexcept
		{
			std::memcpy(alignment_, a_alignment, sizeof(alignment_));
			captured_ = true;
			aimed_ = true;  // only aimed captures are ever cached
		}

		[[nodiscard]] bool Export(float (&a_out)[9]) const noexcept
		{
			if (!captured_) {
				return false;
			}
			std::memcpy(a_out, alignment_, sizeof(alignment_));
			return true;
		}

		void Capture(const float (&a_weaponWorld)[9], const float (&a_parentWorld)[9],
			const float (&a_engineLocal)[9], bool a_aimed) noexcept
		{
			float parentTimesLocal[9];
			Multiply(a_parentWorld, a_engineLocal, parentTimesLocal);
			MultiplyTransposeLeft(a_weaponWorld, parentTimesLocal, alignment_);
			captured_ = true;
			aimed_ = a_aimed;
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
		bool  aimed_ = false;
		float alignment_[9] = {};
	};
}
