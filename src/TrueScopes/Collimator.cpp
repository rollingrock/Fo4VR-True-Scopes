#include "TrueScopes/Collimator.h"

#include "Settings/Settings.h"
#include "TrueScopes/Addresses.h"
#include "TrueScopes/ScopeIdent.h"

#include <xbyak/xbyak.h>

namespace TrueScopes::Collimator
{
	namespace
	{
		// VR NiAVObject / NiNode
		constexpr std::uintptr_t kNameInObject = 0x10;  // BSFixedString
		constexpr std::uintptr_t kParentInAVObject = 0x28;
		constexpr std::uintptr_t kWorldRotate = 0x70;  // NiMatrix3, three rows of 4 floats
		constexpr std::uintptr_t kChildren = 0x168;
		constexpr std::uintptr_t kChildCount = 0x172;  // u16 loop bound, null holes legal

		// BSGeometry's effect property sits somewhere past NiAVObject; the VR layout
		// is not pinned, so it is found once by RTTI and remembered.
		constexpr std::uintptr_t kPropertyScanBegin = 0x120;
		constexpr std::uintptr_t kPropertyScanEnd = 0x160;

		// constant group (r12 at the hook): [+0] ID3D11Buffer, [+8] mapped data.
		// World rows go to data + constantTable[1]*4; c8 for every ENVMAP vertex shader.
		constexpr std::uintptr_t kMappedData = 0x8;
		constexpr std::uint32_t  kWorldRegister = 8;

		// Fallout4VR.exe spans about 0x6c00000; a vtable or locator outside the
		// image is not one.
		constexpr std::uintptr_t kImageSpan = 0x8000000;

		constexpr std::size_t kMaxGlass = 8;
		constexpr std::uint32_t kRescanFrames = 30;

		struct Glass
		{
			std::uintptr_t property;
			std::uintptr_t frame;  // the attach node the sight's shapes hang under
		};

		// Written on the game thread, read on the render thread under a seqlock.
		Glass                      g_glass[kMaxGlass]{};
		std::atomic<std::uint32_t> g_count{ 0 };
		std::atomic<std::uint32_t> g_seq{ 0 };

		constexpr Glass kNone[kMaxGlass]{};

		std::uintptr_t g_lastWeapon = 0;
		std::uint32_t  g_frames = 0;
		std::ptrdiff_t g_propertyOffset = -1;
		std::atomic_bool g_loggedApply{ false };

		bool ReadName(std::uintptr_t a_field, char* a_out, std::size_t a_size)
		{
			a_out[0] = '\0';
			const auto entry = *reinterpret_cast<std::uintptr_t*>(a_field);
			if (entry < 0x10000) {
				return false;
			}
			const auto len = *reinterpret_cast<std::uint32_t*>(entry + 0x10);
			if (len == 0 || len + 1 > a_size) {
				return false;
			}
			const auto* chars = reinterpret_cast<const char*>(entry + 0x18);
			if (chars[len] != '\0') {
				return false;
			}
			std::memcpy(a_out, chars, len + 1);
			return true;
		}

		// vtable[-1] is the complete object locator; +0x0C its type descriptor RVA,
		// the mangled name at descriptor +0x10.
		bool HasTypeName(std::uintptr_t a_obj, const char* a_name)
		{
			if (a_obj < 0x10000) {
				return false;
			}
			const auto base = REL::Module::get().base();
			const auto vtbl = *reinterpret_cast<std::uintptr_t*>(a_obj);
			if (vtbl < base || vtbl >= base + kImageSpan) {
				return false;
			}
			const auto col = *reinterpret_cast<std::uintptr_t*>(vtbl - 8);
			if (col < base || col >= base + kImageSpan) {
				return false;
			}
			const auto td = base + *reinterpret_cast<std::uint32_t*>(col + 0x0C);
			return std::strcmp(reinterpret_cast<const char*>(td + 0x10), a_name) == 0;
		}

