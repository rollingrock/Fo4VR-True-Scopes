#include "TrueScopes/ReticleFit.h"
#include "TrueScopes/ReticleShader.h"

#include <Windows.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <limits>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

using namespace TrueScopes::ReticleFit;

namespace
{
	int g_failures = 0;

	void Require(bool a_value, const char* a_message)
	{
		if (!a_value) {
			std::fprintf(stderr, "FAILED: %s\n", a_message);
			++g_failures;
		}
	}

	bool Near(float a_a, float a_b) { return std::fabs(a_a - a_b) < 1e-5f; }

	void Roles()
	{
		Require(RoleOf("TritiumDot002:0", "", "textures/Weapons/10mmPistol/10mmCircleReticle_d.dds") == Role::kReticle,
			"an inline-material reticle is known by its texture");
		Require(RoleOf("TritiumDot005:0", "", "textures/Effects/GlowSpotLinear.dds") == Role::kOther, "a decorative glow is not a reticle");
		Require(OpticalGlowDot("TritiumDot019:0", "textures/Effects/GlowSpotLinear.dds", true, true), "vanilla 10mm glow dot beside glass");
		Require(OpticalGlowDot("TritiumDot009:0", "textures/Effects/GlowSpotLinear.dds", true, true), "vanilla combat shotgun dot");
		Require(!OpticalGlowDot("TritiumDot019:0", "textures/Effects/GlowSpotLinear.dds", true, false), "an isolated tritium marker is not a reflex sight");
		Require(!OpticalGlowDot("TritiumDot019:0", "textures/Effects/GlowSpotLinear.dds", false, true), "a plain marker beside glass stays native");
		Require(!OpticalGlowDot("FlashlightGlow", "textures/Effects/GlowSpotLinear.dds", true, true), "a flashlight glow is not a reticle");
		Require(RoleOf("ScopeFade:0", "Materials/Scope/LensFadeEffect.bgem") == Role::kOther, "a fade mask is not an aperture");
		Require(RoleOf("CrossHairMesh", "Materials/NewWeapon/Unknown.bgem") == Role::kReticle, "an unseen mod reticle");
		Require(RoleOf("OpticWindow", "Materials/FutureRelease/LensBlue.bgem") == Role::kLens, "an unseen lens material");
		Require(RoleOf("Housing", "Materials/Lenses/OpticBody.bgsm") == Role::kOther, "a folder name does not make a lens");
		Require(RoleOf("Flashlight", "Materials/Gun/Light.bgsm") == Role::kOther, "an emitter is not a reticle");
		Require(RoleOf("Glass", "Reticle.bgem") == Role::kOther, "conflicting evidence stays unknown");
	}

