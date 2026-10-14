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
		constexpr std::uintptr_t kLocalRotate = 0x30;  // NiMatrix3, three rows of 4 floats
		constexpr std::uintptr_t kWorldRotate = 0x70;
		constexpr std::uintptr_t kWorldTranslate = 0xa0;
		constexpr std::uintptr_t kChildren = 0x168;
		constexpr std::uintptr_t kChildCount = 0x172;  // u16 loop bound, null holes legal

		// VR BSGeometry; the effect shader's setup reads it here (0x1428cf90e)
		constexpr std::uintptr_t kShaderPropertyInGeometry = 0x178;
		constexpr std::uintptr_t kIsGeometryVFunc = 0x40;
		constexpr std::uintptr_t kBSGeometryIsGeometry = 0x1c31c50;  // mov rax, rcx; ret

		// immediate setup (slot 9, the call-site stub): rdi = its third argument,
		// r12 = the mapped VS constant group, [+8] data
		constexpr std::uintptr_t kSetupVertexShader = 0x08;
		constexpr std::uintptr_t kSetupTechnique = 0x40;
		constexpr std::uintptr_t kMappedData = 0x08;

		// record setup (slot 3, 0x1428d31f0): the per-pass job FUN_1428aae20 gets a
		// draw record from slot 2 and has slot 3 fill its per-geometry constants.
		// Scene effect shapes, reflex glass included, are drawn this way.
		constexpr std::uintptr_t kPassGeometry = 0x18;
		constexpr std::uintptr_t kPassTechnique = 0x48;
		constexpr std::uintptr_t kRecordVertexShader = 0x60;
		constexpr std::uintptr_t kRecordVSGeometryGroup = 0xe0;  // VS constant group 2
		constexpr std::uintptr_t kRecordGroupData = 0x10;

		constexpr std::uint32_t kSkinned = 0x2;  // World is identity, positions come pre-skinned

		// vertex shader constant table, a float offset per engine constant, 0xFF unused
		constexpr std::uintptr_t kWorldSlot = 0x69;
		constexpr std::uintptr_t kEyePositionSlot = 0x6c;

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

		// Written by Tick, read on the render and job threads under a seqlock.
		Glass                      g_glass[kMaxGlass]{};
		std::atomic<std::uint32_t> g_count{ 0 };
		std::atomic<std::uint32_t> g_seq{ 0 };

		constexpr Glass kNone[kMaxGlass]{};

		// The effect shader culls back faces for every technique but rain and snow,
		// whatever the material says, and vanilla reflex panes face the muzzle: the
		// shooter never sees them. Turning a pane 180 degrees about its model Z
		// (rows 0 and 1 negated) points it at the eye. Tick's own state.
		struct Flip
		{
			std::uintptr_t geometry;
			float          rows[2][4];  // the local rows it had
		};
		Flip          g_flip[kMaxGlass]{};
		std::uint32_t g_flipCount = 0;

		std::uintptr_t g_lastWeapon = 0;
		std::uint32_t  g_lastCount = 0;
		std::uint32_t  g_frames = 0;
		std::atomic_bool g_loggedApply{ false };

		// Render-side outcomes while on, logged from Tick.
		struct Diag
		{
			std::atomic<std::uint32_t> immediate{ 0 };
			std::atomic<std::uint32_t> record{ 0 };
			std::atomic<std::uint32_t> skinned{ 0 };
			std::atomic<std::uint32_t> notEnv{ 0 };
			std::atomic<std::uint32_t> miss{ 0 };
			std::atomic<std::uint32_t> noData{ 0 };
			std::atomic<std::uint32_t> fault{ 0 };
			std::atomic<std::uint32_t> applied{ 0 };
		};
		Diag          g_diag;
		std::uint32_t g_diagFrames = 0;

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

		// The pane nearest the eye: least far along its frame's +Y (the bore), from
		// world positions. Two panes reflecting the reticle give two dots a dot apart
		// (10-14: their normals differ a little); a real reflex sight has one
		// reflector, and an unturned pane stays culled for the shooter.
		std::uint32_t RearmostPane(const Glass (&a_list)[kMaxGlass], std::uint32_t a_n)
		{
			std::uint32_t best = 0;
			__try {
				float bestY = 0.0f;
				for (std::uint32_t i = 0; i < a_n; ++i) {
					const auto* axisY = reinterpret_cast<const float*>(a_list[i].frame + kWorldRotate + 0x10);
					const auto* pos = reinterpret_cast<const float*>(a_list[i].geometry + kWorldTranslate);
					const float y = pos[0] * axisY[0] + pos[1] * axisY[1] + pos[2] * axisY[2];
					if (i == 0 || y < bestY) {
						best = i;
						bestY = y;
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return best;
		}

		// Point the chosen pane at the eye, once; a pane whose rows came back
		// unflipped (a re-attach rewrites them) is flipped again.
		void FlipToEye(const Glass* a_list, std::uint32_t a_n)
		{
			__try {
				for (std::uint32_t i = 0; i < a_n; ++i) {
					auto* rows = reinterpret_cast<float*>(a_list[i].geometry + kLocalRotate);
					Flip* known = nullptr;
					for (std::uint32_t k = 0; k < g_flipCount; ++k) {
						if (g_flip[k].geometry == a_list[i].geometry) {
							known = &g_flip[k];
						}
					}
					if (known && std::memcmp(rows, known->rows, sizeof(known->rows)) != 0) {
						continue;  // still flipped
					}
					if (!known) {
						if (g_flipCount >= kMaxGlass) {
							continue;
						}
						known = &g_flip[g_flipCount++];
						known->geometry = a_list[i].geometry;
					}
					std::memcpy(known->rows, rows, sizeof(known->rows));
					for (int j = 0; j < 8; ++j) {
						rows[j] = -rows[j];
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}

		// Only while the weapon that owns them is still the one drawn; a pane of
		// a replaced weapon may be freed, so it is forgotten, not restored.
		void Unflip()
		{
			__try {
				for (std::uint32_t k = 0; k < g_flipCount; ++k) {
					auto* rows = reinterpret_cast<float*>(g_flip[k].geometry + kLocalRotate);
					bool  flipped = true;
					for (int j = 0; j < 8; ++j) {
						flipped = flipped && rows[j] == -(&g_flip[k].rows[0][0])[j];
					}
					if (flipped) {
						std::memcpy(rows, g_flip[k].rows, sizeof(g_flip[k].rows));
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			g_flipCount = 0;
		}

		// The engine writes World as the transpose of the geometry's stored world
		// rotate (times scale), and the shader takes dp3 against each row, so a
		// stored rotate's rows are its axes in world. World -> frame is then M_f
		// itself: rows <- M_f * rows for both eyes. The w column is the row's
		// translation; rotating it too keeps any dp4 against World consistent.
		bool Rewrite(std::uintptr_t a_geometry, std::uint32_t a_technique, std::uintptr_t a_vs, float* a_data)
		{
			if (a_technique & kSkinned) {
				g_diag.skinned.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			const auto world = *reinterpret_cast<const std::uint8_t*>(a_vs + kWorldSlot);
			if (world == 0xFF || *reinterpret_cast<const std::uint8_t*>(a_vs + kEyePositionSlot) == 0xFF) {
				g_diag.notEnv.fetch_add(1, std::memory_order_relaxed);
				return false;  // not an env-mapped draw
			}
			std::uintptr_t frame = 0;
			if (!FindFrame(a_geometry, frame) || !frame) {
				g_diag.miss.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			if (!a_data) {
				g_diag.noData.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			const auto* r = reinterpret_cast<const float*>(frame + kWorldRotate);
			for (int eye = 0; eye < 2; ++eye) {
				float* w = a_data + world + eye * 12;
				float  rows[3][4];
				std::memcpy(rows, w, sizeof(rows));
				for (int i = 0; i < 3; ++i) {
					for (int j = 0; j < 4; ++j) {
						w[i * 4 + j] = r[i * 4 + 0] * rows[0][j] + r[i * 4 + 1] * rows[1][j] + r[i * 4 + 2] * rows[2][j];
					}
				}
			}
			g_diag.applied.fetch_add(1, std::memory_order_relaxed);
			if (!g_loggedApply.exchange(true)) {
				logger::info("collimator: first glass draw rewritten into sight axes"sv);
			}
			return true;
		}

		bool Active()
		{
			return *Settings::reflexCollimatorEnabled && g_count.load(std::memory_order_relaxed) != 0;
		}

		// Render thread, every immediate effect-shader draw.
		void OnEffectSetup(std::uintptr_t a_geometry, std::uintptr_t a_group, std::uintptr_t a_setup)
		{
			if (!Active()) {
				return;
			}
			g_diag.immediate.fetch_add(1, std::memory_order_relaxed);
			__try {
				Rewrite(a_geometry,
					*reinterpret_cast<const std::uint32_t*>(a_setup + kSetupTechnique),
					*reinterpret_cast<const std::uintptr_t*>(a_setup + kSetupVertexShader),
					*reinterpret_cast<float**>(a_group + kMappedData));
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				g_diag.fault.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// Job threads, every recorded effect-shader draw, after slot 3 filled it.
		void OnRecordSetup(std::uintptr_t a_pass, std::uintptr_t a_record)
		{
			if (!Active()) {
				return;
			}
			g_diag.record.fetch_add(1, std::memory_order_relaxed);
			__try {
				const auto group = *reinterpret_cast<const std::uintptr_t*>(a_record + kRecordVSGeometryGroup);
				Rewrite(*reinterpret_cast<const std::uintptr_t*>(a_pass + kPassGeometry),
					*reinterpret_cast<const std::uint32_t*>(a_pass + kPassTechnique),
					*reinterpret_cast<const std::uintptr_t*>(a_record + kRecordVertexShader),
					group ? *reinterpret_cast<float**>(group + kRecordGroupData) : nullptr);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				g_diag.fault.fetch_add(1, std::memory_order_relaxed);
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

		struct SetupRecordHook
		{
			static void thunk(void* a_shader, std::uintptr_t a_pass, std::uintptr_t a_record, std::uintptr_t a_job)
			{
				func(a_shader, a_pass, a_record, a_job);
				OnRecordSetup(a_pass, a_record);
			}
			static inline REL::Relocation<void (*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t)> func;
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
		REL::Relocation<std::uintptr_t> vtbl{ REL::Offset(Addr::kBSEffectShaderVtbl) };
		const auto recordSlot = reinterpret_cast<std::uintptr_t*>(vtbl.address()) + 3;
		if (*recordSlot != REL::Module::get().base() + Addr::kEffectSetupGeometryRecord) {
			logger::warn("collimator NOT installed (BSEffectShader slot 3 is not the vanilla record setup)"sv);
			return;
		}
		SetupRecordHook::func = vtbl.write_vfunc(3, SetupRecordHook::thunk);

		SetupStub stub(reinterpret_cast<std::uintptr_t>(&OnEffectSetup),
			REL::Module::get().base() + Addr::kEffectSetupPreUnmapTarget);
		auto& trampoline = F4SE::GetTrampoline();
		const auto code = trampoline.allocate(stub);
		trampoline.write_call<5>(site.address(), reinterpret_cast<std::uintptr_t>(code));
		logger::info("collimator hooks installed (immediate + record setup, off until reflexCollimatorEnabled)"sv);
	}

	void Tick(std::uintptr_t a_player)
	{
		if (!*Settings::reflexCollimatorEnabled) {
			if (g_count.load(std::memory_order_relaxed)) {
				if (ScopeIdent::FirstPersonWeaponNode(a_player) == g_lastWeapon) {
					Unflip();
				}
				g_flipCount = 0;
				Publish(kNone, 0);
				g_lastWeapon = 0;
			}
			return;
		}
		if (++g_diagFrames >= 270) {
			g_diagFrames = 0;
			if (g_count.load(std::memory_order_relaxed)) {
				logger::info(FMT_STRING("collimator diag: immediate={} record={} skinned={} notEnv={} miss={} noData={} fault={} applied={} "
										"registered={:x},{:x} flipped={}"),
					g_diag.immediate.load(), g_diag.record.load(), g_diag.skinned.load(), g_diag.notEnv.load(),
					g_diag.miss.load(), g_diag.noData.load(), g_diag.fault.load(), g_diag.applied.load(),
					g_glass[0].geometry, g_glass[1].geometry, g_flipCount);
			}
		}
		const auto weapon = ScopeIdent::FirstPersonWeaponNode(a_player);
		if (weapon == g_lastWeapon && ++g_frames < kRescanFrames) {
			return;
		}
		g_frames = 0;
		const bool changed = weapon != g_lastWeapon;
		g_lastWeapon = weapon;
		if (changed) {
			g_flipCount = 0;  // the old weapon's panes are not ours to touch any more
		}

		Glass      found[kMaxGlass]{};
		const auto n = WalkGuarded(weapon, found);
		// keep the pane already turned while it is still there; choose only once
		std::uint32_t rear = kMaxGlass;
		for (std::uint32_t i = 0; g_flipCount && i < n; ++i) {
			if (found[i].geometry == g_flip[0].geometry) {
				rear = i;
			}
		}
		if (rear == kMaxGlass && n) {
			g_flipCount = 0;  // the turned pane left this weapon (an attachment swap): not ours now
			rear = RearmostPane(found, n);
		}
		if (n) {
			FlipToEye(&found[rear], 1);
		}
		Publish(found, n);
		if (changed || n != g_lastCount) {
			g_lastCount = n;
			logger::info(FMT_STRING("collimator: {} reflex glass shape(s) on the equipped weapon, pane {:x} turned to face the eye "
									"(weapon {:x}; {:x} under {:x}, {:x} under {:x})"),
				n, n ? found[rear].geometry : 0, weapon, found[0].geometry, found[0].frame, found[1].geometry, found[1].frame);
		}
	}
}