		std::uintptr_t EffectProperty(std::uintptr_t a_geom)
		{
			static constexpr const char* kEffect = ".?AVBSEffectShaderProperty@@";
			if (g_propertyOffset >= 0) {
				const auto p = *reinterpret_cast<std::uintptr_t*>(a_geom + g_propertyOffset);
				return HasTypeName(p, kEffect) ? p : 0;
			}
			for (auto off = kPropertyScanBegin; off < kPropertyScanEnd; off += 8) {
				const auto p = *reinterpret_cast<std::uintptr_t*>(a_geom + off);
				if (HasTypeName(p, kEffect)) {
					g_propertyOffset = static_cast<std::ptrdiff_t>(off);
					logger::info(FMT_STRING("collimator: BSGeometry keeps its effect property at +0x{:x}"), off);
					return p;
				}
			}
			return 0;
		}

		bool IsReflexGlass(std::uintptr_t a_property)
		{
			char name[260];
			if (!ReadName(a_property + kNameInObject, name, sizeof(name))) {
				return false;
			}
			for (auto* c = name; *c; ++c) {
				*c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
			}
			return std::strstr(name, "reflexglass") != nullptr;
		}

		void Walk(std::uintptr_t a_obj, Glass (&a_out)[kMaxGlass], std::uint32_t& a_n, int a_depth)
		{
			if (!a_obj || a_depth > 12 || a_n >= kMaxGlass) {
				return;
			}
			if (ScopeIdent::IsNiNode(a_obj)) {
				const auto kids = *reinterpret_cast<std::uintptr_t*>(a_obj + kChildren);
				const auto count = *reinterpret_cast<std::uint16_t*>(a_obj + kChildCount);
				for (std::uint16_t i = 0; kids && i < count; ++i) {
					Walk(*reinterpret_cast<std::uintptr_t*>(kids + i * 8ull), a_out, a_n, a_depth + 1);
				}
				return;
			}
			const auto property = EffectProperty(a_obj);
			if (property && IsReflexGlass(property)) {
				a_out[a_n++] = { property, *reinterpret_cast<std::uintptr_t*>(a_obj + kParentInAVObject) };
			}
		}

		std::uint32_t WalkGuarded(std::uintptr_t a_weapon, Glass (&a_out)[kMaxGlass])
		{
			std::uint32_t n = 0;
			__try {
				Walk(a_weapon, a_out, n, 0);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				n = 0;
			}
			return n;
		}

		void Publish(const Glass (&a_list)[kMaxGlass], std::uint32_t a_n)
		{
			g_seq.fetch_add(1, std::memory_order_acq_rel);  // odd: writing
			for (std::uint32_t i = 0; i < kMaxGlass; ++i) {
				g_glass[i] = i < a_n ? a_list[i] : Glass{};
			}
			g_count.store(a_n, std::memory_order_relaxed);
			g_seq.fetch_add(1, std::memory_order_acq_rel);
		}

		bool FindFrame(std::uintptr_t a_property, std::uintptr_t& a_frame)
		{
			for (int attempt = 0; attempt < 3; ++attempt) {
				const auto s1 = g_seq.load(std::memory_order_acquire);
				if (s1 & 1) {
					continue;
				}
				const auto n = g_count.load(std::memory_order_relaxed);
				bool       hit = false;
				for (std::uint32_t i = 0; i < n && i < kMaxGlass; ++i) {
					if (g_glass[i].property == a_property) {
						a_frame = g_glass[i].frame;
						hit = true;
						break;
					}
				}
				if (g_seq.load(std::memory_order_acquire) == s1) {
					return hit;
				}
			}
			return false;
		}

		// rows <- R^T * rows for both eyes, R = the frame's world rotation (local ->
		// world). The w column is the row's translation; rotating it too keeps every
		// dp4 against World (distances, falloff) consistent with the new axes.
		bool Rewrite(std::uintptr_t a_property, std::uintptr_t a_group)
		{
			__try {
				std::uintptr_t frame = 0;
				if (!FindFrame(a_property, frame) || !frame) {
					return false;
				}
				const auto* r = reinterpret_cast<const float*>(frame + kWorldRotate);
				auto*       data = *reinterpret_cast<float**>(a_group + kMappedData);
				if (!data) {
					return false;
				}
				for (int eye = 0; eye < 2; ++eye) {
					float* w = data + (kWorldRegister + eye * 3) * 4;
					float  rows[3][4];
					std::memcpy(rows, w, sizeof(rows));
					for (int i = 0; i < 3; ++i) {
						for (int j = 0; j < 4; ++j) {
							w[i * 4 + j] = r[0 * 4 + i] * rows[0][j] + r[1 * 4 + i] * rows[1][j] + r[2 * 4 + i] * rows[2][j];
						}
					}
				}
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;  // a dangling frame or group pointer: leave this draw alone
			}
		}

