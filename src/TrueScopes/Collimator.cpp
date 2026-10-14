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

		// VR BSGeometry; the effect shader's setup reads it here (0x1428cf90e)
		constexpr std::uintptr_t kShaderPropertyInGeometry = 0x178;
		constexpr std::uintptr_t kIsGeometryVFunc = 0x40;
		constexpr std::uintptr_t kBSGeometryIsGeometry = 0x1c31c50;  // mov rax, rcx; ret

		// constant group (r12 at the hook): [+0] ID3D11Buffer, [+8] mapped data.
		constexpr std::uintptr_t kMappedData = 0x8;

		// the setup's third argument (rdi at the hook)
		constexpr std::uintptr_t kVertexShader = 0x08;
		constexpr std::uintptr_t kTechnique = 0x40;
		constexpr std::uint32_t  kSkinned = 0x2;  // World is identity, positions come pre-skinned

		// vertex shader constant table, a float offset per engine constant, 0xFF unused
		constexpr std::uintptr_t kWorldSlot = 0x69;
		constexpr std::uintptr_t kEyePositionSlot = 0x6c;

		// render context cached-state block: dirty mask at +0, cull mode at +0xbc
		// (0 none, 1 back). The effect shader turns culling off only for rain and
		// snow, so effect shapes are one-sided whatever their material says - and
		// vanilla reflex panes face the muzzle, invisible from behind.
		constexpr std::uintptr_t kStateBlock = 0x1ee0;
		constexpr std::uintptr_t kCullMode = 0xbc;
		constexpr std::uint32_t  kRasterDirty = 0x8;

		// Fallout4VR.exe spans about 0x6c00000; a vtable or locator outside the
		// image is not one.
		constexpr std::uintptr_t kImageSpan = 0x8000000;

		constexpr std::size_t kMaxGlass = 8;
		constexpr std::uint32_t kRescanFrames = 30;

		struct Glass
		{
			std::uintptr_t geometry;
			std::uintptr_t frame;  // the attach node the sight's shapes hang under
		};

		// Written on the game thread, read on the render thread under a seqlock.
		Glass                      g_glass[kMaxGlass]{};
		std::atomic<std::uint32_t> g_count{ 0 };
		std::atomic<std::uint32_t> g_seq{ 0 };

		constexpr Glass kNone[kMaxGlass]{};

		std::uintptr_t g_lastWeapon = 0;
		std::uint32_t  g_frames = 0;
		std::atomic_bool g_loggedApply{ false };

		// cull mode a glass draw replaced, put back after that draw; -1 = none pending
		thread_local std::int32_t t_cullBefore = -1;

		// Render-thread outcomes while on, logged from Tick. 10-14's first run found
		// the glass but never rewrote a draw; these say which check turned it away.
		struct Diag
		{
			std::atomic<std::uint32_t> calls{ 0 };
			std::atomic<std::uint32_t> skinned{ 0 };
			std::atomic<std::uint32_t> notEnv{ 0 };
			std::atomic<std::uint32_t> miss{ 0 };
			std::atomic<std::uint32_t> noData{ 0 };
			std::atomic<std::uint32_t> fault{ 0 };
			std::atomic<std::uint32_t> applied{ 0 };
		};
		Diag                        g_diag;
		std::atomic<std::uintptr_t> g_drawnGlass[4]{};  // reflex glass seen at the hook but not registered
		std::uint32_t               g_diagFrames = 0;
		std::uint32_t               g_diagLastCalls = 0;

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
			const auto vtbl = *reinterpret_cast<std::uintptr_t*>(a_geom);
			if (!vtbl || *reinterpret_cast<std::uintptr_t*>(vtbl + kIsGeometryVFunc) != REL::Module::get().base() + kBSGeometryIsGeometry) {
				return 0;
			}
			const auto p = *reinterpret_cast<std::uintptr_t*>(a_geom + kShaderPropertyInGeometry);
			return HasTypeName(p, ".?AVBSEffectShaderProperty@@") ? p : 0;
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
				a_out[a_n++] = { a_obj, *reinterpret_cast<std::uintptr_t*>(a_obj + kParentInAVObject) };
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

		bool FindFrame(std::uintptr_t a_geometry, std::uintptr_t& a_frame)
		{
			for (int attempt = 0; attempt < 3; ++attempt) {
				const auto s1 = g_seq.load(std::memory_order_acquire);
				if (s1 & 1) {
					continue;
				}
				const auto n = g_count.load(std::memory_order_relaxed);
				bool       hit = false;
				for (std::uint32_t i = 0; i < n && i < kMaxGlass; ++i) {
					if (g_glass[i].geometry == a_geometry) {
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

		std::uintptr_t RenderState()
		{
			const auto base = REL::Module::get().base();
			auto       ctx = *reinterpret_cast<std::uintptr_t*>(base + Addr::kRenderCtxSlot);
			if (!ctx) {
				ctx = *reinterpret_cast<std::uintptr_t*>(base + Addr::kRenderCtxSlotAlt);
			}
			return ctx ? ctx + kStateBlock : 0;
		}

		// the engine's own rain/snow write (0x1428d0fcd), for this draw only
		void ForceTwoSided()
		{
			const auto st = RenderState();
			if (!st) {
				return;
			}
			auto& cull = *reinterpret_cast<std::uint32_t*>(st + kCullMode);
			if (cull != 0) {
				t_cullBefore = static_cast<std::int32_t>(cull);
				*reinterpret_cast<std::uint32_t*>(st) |= kRasterDirty;
				cull = 0;
			}
		}

		void RestoreCull()
		{
			if (t_cullBefore < 0) {
				return;
			}
			__try {
				if (const auto st = RenderState()) {
					auto& cull = *reinterpret_cast<std::uint32_t*>(st + kCullMode);
					if (cull != static_cast<std::uint32_t>(t_cullBefore)) {
						*reinterpret_cast<std::uint32_t*>(st) |= kRasterDirty;
						cull = static_cast<std::uint32_t>(t_cullBefore);
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			t_cullBefore = -1;
		}

		// The engine writes World as the transpose of the geometry's stored world
		// rotate (times scale), and the shader takes dp3 against each row, so a
		// stored rotate's rows are its axes in world. World -> frame is then M_f
		// itself: rows <- M_f * rows for both eyes. The w column is the row's
		// translation; rotating it too keeps any dp4 against World consistent.
		bool Rewrite(std::uintptr_t a_geometry, std::uintptr_t a_group, std::uintptr_t a_setup)
		{
			__try {
				g_diag.calls.fetch_add(1, std::memory_order_relaxed);
				if (*reinterpret_cast<const std::uint32_t*>(a_setup + kTechnique) & kSkinned) {
					g_diag.skinned.fetch_add(1, std::memory_order_relaxed);
					return false;
				}
				const auto vs = *reinterpret_cast<const std::uintptr_t*>(a_setup + kVertexShader);
				const auto world = *reinterpret_cast<const std::uint8_t*>(vs + kWorldSlot);
				if (world == 0xFF || *reinterpret_cast<const std::uint8_t*>(vs + kEyePositionSlot) == 0xFF) {
					g_diag.notEnv.fetch_add(1, std::memory_order_relaxed);
					return false;  // not an env-mapped draw
				}
				std::uintptr_t frame = 0;
				if (!FindFrame(a_geometry, frame) || !frame) {
					g_diag.miss.fetch_add(1, std::memory_order_relaxed);
					const auto property = EffectProperty(a_geometry);
					if (property && IsReflexGlass(property)) {
						for (auto& slot : g_drawnGlass) {
							std::uintptr_t expected = 0;
							if (slot.load(std::memory_order_relaxed) == a_geometry ||
								slot.compare_exchange_strong(expected, a_geometry)) {
								break;
							}
						}
					}
					return false;
				}
				ForceTwoSided();
				const auto* r = reinterpret_cast<const float*>(frame + kWorldRotate);
				auto*       data = *reinterpret_cast<float**>(a_group + kMappedData);
				if (!data) {
					g_diag.noData.fetch_add(1, std::memory_order_relaxed);
					return false;
				}
				for (int eye = 0; eye < 2; ++eye) {
					float* w = data + world + eye * 12;
					float  rows[3][4];
					std::memcpy(rows, w, sizeof(rows));
					for (int i = 0; i < 3; ++i) {
						for (int j = 0; j < 4; ++j) {
							w[i * 4 + j] = r[i * 4 + 0] * rows[0][j] + r[i * 4 + 1] * rows[1][j] + r[i * 4 + 2] * rows[2][j];
						}
					}
				}
				g_diag.applied.fetch_add(1, std::memory_order_relaxed);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				g_diag.fault.fetch_add(1, std::memory_order_relaxed);
				return false;  // a dangling frame or group pointer: leave this draw alone
			}
		}

		// Render thread, every effect-shader draw.
		void OnEffectSetup(std::uintptr_t a_geometry, std::uintptr_t a_group, std::uintptr_t a_setup)
		{
			if (!*Settings::reflexCollimatorEnabled || g_count.load(std::memory_order_relaxed) == 0) {
				return;
			}
			if (Rewrite(a_geometry, a_group, a_setup) && !g_loggedApply.exchange(true)) {
				logger::info("collimator: first glass draw rewritten into sight axes"sv);
			}
		}

		// At the site rbx holds the drawn geometry (just compared against the
		// slot-6 cache), r12 the mapped VS constant group and rdi the setup's third
		// argument; all callee-saved and the same on every path in. Hand them over,
		// then run the original call.
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
				mov(r8, rdi);
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

		// RestoreGeometry runs after the draw; put back what ForceTwoSided changed.
		struct RestoreGeometryHook
		{
			static void thunk(void* a_shader, std::uintptr_t a_pass)
			{
				func(a_shader, a_pass);
				RestoreCull();
			}
			static inline REL::Relocation<void (*)(void*, std::uintptr_t)> func;
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
		// without the restore the cull change would outlive the glass draw
		REL::Relocation<std::uintptr_t> vtbl{ REL::Offset(Addr::kBSEffectShaderVtbl) };
		const auto restoreSlot = reinterpret_cast<std::uintptr_t*>(vtbl.address()) + 10;
		if (*restoreSlot != REL::Module::get().base() + Addr::kEffectRestoreGeometry) {
			logger::warn("collimator NOT installed (BSEffectShader::RestoreGeometry is not the vanilla one)"sv);
			return;
		}
		RestoreGeometryHook::func = vtbl.write_vfunc(10, RestoreGeometryHook::thunk);

		SetupStub stub(reinterpret_cast<std::uintptr_t>(&OnEffectSetup),
			REL::Module::get().base() + Addr::kEffectSetupPreUnmapTarget);
		auto& trampoline = F4SE::GetTrampoline();
		const auto code = trampoline.allocate(stub);
		trampoline.write_call<5>(site.address(), reinterpret_cast<std::uintptr_t>(code));
		logger::info("collimator hooks installed (effect setup + restore, off until reflexCollimatorEnabled)"sv);
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
		if (++g_diagFrames >= 270) {
			g_diagFrames = 0;
			const auto calls = g_diag.calls.load(std::memory_order_relaxed);
			if (calls != g_diagLastCalls || g_count.load(std::memory_order_relaxed)) {
				g_diagLastCalls = calls;
				logger::info(FMT_STRING("collimator diag: calls={} skinned={} notEnv={} miss={} noData={} fault={} applied={} "
										"registered={:x},{:x} drawnGlass={:x},{:x},{:x},{:x}"),
					calls, g_diag.skinned.load(), g_diag.notEnv.load(), g_diag.miss.load(), g_diag.noData.load(),
					g_diag.fault.load(), g_diag.applied.load(), g_glass[0].geometry, g_glass[1].geometry,
					g_drawnGlass[0].load(), g_drawnGlass[1].load(), g_drawnGlass[2].load(), g_drawnGlass[3].load());
			}
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
			logger::info(FMT_STRING("collimator: {} reflex glass shape(s) on the equipped weapon (weapon {:x}; {:x} under {:x}, {:x} under {:x})"),
				n, weapon, found[0].geometry, found[0].frame, found[1].geometry, found[1].frame);
		}
	}
}
