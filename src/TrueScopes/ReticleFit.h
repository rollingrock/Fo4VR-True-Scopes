#pragma once

// The reflex reticle's geometry and policy, no engine types: which shapes are a
// reticle or a lens, the optical frame fitted from the reticle card's UVs, each
// eye's aim through the glass, the cached-batch split. From Bruno's corrected
// reticles (ROCK_Emitters). tests/ReticleFitTests.cpp compiles this file as is.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace TrueScopes::ReticleFit
{
	inline constexpr std::size_t kMaxMeshes = 16;
	inline constexpr std::size_t kMaxVertices = 65535;  // 16-bit indices
	inline constexpr std::size_t kMaxTriangles = 32768;
	inline constexpr std::uint32_t kMaxPackets = 4096;

	struct Vec3
	{
		float x, y, z;
	};

	namespace detail
	{
		inline Vec3 Add(Vec3 a_a, Vec3 a_b) { return { a_a.x + a_b.x, a_a.y + a_b.y, a_a.z + a_b.z }; }
		inline Vec3 Sub(Vec3 a_a, Vec3 a_b) { return { a_a.x - a_b.x, a_a.y - a_b.y, a_a.z - a_b.z }; }
		inline Vec3 Mul(Vec3 a_a, float a_f) { return { a_a.x * a_f, a_a.y * a_f, a_a.z * a_f }; }
		inline float Dot(Vec3 a_a, Vec3 a_b) { return a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z; }
		inline Vec3 Cross(Vec3 a_a, Vec3 a_b) { return { a_a.y * a_b.z - a_a.z * a_b.y, a_a.z * a_b.x - a_a.x * a_b.z, a_a.x * a_b.y - a_a.y * a_b.x }; }
		inline float Length(Vec3 a_a) { return std::sqrt(Dot(a_a, a_a)); }
		inline bool Finite(Vec3 a_a) { return std::isfinite(a_a.x) && std::isfinite(a_a.y) && std::isfinite(a_a.z); }

		inline bool Normalize(Vec3 a_in, Vec3& a_out)
		{
			const float l2 = Dot(a_in, a_in);
			if (!Finite(a_in) || !std::isfinite(l2) || l2 <= 1e-8f) {
				a_out = {};
				return false;
			}
			a_out = Mul(a_in, 1.0f / std::sqrt(l2));
			return Finite(a_out);
		}

		inline bool Contains(std::string_view a_s, std::string_view a_token)
		{
			if (a_s.size() > 1024 || a_s.size() < a_token.size()) {
				return false;
			}
			for (std::size_t i = 0; i + a_token.size() <= a_s.size(); ++i) {
				bool match = true;
				for (std::size_t j = 0; match && j < a_token.size(); ++j) {
					const char c = a_s[i + j];
					match = (c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : c) == a_token[j];
				}
				if (match) {
					return true;
				}
			}
			return false;
		}

		inline std::string_view BaseName(std::string_view a_path)
		{
			const auto slash = a_path.find_last_of("/\\");
			return slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);
		}
	}

	// Names say what a shape might be, never which mod it came from.
	enum class Role
	{
		kOther,
		kReticle,
		kLens,
	};

	inline Role Hint(std::string_view a_s)
	{
		using detail::Contains;
		if (Contains(a_s, "retic") || Contains(a_s, "retuc") || Contains(a_s, "crosshair") || Contains(a_s, "red_dot") ||
			Contains(a_s, "reddot")) {
			return Role::kReticle;
		}
		if (Contains(a_s, "fade") || Contains(a_s, "parallax")) {
			return Role::kOther;
		}
		if (Contains(a_s, "glass") || Contains(a_s, "lens")) {
			return Role::kLens;
		}
		return Role::kOther;
	}

	// node name, then material file name, then texture file name; node and
	// material disagreeing is no evidence at all
	inline Role RoleOf(std::string_view a_node, std::string_view a_material, std::string_view a_texture = {})
	{
		const auto a = Hint(a_node);
		const auto b = Hint(detail::BaseName(a_material));
		if (a != Role::kOther && b != Role::kOther && a != b) {
			return Role::kOther;
		}
		if (a != Role::kOther) {
			return a;
		}
		if (b != Role::kOther) {
			return b;
		}
		return Hint(detail::BaseName(a_texture));
	}

	// Vanilla reflex dots are TritiumDot shapes with GlowSpotLinear art, which iron
	// sight markers use too. Only one with angular falloff beside glass is a reticle.
	inline bool OpticalGlowDot(std::string_view a_node, std::string_view a_texture, bool a_angularFalloff, bool a_lensSibling)
	{
		return a_angularFalloff && a_lensSibling && detail::Contains(a_node, "tritiumdot") &&
		       detail::Contains(detail::BaseName(a_texture), "glowspot");
	}

	struct Vertex
	{
		Vec3 position;
		float u, v;
	};

	struct Mesh
	{
		std::span<const Vertex> vertices;
		std::span<const std::uint16_t> indices;
		bool twoSided;
		bool lens;
		bool occluder;
	};

	struct Frame
	{
		Vec3 lensCenter{};
		Vec3 reticleCenter{};
		Vec3 forward{};
		Vec3 right{};
		float aspect = 1.0f;
		std::array<float, 4> uvBounds{ 0.0f, 0.0f, 1.0f, 1.0f };
		std::size_t lensMesh = 0;  // 0 = no glass, the reticle card itself is the aperture
	};

	enum class Result
	{
		kReady,
		kInvalidMesh,
		kReticleMapping,
		kAlignment,
		kClosed,
	};

	inline const char* ResultName(Result a_r)
	{
		switch (a_r) {
		case Result::kReady:
			return "ready";
		case Result::kInvalidMesh:
			return "invalid-or-large-mesh";
		case Result::kReticleMapping:
			return "unsupported-reticle-mapping";
		case Result::kAlignment:
			return "unaligned-optical-surfaces";
		case Result::kClosed:
			return "closed-or-obstructed-channel";
		}
		return "unknown";
	}

	namespace detail
	{
		// an infinite line along the view axis; a one-sided housing blocks only
		// with its front faces
		inline bool Intersects(const Mesh& a_m, Vec3 a_p, Vec3 a_dir)
		{
			for (std::size_t i = 0; i + 2 < a_m.indices.size(); i += 3) {
				const auto a = a_m.vertices[a_m.indices[i]].position;
				const auto b = Sub(a_m.vertices[a_m.indices[i + 1]].position, a);
				const auto c = Sub(a_m.vertices[a_m.indices[i + 2]].position, a);
				const auto h = Cross(a_dir, c);
				const float d = Dot(b, h);
				if (std::fabs(d) < 1e-7f || (!a_m.twoSided && d < 0.0f)) {
					continue;
				}
				const auto q = Sub(a_p, a);
				const float u = Dot(q, h) / d;
				const float v = Dot(a_dir, Cross(q, b)) / d;
				if (u >= -1e-5f && v >= -1e-5f && u + v <= 1.00001f) {
					return true;
				}
			}
			return false;
		}
	}

	// All meshes in the optic parent's frame; [0] is the reticle card, the rest
	// keep their lens / opaque roles. The reticle's UV frame gives the view axis,
	// the nearest lens crossing it is the aperture, and the reticle is pushed past
	// the whole optic so scene depth clips it at the tube's far end.
	inline Result Qualify(std::span<const Mesh> a_meshes, Frame& a_out, Vec3 a_viewForward = { 0.0f, 1.0f, 0.0f })
	{
		using namespace detail;
		a_out = {};
		if (a_meshes.size() < 2 || a_meshes.size() > kMaxMeshes) {
			return Result::kInvalidMesh;
		}
		std::size_t triangles = 0;
		for (const auto& m : a_meshes) {
			if (m.vertices.size() < 3 || m.vertices.size() > kMaxVertices || m.indices.empty() || m.indices.size() % 3) {
				return Result::kInvalidMesh;
			}
			triangles += m.indices.size() / 3;
			if (triangles > kMaxTriangles) {
				return Result::kInvalidMesh;
			}
			for (const auto& v : m.vertices) {
				if (!Finite(v.position) || !std::isfinite(v.u) || !std::isfinite(v.v)) {
					return Result::kInvalidMesh;
				}
			}
			for (const auto i : m.indices) {
				if (i >= m.vertices.size()) {
					return Result::kInvalidMesh;
				}
			}
		}

		// least-squares UV frame over the whole card: tiny fan triangles amplify
		// half-float error (the Compact Point reticle)
		const auto& reticle = a_meshes[0];
		double mu = 0.0, mv = 0.0;
		double mean[3]{};
		for (const auto& v : reticle.vertices) {
			mu += v.u;
			mv += v.v;
			mean[0] += v.position.x;
			mean[1] += v.position.y;
			mean[2] += v.position.z;
		}
		const double count = static_cast<double>(reticle.vertices.size());
		mu /= count;
		mv /= count;
		for (auto& m : mean) {
			m /= count;
		}
		double uu = 0.0, uv = 0.0, vv = 0.0;
		double pu[3]{}, pv[3]{};
		for (const auto& v : reticle.vertices) {
			const double du = v.u - mu, dv = v.v - mv;
			uu += du * du;
			uv += du * dv;
			vv += dv * dv;
			const double delta[3]{ v.position.x - mean[0], v.position.y - mean[1], v.position.z - mean[2] };
			for (int i = 0; i < 3; ++i) {
				pu[i] += delta[i] * du;
				pv[i] += delta[i] * dv;
			}
		}
		const double det = uu * vv - uv * uv;
		if (!std::isfinite(det) || det <= 1e-12) {
			return Result::kReticleMapping;
		}
		float us[3], vs[3], os[3];
		for (int i = 0; i < 3; ++i) {
			const double u = (pu[i] * vv - pv[i] * uv) / det;
			const double v = (pv[i] * uu - pu[i] * uv) / det;
			us[i] = static_cast<float>(u);
			vs[i] = static_cast<float>(v);
			os[i] = static_cast<float>(mean[i] - u * mu - v * mv);
		}
		const Vec3 du{ us[0], us[1], us[2] }, dv{ vs[0], vs[1], vs[2] }, origin{ os[0], os[1], os[2] };
		const float width = Length(du), height = Length(dv);
		if (width < 1e-4f || height < 1e-4f || !std::isfinite(width + height) || std::fabs(Dot(du, dv)) > width * height * 0.01f) {
			return Result::kReticleMapping;
		}
		float minU = INFINITY, maxU = -INFINITY, minV = INFINITY, maxV = -INFINITY;
		for (const auto& v : reticle.vertices) {
			const auto fitted = Add(origin, Add(Mul(du, v.u), Mul(dv, v.v)));
			if (Length(Sub(v.position, fitted)) > (std::max)(width, height) * 0.03f) {
				return Result::kReticleMapping;
			}
			minU = (std::min)(minU, v.u);
			maxU = (std::max)(maxU, v.u);
			minV = (std::min)(minV, v.v);
			maxV = (std::max)(maxV, v.v);
		}
		if (maxU - minU < 1e-5f || maxV - minV < 1e-5f) {
			return Result::kReticleMapping;
		}
		a_out.uvBounds = { minU, minV, maxU - minU, maxV - minV };
		a_out.reticleCenter = Add(origin, Add(Mul(du, (minU + maxU) * 0.5f), Mul(dv, (minV + maxV) * 0.5f)));
		a_out.right = Mul(du, 1.0f / width);
		auto normal = Cross(du, dv);
		normal = Mul(normal, 1.0f / Length(normal));
		if (!Finite(a_viewForward) || std::fabs(Dot(normal, a_viewForward)) < 1e-5f) {
			return Result::kAlignment;
		}
		a_out.aspect = (width * (maxU - minU)) / (height * (maxV - minV));
		if (Dot(normal, a_viewForward) < 0.0f) {
			normal = Mul(normal, -1.0f);
			a_out.uvBounds[1] += a_out.uvBounds[3];
			a_out.uvBounds[3] = -a_out.uvBounds[3];
		}
		a_out.forward = normal;

		// the rear lens surface on the axis, whatever the glass's shape or count
		float rear = INFINITY;
		for (std::size_t i = 1; i < a_meshes.size(); ++i) {
			const auto& lens = a_meshes[i];
			if (!lens.lens || !Intersects(lens, a_out.reticleCenter, normal)) {
				continue;
			}
			float lo = INFINITY, hi = -INFINITY;
			for (const auto& v : lens.vertices) {
				const float axial = Dot(Sub(v.position, a_out.reticleCenter), normal);
				lo = (std::min)(lo, axial);
				hi = (std::max)(hi, axial);
			}
			const float axial = (lo + hi) * 0.5f;
			if (axial < rear) {
				rear = axial;
				a_out.lensMesh = i;
			}
		}
		// open holo sights have a card and housing but no glass: the card bounds
		// it, and the housing still rejects a closed channel
		if (!a_out.lensMesh) {
			rear = 0.0f;
		}
		a_out.lensCenter = Add(a_out.reticleCenter, Mul(normal, rear));
		for (std::size_t i = 1; i < a_meshes.size(); ++i) {
			if (!a_meshes[i].lens && a_meshes[i].occluder && Intersects(a_meshes[i], a_out.lensCenter, normal)) {
				return Result::kClosed;
			}
		}
		float farthest = 0.0f;
		for (const auto& m : a_meshes) {
			for (const auto& v : m.vertices) {
				farthest = (std::max)(farthest, Dot(Sub(v.position, a_out.reticleCenter), normal));
			}
		}
		a_out.reticleCenter = Add(a_out.reticleCenter, Mul(normal, farthest));
		return Result::kReady;
	}

	// What the reticle sits on: a hit point, or a direction (parallel rays from
	// both eyes, the reticle at infinity).
	struct Target
	{
		Vec3 pointOrDirection{};
		bool hit = false;
	};

	struct EyeAim
	{
		Vec3 eye{};
		Vec3 forward{};
		Vec3 right{};
		Vec3 up{};
	};

	// False with the eye ahead of the lens, the target behind the lens or a
	// direction off the lens's side: no through-lens solution.
	inline bool BuildEyeAim(Vec3 a_eye, Vec3 a_lensCenter, Vec3 a_lensForward, Vec3 a_lensRight, const Target& a_target, EyeAim& a_out)
	{
		using namespace detail;
		a_out = {};
		Vec3 forward{}, right{}, normal{};
		if (!Normalize(a_lensForward, normal) || !Normalize(a_lensRight, right) ||
			!Normalize(a_target.hit ? Sub(a_target.pointOrDirection, a_eye) : a_target.pointOrDirection, forward)) {
			return false;
		}
		const float lensDistance = Dot(Sub(a_lensCenter, a_eye), normal);
		const float facing = Dot(forward, normal);
		if (!std::isfinite(lensDistance) || lensDistance <= 1e-4f || !std::isfinite(facing) || facing <= 1e-4f) {
			return false;
		}
		if (a_target.hit && Dot(Sub(a_target.pointOrDirection, a_lensCenter), normal) <= 1e-4f) {
			return false;
		}
		const float along = Dot(right, forward);
		if (!Normalize(Sub(right, Mul(forward, along)), right)) {
			return false;
		}
		Vec3 up{};
		if (!Normalize(Cross(right, forward), up)) {
			return false;
		}
		a_out = { a_eye, forward, right, up };
		return true;
	}

	// CPU twin of the pixel shader's angular projection
	inline bool ProjectUv(const EyeAim& a_aim, Vec3 a_surface, float a_halfAngleTangent, float& a_u, float& a_v)
	{
		using namespace detail;
		a_u = a_v = 0.0f;
		if (!std::isfinite(a_halfAngleTangent) || a_halfAngleTangent <= 0.0f) {
			return false;
		}
		const auto ray = Sub(a_surface, a_aim.eye);
		const float denom = Dot(ray, a_aim.forward) * (2.0f * a_halfAngleTangent);
		if (!std::isfinite(denom) || denom <= 1e-6f) {
			return false;
		}
		a_u = 0.5f + Dot(ray, a_aim.right) / denom;
		a_v = 0.5f - Dot(ray, a_aim.up) / denom;
		return std::isfinite(a_u) && std::isfinite(a_v) && a_u >= 0.0f && a_u <= 1.0f && a_v >= 0.0f && a_v <= 1.0f;
	}

	// Splits a cached batch so each selected pass runs on its own, keeping group
	// boundaries and order; every other range keeps the native route. Emits
	// nothing (false) for a bad batch or no match, so the caller runs it whole.
	template <class Matches, class Emit>
	bool Route(std::span<const std::uint32_t> a_ends, std::uint32_t a_count, Matches a_matches, Emit a_emit)
	{
		if (!a_count || a_count > kMaxPackets || a_ends.empty() || a_ends.size() > a_count || a_ends.back() != a_count) {
			return false;
		}
		std::uint32_t previous = 0;
		for (const auto end : a_ends) {
			if (end <= previous || end > a_count) {
				return false;
			}
			previous = end;
		}
		bool found = false;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			found = a_matches(i) || found;
		}
		if (!found) {
			return false;
		}
		std::uint32_t begin = 0;
		for (const auto end : a_ends) {
			auto run = begin;
			for (auto i = begin; i < end; ++i) {
				if (!a_matches(i)) {
					continue;
				}
				if (run != i) {
					a_emit(run, i, false);
				}
				a_emit(i, i + 1, true);
				run = i + 1;
			}
			if (run != end) {
				a_emit(run, end, false);
			}
			begin = end;
		}
		return true;
	}

	// The mesh cache. A missed aim sample must not throw away a finished readback;
	// new resources do, a moved attachment only needs a refit.
	enum class CacheState
	{
		kEmpty,
		kPending,
		kReady,
		kRejected,
	};

	enum class CacheAction
	{
		kReload,
		kReadPending,
		kRefit,
		kReuse,
		kKeepRejected,
	};

	constexpr CacheAction Choose(CacheState a_state, bool a_sameResources, bool a_samePose, bool a_buffersReady, bool a_retry = false)
	{
		if (a_state == CacheState::kEmpty || !a_sameResources) {
			return CacheAction::kReload;
		}
		if (a_state == CacheState::kRejected && a_retry) {
			return CacheAction::kReload;
		}
		if (a_state == CacheState::kPending) {
			return CacheAction::kReadPending;
		}
		if (!a_samePose) {
			return a_buffersReady ? CacheAction::kRefit : CacheAction::kReload;
		}
		return a_state == CacheState::kReady ? CacheAction::kReuse : CacheAction::kKeepRejected;
	}

	enum class Tint
	{
		kUniform,
		kVarying,
		kInvalid,
	};

	// A constant R8G8B8A8 vertex colour folds into the material tint exactly; a
	// gradient cannot, so that card keeps its native draw.
	inline Tint ReadUniformTint(std::span<const std::byte> a_vertices, std::size_t a_count, std::size_t a_stride,
		std::size_t a_offset, std::array<float, 4>& a_tint)
	{
		if (!a_count || a_stride < 4 || a_offset > a_stride - 4 || a_count > a_vertices.size() / a_stride) {
			return Tint::kInvalid;
		}
		std::array<std::uint8_t, 4> first{};
		std::memcpy(first.data(), a_vertices.data() + a_offset, first.size());
		for (std::size_t i = 1; i < a_count; ++i) {
			if (std::memcmp(first.data(), a_vertices.data() + i * a_stride + a_offset, first.size()) != 0) {
				return Tint::kVarying;
			}
		}
		for (std::size_t i = 0; i < first.size(); ++i) {
			a_tint[i] = static_cast<float>(first[i]) / 255.0f;
		}
		return Tint::kUniform;
	}

	// A callee we call but never patch. Another mod's MinHook may own its first
	// five bytes (E9); accept that when the rest of the prologue is intact and
	// call the entry, so their hook still runs.
	struct Entry
	{
		bool valid;
		std::uintptr_t detour;  // for the log only
	};

	template <std::size_t N, class Read, class Executable>
	Entry InspectEntry(std::uintptr_t a_address, const std::array<std::uint8_t, N>& a_signature, Read a_read, Executable a_executable)
	{
		constexpr std::size_t kDetour = 5;
		std::array<std::uint8_t, N> bytes{};
		if (!a_read(a_address, bytes.data(), bytes.size()) || !a_executable(a_address)) {
			return {};
		}
		if (bytes == a_signature) {
			return { true, 0 };
		}
		if (bytes[0] != 0xE9 || !std::equal(bytes.begin() + kDetour, bytes.end(), a_signature.begin() + kDetour)) {
			return {};
		}
		std::int32_t rel = 0;
		std::memcpy(&rel, bytes.data() + 1, sizeof(rel));
		const auto target = a_address + kDetour + static_cast<std::intptr_t>(rel);
		if ((target >= a_address && target < a_address + bytes.size()) || !a_executable(target)) {
			return {};
		}
		return { true, target };
	}
}
