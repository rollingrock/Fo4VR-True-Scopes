#pragma once

// v0.2.124 — quaternion One Euro filter (Casiez et al. 2012) for the scope
// camera's world orientation (STATUS 3.7d: magnification amplifies hand
// tremor ~6x at 4x zoom; a speed-adaptive cutoff crushes at-rest tremor
// without lagging deliberate pans — a fixed EMA cannot do both).
//
// Numerics per the adversarial review: ALL angle computations use the
// chord/atan2 form (2*atan2(|vec(dq)|, |w(dq)|)) — the naive 2*acos(|dot|)
// quantizes in ~0.03 rad/s steps at tremor scale in float32 and pollutes the
// adaptive derivative. |w| makes every angle double-cover-invariant, so no
// explicit hemisphere management is needed outside Slerp (which aligns
// internally). Header-only, POD, render-thread single-consumer.
//
// Basis convention: the 9 rotation floats live in NiPoint4 rows at +0x00,
// +0x10, +0x20 of the matrix block (m[0..2], m[4..6], m[8..10]); the pad
// floats m[3]/m[7]/m[11] are NEVER written. QuatFromBasis/BasisFromQuat are
// exact inverses over that layout, which is all correctness requires — the
// project's transposed-on-read question is conjugation-invariant here.

#include <cmath>
#include <cstring>

namespace OneEuro
{
	inline float Alpha(float a_dt, float a_fc) noexcept
	{
		const float r = 6.2831853f * a_fc * a_dt;
		return r / (r + 1.0f);
	}

	// q = {x, y, z, w}
	inline void Normalize(float* a_q) noexcept
	{
		const float n = std::sqrt(a_q[0] * a_q[0] + a_q[1] * a_q[1] + a_q[2] * a_q[2] + a_q[3] * a_q[3]);
		if (n > 1.0e-12f) {
			const float inv = 1.0f / n;
			for (int i = 0; i < 4; ++i) {
				a_q[i] *= inv;
			}
		} else {
			a_q[0] = a_q[1] = a_q[2] = 0.0f;
			a_q[3] = 1.0f;
		}
	}

	// Shepperd's method over the row layout described above.
	inline void QuatFromBasis(const float* a_m, float* a_q) noexcept
	{
		const float m00 = a_m[0], m01 = a_m[1], m02 = a_m[2];
		const float m10 = a_m[4], m11 = a_m[5], m12 = a_m[6];
		const float m20 = a_m[8], m21 = a_m[9], m22 = a_m[10];
		const float tr = m00 + m11 + m22;
		if (tr > 0.0f) {
			const float s = std::sqrt(tr + 1.0f) * 2.0f;
			a_q[3] = 0.25f * s;
			a_q[0] = (m21 - m12) / s;
			a_q[1] = (m02 - m20) / s;
			a_q[2] = (m10 - m01) / s;
		} else if (m00 > m11 && m00 > m22) {
			const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
			a_q[3] = (m21 - m12) / s;
			a_q[0] = 0.25f * s;
			a_q[1] = (m01 + m10) / s;
			a_q[2] = (m02 + m20) / s;
		} else if (m11 > m22) {
			const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
			a_q[3] = (m02 - m20) / s;
			a_q[0] = (m01 + m10) / s;
			a_q[1] = 0.25f * s;
			a_q[2] = (m12 + m21) / s;
		} else {
			const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
			a_q[3] = (m10 - m01) / s;
			a_q[0] = (m02 + m20) / s;
			a_q[1] = (m12 + m21) / s;
			a_q[2] = 0.25f * s;
		}
		Normalize(a_q);
	}

	// Writes ONLY the 9 rotation floats; pads m[3]/m[7]/m[11] untouched.
	inline void BasisFromQuat(const float* a_q, float* a_m) noexcept
	{
		const float x = a_q[0], y = a_q[1], z = a_q[2], w = a_q[3];
		a_m[0] = 1.0f - 2.0f * (y * y + z * z);
		a_m[1] = 2.0f * (x * y - z * w);
		a_m[2] = 2.0f * (x * z + y * w);
		a_m[4] = 2.0f * (x * y + z * w);
		a_m[5] = 1.0f - 2.0f * (x * x + z * z);
		a_m[6] = 2.0f * (y * z - x * w);
		a_m[8] = 2.0f * (x * z - y * w);
		a_m[9] = 2.0f * (y * z + x * w);
		a_m[10] = 1.0f - 2.0f * (x * x + y * y);
	}

	// Angle between two unit quaternions via the chord form — double-cover
	// invariant (|w|), numerically clean at tremor scale.
	inline float Angle(const float* a_a, const float* a_b) noexcept
	{
		// dq = conj(a) * b
		const float ax = -a_a[0], ay = -a_a[1], az = -a_a[2], aw = a_a[3];
		const float bx = a_b[0], by = a_b[1], bz = a_b[2], bw = a_b[3];
		const float w = aw * bw - ax * bx - ay * by - az * bz;
		const float x = aw * bx + ax * bw + ay * bz - az * by;
		const float y = aw * by - ax * bz + ay * bw + az * bx;
		const float z = aw * bz + ax * by - ay * bx + az * bw;
		return 2.0f * std::atan2(std::sqrt(x * x + y * y + z * z), std::fabs(w));
	}

	// out = slerp(a, b, t) with hemisphere alignment; nlerp below the
	// numerical-degeneracy threshold (the common tremor-scale case).
	inline void Slerp(const float* a_a, const float* a_b, float a_t, float* a_out) noexcept
	{
		float b[4] = { a_b[0], a_b[1], a_b[2], a_b[3] };
		float d = a_a[0] * b[0] + a_a[1] * b[1] + a_a[2] * b[2] + a_a[3] * b[3];
		if (d < 0.0f) {
			for (int i = 0; i < 4; ++i) {
				b[i] = -b[i];
			}
			d = -d;
		}
		if (d > 0.9995f) {
			for (int i = 0; i < 4; ++i) {
				a_out[i] = a_a[i] + (b[i] - a_a[i]) * a_t;
			}
		} else {
			const float th = std::acos(d);
			const float s = std::sin(th);
			const float wa = std::sin((1.0f - a_t) * th) / s;
			const float wb = std::sin(a_t * th) / s;
			for (int i = 0; i < 4; ++i) {
				a_out[i] = a_a[i] * wa + b[i] * wb;
			}
		}
		Normalize(a_out);
	}

	struct QuatFilter
	{
		float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };         // filtered orientation
		float qRawPrev[4] = { 0.0f, 0.0f, 0.0f, 1.0f };  // last raw (derivative)
		float omegaHat = 0.0f;                           // low-passed angular speed, rad/s
		bool  primed = false;
	};
}