		// Render thread, every effect-shader draw.
		void OnEffectSetup(std::uintptr_t a_property, std::uintptr_t a_group)
		{
			if (!*Settings::reflexCollimatorEnabled || g_count.load(std::memory_order_relaxed) == 0) {
				return;
			}
			if (Rewrite(a_property, a_group) && !g_loggedApply.exchange(true)) {
				logger::info("collimator: first glass draw rewritten into sight axes"sv);
			}
		}

		// At the site rbx holds the drawn shape's shader property (just compared
		// against the slot-6 cache) and r12 the mapped VS constant group; both are
		// callee-saved. Hand them over, then run the original call.
		struct SetupStub : Xbyak::CodeGenerator
		{
			SetupStub(std::uintptr_t a_fn, std::uintptr_t a_original)
			{
				Xbyak::Label original;

				push(rcx);
				push(rdx);
				push(r8);
				push(r9);
				push(r10);
				push(r11);
				push(rax);  // 7 pushes on a call-misaligned stack: aligned again
				sub(rsp, 0x80);
				for (int i = 0; i < 6; ++i) {
					movdqu(ptr[rsp + 0x20 + i * 0x10], Xbyak::Xmm(i));
				}
				mov(rcx, rbx);
				mov(rdx, r12);
				mov(rax, a_fn);
				call(rax);
				for (int i = 0; i < 6; ++i) {
					movdqu(Xbyak::Xmm(i), ptr[rsp + 0x20 + i * 0x10]);
				}
				add(rsp, 0x80);
				pop(rax);
				pop(r11);
				pop(r10);
				pop(r9);
				pop(r8);
				pop(rdx);
				pop(rcx);
				jmp(ptr[rip + original]);

				L(original);
				dq(a_original);
			}
		};
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> site{ REL::Offset(Addr::kEffectSetupPreUnmapCallSite) };
		static constexpr std::uint8_t   kOrig[] = { 0xE8, 0xA3, 0x2D, 0x4D, 0xFF };
		if (std::memcmp(reinterpret_cast<const void*>(site.address()), kOrig, sizeof(kOrig)) != 0) {
			logger::warn("collimator NOT installed (byte mismatch at the effect setup site)"sv);
			return;
		}
		SetupStub stub(reinterpret_cast<std::uintptr_t>(&OnEffectSetup),
			REL::Module::get().base() + Addr::kEffectSetupPreUnmapTarget);
		auto& trampoline = F4SE::GetTrampoline();
		const auto code = trampoline.allocate(stub);
		trampoline.write_call<5>(site.address(), reinterpret_cast<std::uintptr_t>(code));
		logger::info("collimator hook installed (effect setup, off until reflexCollimatorEnabled)"sv);
	}

	void Tick(std::uintptr_t a_player)
	{
		if (!*Settings::reflexCollimatorEnabled) {
			if (g_count.load(std::memory_order_relaxed)) {
				Publish(kNone, 0);
				g_lastWeapon = 0;
			}
			return;
		}
		const auto weapon = ScopeIdent::FirstPersonWeaponNode(a_player);
		if (weapon == g_lastWeapon && ++g_frames < kRescanFrames) {
			return;
		}
		g_frames = 0;
		const bool changed = weapon != g_lastWeapon;
		g_lastWeapon = weapon;

		Glass      found[kMaxGlass]{};
		const auto n = WalkGuarded(weapon, found);
		Publish(found, n);
		if (changed) {
			logger::info(FMT_STRING("collimator: {} reflex glass shape(s) on the equipped weapon"), n);
		}
	}
}