	void Qualification()
	{
		std::array<Vertex, 4> reticle{ { { { -1, 10, -1 }, 0, 1 }, { { 1, 10, -1 }, 1, 1 }, { { 1, 10, 1 }, 1, 0 }, { { -1, 10, 1 }, 0, 0 } } };
		auto lens = reticle;
		auto wall = reticle;
		for (auto& v : lens) {
			v.position.y = 0;
			v.position.x *= 2;
			v.position.z *= 2;
		}
		for (auto& v : wall) {
			v.position.y = 3;
			v.position.x += 4;
		}
		std::array<std::uint16_t, 6> indices{ 0, 1, 2, 0, 2, 3 };
		std::array<Mesh, 3> meshes{ { { reticle, indices, true, false, false }, { lens, indices, true, true, false }, { wall, indices, true, false, true } } };
		Frame frame;
		Require(Qualify(meshes, frame) == Result::kReady, "a clear optic qualifies");
		Require(Near(frame.forward.y, 1) && Near(frame.aspect, 1), "the frame comes from the geometry");

		std::array<Mesh, 2> glassless{ meshes[0], meshes[2] };
		Require(Qualify(glassless, frame) == Result::kReady && frame.lensMesh == 0, "an open holo uses its card as the aperture");
		for (auto& v : wall) {
			v.position.x -= 4;
		}
		Require(Qualify(glassless, frame) == Result::kClosed, "a glassless sight cannot look through a solid part");
		for (auto& v : wall) {
			v.position.x += 4;
		}
		std::vector<Vertex> housing(23937, wall[0]);
		for (std::size_t i = 0; i < wall.size(); ++i) {
			housing[i] = wall[i];
		}
		glassless[1].vertices = housing;
		Require(Qualify(glassless, frame) == Result::kReady, "a SCAR-sized housing is in budget");

		const std::array<Vertex, 5> noisy{ reticle[0], reticle[1], reticle[2], reticle[3], Vertex{ { -.999f, 10, -.999f }, .0001f, .9999f } };
		const std::array<std::uint16_t, 9> fan{ 0, 4, 1, 1, 2, 3, 0, 1, 3 };
		auto quantized = meshes;
		quantized[0] = { noisy, fan, true, false, false };
		Require(Qualify(quantized, frame) == Result::kReady && std::fabs(frame.reticleCenter.x) < .001f, "tiny fan triangles do not skew the fit");

		for (auto& v : wall) {
			v.position.x -= 4;
		}
		Require(Qualify(meshes, frame) == Result::kClosed, "a solid rear card stays vanilla");
		for (auto& v : wall) {
			v.position.x += 4;
		}
		auto front = lens;
		for (auto& v : front) {
			v.position.y = 5;
		}
		std::array<Mesh, 4> layered{ meshes[0], Mesh{ front, indices, true, true, false }, meshes[2], meshes[1] };
		Require(Qualify(layered, frame) == Result::kReady && frame.lensMesh == 3, "of several glass layers the rear one is the aperture");
		layered[3].occluder = true;
		Require(Qualify(layered, frame) == Result::kReady, "glass does not block other glass");
		for (auto& v : wall) {
			v.position.x -= 4;
		}
		meshes[2].occluder = false;
		Require(Qualify(meshes, frame) == Result::kReady, "a transparent overlay is left to scene depth");
		meshes[2].occluder = true;
		for (auto& v : wall) {
			v.position.x += 4;
		}
		reticle[0].u = .25f;
		Require(Qualify(meshes, frame) == Result::kReticleMapping, "a non-affine mapping has no optical frame");
		reticle[0].u = 0;
		indices[0] = 100;
		Require(Qualify(meshes, frame) == Result::kInvalidMesh, "a bad index is caught before use");
		indices[0] = 0;
		lens[0].position.x = std::numeric_limits<float>::quiet_NaN();
		Require(Qualify(meshes, frame) == Result::kInvalidMesh, "a NaN mesh fails closed");
		lens[0].position.x = -2;

		// a rigid turn that puts the view axis on +X
		for (auto* vs : { &reticle, &lens, &wall }) {
			for (auto& v : *vs) {
				const auto p = v.position;
				v.position = { p.y, -p.x, p.z };
			}
		}
		Require(Qualify(meshes, frame, { 1, 0, 0 }) == Result::kReady && Near(frame.forward.x, 1), "model axes do not matter");
		Require(Qualify(std::span(meshes).first(2), frame, { 1, 0, 0 }) == Result::kReady, "no housing sibling is needed");
		for (auto& v : reticle) {
			v.v = 1 - v.v;
		}
		Require(Qualify(meshes, frame, { 1, 0, 0 }) == Result::kReady && frame.uvBounds[3] < 0, "mirrored UVs keep the view side and flip sampling");
	}

	void Aim()
	{
		const Vec3 centre{ 0, 10, 0 }, forward{ 0, 1, 0 }, right{ 1, 0, 0 };
		const Target hit{ { 0, 1000, 0 }, true };
		for (float eyeX : { -0.5f, 0.0f, 0.5f, 1.0f }) {
			EyeAim aim;
			float u = 0, v = 0;
			Require(BuildEyeAim({ eyeX, 0, 0 }, centre, forward, right, hit, aim), "an eye with a target ahead");
			Require(ProjectUv(aim, { eyeX * 0.99f, 10, 0 }, 0.016f, u, v) && Near(u, 0.5f) && Near(v, 0.5f), "each eye sees the target at the art's centre");
		}
		EyeAim aim;
		float u = 0, v = 0;
		Require(BuildEyeAim({ 1, 0, 0 }, centre, forward, right, hit, aim), "a moved eye");
		Require(!ProjectUv(aim, centre, 0.016f, u, v), "a dot fixed on the glass is not the moved eye's aim");
		Require(BuildEyeAim({ 0.5f, 0, 0 }, centre, forward, right, { { 0, 1, 0 }, false }, aim), "a direction target");
		Require(ProjectUv(aim, { 0.5f, 10, 0 }, 0.016f, u, v) && Near(u, 0.5f), "infinity: parallel sightlines");
		Require(!BuildEyeAim({ 0, 11, 0 }, centre, forward, right, hit, aim), "an eye ahead of the lens");
		Require(!BuildEyeAim({}, centre, forward, right, { { 0, 5, 0 }, true }, aim), "a hit before the lens");
		Require(!BuildEyeAim({}, centre, forward, right, { { 1, 0, 0 }, false }, aim), "an aim along the glass");
		Require(!BuildEyeAim({}, centre, forward, right, { { 0, -1, 0 }, false }, aim), "an aim backwards");
		Require(!BuildEyeAim({}, centre, forward, right, { { std::numeric_limits<float>::quiet_NaN(), 1, 0 }, true }, aim), "a NaN target");
		Require(!BuildEyeAim({}, centre, {}, right, hit, aim), "no optic normal");
		Require(!ProjectUv(aim, centre, 0, u, v), "zero angular size");
		Require(BuildEyeAim({ 0, -1, 0 }, { 10, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { { 1000, 0, 0 }, true }, aim), "a turned optic");
		Require(ProjectUv(aim, { 10, -0.99f, 0 }, 0.016f, u, v) && Near(u, 0.5f), "a turned optic projects the same");
	}

	void Routing()
	{
		const std::array<std::uint32_t, 3> ends{ 4, 7, 10 };
		for (std::uint32_t mask = 0; mask < 1024; ++mask) {
			std::array<unsigned, 10> visits{};
			unsigned next = 0;
			bool correct = true;
			const bool routed = Route(ends, 10, [&](unsigned i) { return (mask & (1u << i)) != 0; }, [&](unsigned b, unsigned e, bool sel) {
                    correct = correct && b == next && b < e && e <= 10;
                    for (auto boundary : ends) {
                        if (b < boundary && boundary < e) {
                            correct = false;
                        }
                    }
                    if (sel && e != b + 1) {
                        correct = false;
                    }
                    for (unsigned i = b; i < e; ++i) {
                        ++visits[i];
                        correct = correct && sel == ((mask & (1u << i)) != 0);
                    }
                    next = e; });
			if (routed != (mask != 0) || !correct || next != (mask ? 10u : 0u)) {
				Require(false, "a split keeps order, groups and each pass's route");
				return;
			}
			for (const auto n : visits) {
				if (n != (mask ? 1u : 0u)) {
					Require(false, "no draw dropped or doubled around a split");
					return;
				}
			}
		}
		const auto rejected = [&](std::span<const std::uint32_t> a_ends, std::uint32_t a_count) {
			bool touched = false;
			const auto r = Route(a_ends, a_count, [&](unsigned) { touched = true; return true; }, [&](unsigned, unsigned, bool) { touched = true; });
			Require(!r && !touched, "a bad batch stays native before any pass is read");
		};
		rejected({}, 0);
		rejected(ends, 9);
		rejected(ends, kMaxPackets + 1);
		const std::array<std::uint32_t, 3> unordered{ 7, 4, 10 }, duplicate{ 4, 4, 10 }, outOfRange{ 4, 11, 10 };
		rejected(unordered, 10);
		rejected(duplicate, 10);
		rejected(outOfRange, 10);
	}

	void Cache()
	{
		using S = CacheState;
		using A = CacheAction;
		Require(Choose(S::kEmpty, true, true, false) == A::kReload, "an empty optic starts a readback");
		Require(Choose(S::kPending, true, false, false) == A::kReadPending, "a moving attachment does not restart a copy");
		Require(Choose(S::kReady, true, true, true) == A::kReuse, "unchanged geometry is reused");
		Require(Choose(S::kReady, true, false, true) == A::kRefit, "a moved pose refits without a copy");
		Require(Choose(S::kRejected, false, true, false) == A::kReload, "new resources retry a rejection");
		Require(Choose(S::kRejected, true, false, true) == A::kRefit, "a reopened optic recovers");
		Require(Choose(S::kRejected, true, false, false) == A::kReload, "incomplete rejected geometry is not reused");
		Require(Choose(S::kRejected, true, true, true) == A::kKeepRejected, "an unsupported optic does not churn copies");
		Require(Choose(S::kRejected, true, true, false, true) == A::kReload, "a failed copy retries after the delay");
	}

	void VertexTint()
	{
		std::array<std::byte, 9 * 24> bytes{};
		for (std::size_t v = 0; v < 9; ++v) {
			bytes[v * 24] = std::byte(v);
			for (unsigned c = 0; c < 4; ++c) {
				bytes[v * 24 + 20 + c] = std::byte{ 255 };
			}
		}
		std::array<float, 4> tint{};
		Require(ReadUniformTint(bytes, 9, 24, 20, tint) == Tint::kUniform && tint == std::array<float, 4>{ 1, 1, 1, 1 }, "neutral vertex colours");
		for (std::size_t v = 0; v < 9; ++v) {
			bytes[v * 24 + 21] = std::byte{ 128 };
			bytes[v * 24 + 22] = std::byte{ 64 };
			bytes[v * 24 + 23] = std::byte{ 0 };
		}
		Require(ReadUniformTint(bytes, 9, 24, 20, tint) == Tint::kUniform && Near(tint[1], 128.0f / 255.0f) && Near(tint[2], 64.0f / 255.0f) && tint[3] == 0,
			"a constant colour keeps all four channels");
		const auto saved = tint;
		bytes[8 * 24 + 21] = std::byte{ 127 };
		Require(ReadUniformTint(bytes, 9, 24, 20, tint) == Tint::kVarying && tint == saved, "a gradient is not flattened");
		Require(ReadUniformTint(bytes, 9, 24, 22, tint) == Tint::kInvalid, "a read past the vertex");
		Require(ReadUniformTint(std::span(bytes).first(bytes.size() - 1), 9, 24, 20, tint) == Tint::kInvalid, "truncated data");
		Require(ReadUniformTint(bytes, 9, 0, 0, tint) == Tint::kInvalid && ReadUniformTint(bytes, 0, 24, 20, tint) == Tint::kInvalid, "no stride, no vertices");
	}

	void Entries()
	{
		constexpr std::array<std::uint8_t, 8> sig{ 0x48, 0x89, 0x74, 0x24, 0x20, 0x88, 0x54, 0x24 };
		std::array<std::uint8_t, 512> mem{};
		const auto base = reinterpret_cast<std::uintptr_t>(mem.data());
		const auto read = [&](std::uintptr_t p, void* out, std::size_t n) {
			if (p < base || n > mem.size() || p - base > mem.size() - n) {
				return false;
			}
			std::memcpy(out, reinterpret_cast<const void*>(p), n);
			return true;
		};
		const auto exec = [&](std::uintptr_t p) { return p >= base && p < base + mem.size(); };
		std::copy(sig.begin(), sig.end(), mem.begin());
		auto e = InspectEntry(base, sig, read, exec);
		Require(e.valid && !e.detour, "an untouched entry");
		const auto jump = [&](std::int32_t rel) {
			mem[0] = 0xE9;
			std::memcpy(mem.data() + 1, &rel, sizeof(rel));
		};
		jump(251);
		e = InspectEntry(base, sig, read, exec);
		Require(e.valid && e.detour == base + 256, "another mod's five-byte detour is accepted");
		mem[5] ^= 1;
		Require(!InspectEntry(base, sig, read, exec).valid, "a detour over a changed prologue is refused");
		mem[5] ^= 1;
		jump(-5);
		Require(!InspectEntry(base, sig, read, exec).valid, "a self jump");
		jump(0);
		Require(!InspectEntry(base, sig, read, exec).valid, "a jump into the displaced bytes");
		jump(4091);
		Require(!InspectEntry(base, sig, read, exec).valid, "a jump out of executable memory");
		jump(251);
		mem[0] = 0xE8;
		Require(!InspectEntry(base, sig, read, exec).valid, "a call is not a detour");
	}

	// the HLSL compiles and its constant buffer is the size the plugin uploads
	void Shader()
	{
		using Microsoft::WRL::ComPtr;
		const auto compile = [](const char* a_entry, const char* a_profile, ComPtr<ID3DBlob>& a_out) {
			ComPtr<ID3DBlob> errors;
			const auto hr = D3DCompile(TrueScopes::ReticleShader::kSource, sizeof(TrueScopes::ReticleShader::kSource) - 1, "ReticleTest", nullptr, nullptr,
				a_entry, a_profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, a_out.GetAddressOf(), errors.GetAddressOf());
			if (FAILED(hr) && errors) {
				std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
			}
			return SUCCEEDED(hr);
		};
		ComPtr<ID3DBlob> vs, ps, cs;
		Require(compile("VSMain", "vs_5_0", vs) && compile("PSMain", "ps_5_0", ps) && compile("CSVisibility", "cs_5_0", cs), "the reticle HLSL compiles");
		if (!ps) {
			return;
		}
		ComPtr<ID3D11ShaderReflection> reflect;
		Require(SUCCEEDED(D3DReflect(ps->GetBufferPointer(), ps->GetBufferSize(), IID_PPV_ARGS(reflect.GetAddressOf()))), "reflection");
		if (!reflect) {
			return;
		}
		D3D11_SHADER_BUFFER_DESC desc{};
		reflect->GetConstantBufferByName("Optic")->GetDesc(&desc);
		Require(desc.Size == 464, "the Optic constant buffer is 464 bytes, as the plugin's Constants");
	}
}

int main()
{
	Roles();
	Qualification();
	Aim();
	Routing();
	Cache();
	VertexTint();
	Entries();
	Shader();
	if (g_failures) {
		std::fprintf(stderr, "%d reticle fit check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("reticle fit: all checks passed\n");
	return 0;
}
