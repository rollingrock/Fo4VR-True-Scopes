#include "TrueScopes/ScopeRender.h"

#include "Settings/Settings.h"
#include "TrueScopes/Addresses.h"

// Phase 2: mono world render from PrimaryWeaponScopeCamera into RT 0x61 -> 0x62.
//
// v0.2.8 rewrite — the recipe now mirrors the engine's own deferred scene render,
// decompiled end-to-end (see SESSION_2026-08-07_DEFERRED_DEEP_DIVE.md in the
// investigation repo):
//   * FUN_140c87320 / FUN_140b03d60 = the engine's two deferred scene renderers.
//     Both use accumulator renderMode 0x19 (deferred). renderMode 0 routes every
//     pass to the FORWARD buckets, which FUN_1427ff8b0 never draws — that is why
//     v0.2.3..v0.2.7 resolved to a black lens no matter where the camera was.
//   * renderer+4 (the scope-pass flag, reader FUN_141d947d0) is checked ~10x inside
//     FUN_1427ff8b0 and reroutes light buffers 0x24/0x25 -> 0x6a/0x6b and DS 1 -> 0xC.
//     We bracket the whole render with renderer+4 = 1 so our pass runs in the exact
//     environment the vanilla scoped world render used (proven live in the Route A demo).
//   * Bind modes (decoded from FUN_141dbd380 + the apply fn FUN_141d9b190):
//     0 = clear-on-apply, 3 = no clear, 4 = CopyResource restore (the v0.2.6 smear).
//     Engine G-buffer binds: slots 0-4 mode 0, slot 5 (0x23) mode 3, byte-verified.
//   * Accumulation = ONE BSShaderUtil::AccumulateScene(cam, ssn, cull, 1) — for an
//     SSN it adds every attached child. No portal lists, no manual subtrees. Lights
//     run BEFORE accumulation (engine order in both templates).
//   * The resolve draws the G-buffer groups itself, then composites into out target
//     (param_5) via the shader-6 quad. Its internal 0x61->0x62 copy is refraction-only;
//     the per-frame lens delivery in vanilla is FUN_1427b08c0(0x61, 0x62, 0) from
//     FUN_14284e370 — we keep doing that explicitly.
// All raw offsets decoded from code bytes (Ghidra high-.data labels are unreliable;
// code bytes are ground truth and match the live process).

namespace TrueScopes::ScopeRender
{
	namespace
	{
		// --- function typedefs (RVAs) ---
		using AccumCtor_t = void* (*)(void*);                                                          // 0x281b790  BSShaderAccumulator::ctor(mem)
		using CullCtor_t = void* (*)(void*, std::uint32_t);                                            // 0x1d4d8e0  BSCullingProcess::ctor(mem, 0)
		using CullDtor_t = void (*)(void*);                                                            // 0x1d4d960  BSCullingProcess::dtor (Ghidra-mislabeled as ctor)
		using CullSetAccum_t = void (*)(void*, void*);                                                 // 0x1d4d9c0  BSCullingProcess::SetAccumulator
		using SetCameraFOV_t = void (*)(std::uintptr_t, float, float, float);                          // 0x2804a90  BSShaderUtil::SetCameraFOV(cam, fovDeg, w, h)
		using BuildIsp_t = void (*)(void*, std::uintptr_t, void*);                                     // 0x2812be0  build ImageSpace param block(buf, cam, accum)
		using ClearPrevCam_t = void (*)(std::uintptr_t);                                               // 0x1d95240  clear prev-frame camera cache(renderer)
		using GetPortalEntry_t = std::uintptr_t (*)();                                                 // 0xd878f0   Main::GetCameraPortalGraphEntry()
		using AccumSceneArray_t = void (*)(std::uintptr_t, std::uintptr_t, void*, std::uint32_t);      // 0x27ff5d0  BSShaderUtil::AccumulateSceneArray(cam, &array, cull, 0)
		using AccumScene_t = void (*)(std::uintptr_t, std::uintptr_t, void*, std::uint32_t);           // 0x27ff370  BSShaderUtil::AccumulateScene(cam, node, cull, 0)
		using ProcessLights_t = void (*)(std::uintptr_t, void*);                                       // 0x27eab40  ShadowSceneNode::ProcessQueuedLights(ssn, cull)
		using DrawAccum_t = void (*)(std::uintptr_t, void*, void*, std::uint8_t);                      // 0x27ff820  draw accumulated groups(cam, accum, isp, 0); finishes+clears passes
		using DeferredResolve_t = void (*)(std::uintptr_t, void*, void*, std::uintptr_t,              // 0x27ff8b0  deferred G-buffer draw + lighting resolve
			std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);                              //            (cam, accum, cull, ssn, outTargetIdx, 1, 0, 1) — flat-screen light-pass pattern
		using FinishAccum_t = void (*)(void*);                                                        // 0x281e750  thunk_FUN_14281ec00: finish + clear pass lists
		using CommitTargetsAlt_t = void (*)(std::uintptr_t);                                          // 0x1db9f80  commit variant used by the MRT light pass
		using VanillaLensCopy_t = void (*)(std::uint32_t, std::uint32_t, std::uint8_t);               // 0x27b08c0  vanilla scope copy chain (0x61, 0x62, 0) — effect 0xf
		using AcquireTarget_t = void (*)(std::uintptr_t, std::int32_t);                                // 0x1dbad90 RT / 0x1dbaea0 DS
		using ReleaseTarget_t = void (*)(std::uintptr_t, std::int32_t);                                // 0x1dbae00 RT / 0x1dbaf10 DS
		using SetCurRT_t = void (*)(std::uintptr_t, std::uint32_t, std::int32_t, std::uint32_t);       // 0x1db9dd0  SetCurrentRenderTarget(mgr, slot, idx, mode)
		using SelectDS_t = void (*)(std::uintptr_t, std::int32_t, std::uint32_t, std::uint32_t);       // 0x1db9e40  select depth-stencil(mgr, dsIdx, mode, slice)
		using CommitTargets_t = void (*)(std::uintptr_t);                                              // 0x1db9fe0
		using ClearColor_t = void (*)(std::uintptr_t);                                                 // 0x1d8dd80  Renderer::ClearColor(renderer)
		using Flush_t = void (*)(std::uintptr_t);                                                      // 0x1d8dc70  Renderer::Flush(renderer)
		using IsmCopy_t = void (*)(std::uint32_t, std::uint32_t);                                      // 0x27b0880  ImageSpaceManager::Copy(src, dst)

		template <class T>
		[[nodiscard]] T Fn(std::uintptr_t a_rva)
		{
			return reinterpret_cast<T>(REL::Module::get().base() + a_rva);
		}

		// --- data (fixed, byte/live-verified this session) ---
		constexpr std::uintptr_t kPlayerGlobal = 0x5b043f0;      // g_player (F4SEVR, live-verified)
		constexpr std::uintptr_t kRTManager = 0x38ac010;         // RenderTargetManager (decoded from SetCurrentRenderTarget + slot-6 bind call sites)
		constexpr std::uintptr_t kRendererRVA = 0x6239340;       // BSGraphics::Renderer (live-verified)
		constexpr std::uintptr_t kCamOffsetInPlayer = 0x720;     // PrimaryWeaponScopeCamera (NiCamera, mono)
		constexpr std::int32_t kTempRT = 0x18;                   // LocalMap's color target
		constexpr std::int32_t kTempDS = 4;                      // LocalMap's depth-stencil

		// --- data resolved at runtime from code anchors ---
		std::uintptr_t g_ssnArray = 0;    // BSShaderManager SSN slot array (slot 0 = world); anchor: lea in SetShadowSceneNode
		std::uintptr_t g_fovMode738 = 0;  // byte: force symmetric-FOV frusta in SetCameraFOV (vanilla scope pass forces 1)
		std::uintptr_t g_fovMode750 = 0;  // dword: which view gets symmetric FOV; 2 = all (vanilla scope pass forces 2)

		void* g_accum = nullptr;
		bool g_available = false;

		// Fault forensics: which step the render was in when the SEH guard fired.
		volatile long g_lastStep = 0;
#define RENDER_STEP(n) g_lastStep = (n)

		// Accumulator layout (from FUN_14281ec00): 38 pass groups, stride 0x678, base
		// accum+0x18; per-group sub-bucket counts at +0x608 + i*0x18 + 0x10 (i = 0..3).
		// Captured POD-side inside the SEH frame, logged from Render() afterwards.
		constexpr std::uint32_t kPassGroupCount = 38;
		std::uint32_t g_passCounts[kPassGroupCount] = {};
		std::uint32_t g_passTotal = 0;

		// The camera's port rect as found before we overwrite it (diagnostics).
		float g_foundPort[4] = {};

		void CapturePassCounts(std::uintptr_t a_accum) noexcept
		{
			g_passTotal = 0;
			for (std::uint32_t g = 0; g < kPassGroupCount; ++g) {
				const auto groupBase = a_accum + 0x18 + static_cast<std::uintptr_t>(g) * 0x678;
				std::uint32_t n = 0;
				for (std::uint32_t i = 0; i < 4; ++i) {
					n += *reinterpret_cast<const std::uint32_t*>(groupBase + 0x608 + i * 0x18 + 0x10);
				}
				g_passCounts[g] = n;
				g_passTotal += n;
			}
		}

		// Decode a RIP-relative operand at a known instruction, verifying the opcode
		// bytes first. Returns 0 on mismatch.
		[[nodiscard]] std::uintptr_t RipResolve(
			std::uintptr_t a_instrRVA,
			std::initializer_list<std::uint8_t> a_opcode,
			std::size_t a_dispOffset,
			std::size_t a_instrLen,
			std::string_view a_what)
		{
			const auto addr = REL::Module::get().base() + a_instrRVA;
			const auto* p = reinterpret_cast<const std::uint8_t*>(addr);
			std::size_t i = 0;
			for (const auto b : a_opcode) {
				if (p[i++] != b) {
					logger::critical(FMT_STRING("RIP anchor mismatch for {} at {:016X}"), a_what, addr);
					return 0;
				}
			}
			const auto disp = *reinterpret_cast<const std::int32_t*>(p + a_dispOffset);
			return addr + a_instrLen + disp;
		}

		// The whole render, POD-only so the SEH wrapper below is legal.
		void RenderImpl(float a_fovDeg)
		{
			const auto base = REL::Module::get().base();
			const auto rtm = base + kRTManager;
			const auto renderer = base + kRendererRVA;

			const auto player = *reinterpret_cast<std::uintptr_t*>(base + kPlayerGlobal);
			if (!player) {
				return;
			}
			const auto cam = *reinterpret_cast<std::uintptr_t*>(player + kCamOffsetInPlayer);
			if (!cam) {
				return;
			}

			// Place the camera at the objective end of the scope tube (weapon-local
			// offset; the node is parented under PrimaryWeaponOffsetNode). SetCameraFOV
			// ends with NiAVObject::Update, which propagates this to world space.
			{
				auto* localTranslate = reinterpret_cast<float*>(cam + 0x60);  // NiAVObject::local(0x30).translate(+0x30)
				localTranslate[0] = static_cast<float>(*Settings::scopeCamOffsetX);
				localTranslate[1] = static_cast<float>(*Settings::scopeCamOffsetY);
				localTranslate[2] = static_cast<float>(*Settings::scopeCamOffsetZ);
			}

			// Zoom FOV: force SetCameraFOV's symmetric-frustum path (instead of HMD eye
			// projections) exactly like the vanilla scope pass does, then restore.
			RENDER_STEP(1);
			auto* mode738 = reinterpret_cast<std::uint8_t*>(g_fovMode738);
			auto* mode750 = reinterpret_cast<std::int32_t*>(g_fovMode750);
			const auto saved738 = *mode738;
			const auto saved750 = *mode750;
			*mode738 = 1;
			*mode750 = 2;
			Fn<SetCameraFOV_t>(0x2804a90)(cam, a_fovDeg, 1.0f, 1.0f);
			*mode738 = saved738;
			*mode750 = saved750;

			// Full-frame port. The camera's NiRect (NiCamera+0x184: left, right, top,
			// bottom) drives every viewport computed from the camera-data block
			// (FUN_141d8d480 scales rect[0..3] by the bound target's dimensions) — a
			// stereo eye rect here renders into half the target: the left-half-black lens.
			auto* port = reinterpret_cast<float*>(cam + 0x184);
			std::memcpy(g_foundPort, port, sizeof(g_foundPort));
			port[0] = 0.0f;  // left
			port[1] = 1.0f;  // right
			port[2] = 1.0f;  // top
			port[3] = 0.0f;  // bottom

			// Accumulator: DEFERRED renderMode 0x19 (0 = forward buckets, which the
			// resolve never draws — the v0.2.x black-lens root cause), the deferred
			// enable bytes both engine templates set, world SSN, eye positions.
			RENDER_STEP(2);
			const auto accum = reinterpret_cast<std::uintptr_t>(g_accum);
			const auto ssn0 = *reinterpret_cast<std::uintptr_t*>(g_ssnArray);
			if (!ssn0) {
				return;
			}
			*reinterpret_cast<std::uint32_t*>(accum + 0xf688) = 0x19;
			*reinterpret_cast<std::uint8_t*>(accum + 0xf669) = 1;  // FUN_140b03d60: set when deferred
			*reinterpret_cast<std::uint8_t*>(accum + 0xf66a) = 1;  // FUN_140c875f0: set for the world render
			*reinterpret_cast<std::uintptr_t*>(accum + 0xf680) = ssn0;  // BSShaderAccumulator::shadowSceneNode
			const auto* eyePos = reinterpret_cast<const float*>(cam + 0xa0);  // NiAVObject::world.translate
			std::memcpy(reinterpret_cast<void*>(accum + 0xf690), eyePos, 12);
			std::memcpy(reinterpret_cast<void*>(accum + 0xf6a0), eyePos, 12);

			// Stack culling process bound to our accumulator.
			RENDER_STEP(3);
			alignas(16) std::uint8_t cullBuf[0x1a0];
			Fn<CullCtor_t>(0x1d4d8e0)(cullBuf, 0);
			RENDER_STEP(4);
			Fn<CullSetAccum_t>(0x1d4d9c0)(cullBuf, g_accum);

			RENDER_STEP(5);
			Fn<ClearPrevCam_t>(0x1d95240)(renderer);

			// G-buffer target setup, byte-decoded from FUN_140c87320: slots 0-4 mode 0
			// (clear-on-apply), slot 5 (0x23) mode 3 (preserve). DS 0xC — the scope-pass
			// depth the +4 remap selects everywhere — mode 0 so depth+stencil start clean.
			RENDER_STEP(6);
			Fn<ClearPrevCam_t>(0x1d94990)(renderer);  // Renderer::ResetState
			Fn<SelectDS_t>(0x1db9e40)(rtm, 0xc, 0, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x1c, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, 0x1d, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 2, 0x20, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 3, 0x21, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 4, 0x22, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 5, 0x23, 3);
			Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);

			// ONE AccumulateScene over the world SSN. NO ProcessQueuedLights: the main
			// world pass already ran it this frame (the SSN light lists our resolve
			// reads are fresh), it needs a fully-populated VR camera at cull+0x18
			// (UpdateLightList 0x1427ee2f0 dereferences it at +0x53 — the v0.2.8/9
			// step-7 C0000005), and re-running it against our camera would mutate the
			// shared SSN light lists the next main frame depends on. Vanilla scoped
			// mode also runs the per-frame light update exactly once.
			RENDER_STEP(8);
			Fn<AccumScene_t>(0x27ff370)(cam, ssn0, cullBuf, 1);

			RENDER_STEP(9);
			CapturePassCounts(accum);

			// Drop the deferred-decal groups from our accumulator. The main view drew
			// and released this frame's decal passes already; re-accumulating the same
			// geometry hands back dangling pass objects (v0.2.11 forensics: garbage
			// vtable call in FUN_14281e500(accum, 0x11, 9, ...) — the decal group draw
			// at resolve+0x17b6). FUN_14281ecb0 is the engine's own per-group clear.
			RENDER_STEP(10);
			using ClearGroup_t = void (*)(std::uintptr_t);
			for (const std::uint32_t g : { 9u, 0x11u, 0x12u, 0x13u }) {
				Fn<ClearGroup_t>(0x281ecb0)(accum + 0x18 + static_cast<std::uintptr_t>(g) * 0x678);
			}

			RENDER_STEP(11);
			Fn<Flush_t>(0x1d8dc70)(renderer);

			// Deferred G-buffer group draw + lighting + composite into 0x61. With
			// renderer+4 = 1 (set by our caller) the resolve's internal routing matches
			// the vanilla scoped frame: light buffers 0x6a/0x6b, DS 0xC. param_6 = 0xC
			// keeps its tail bind consistent with that.
			RENDER_STEP(12);
			Fn<DeferredResolve_t>(0x27ff8b0)(cam, g_accum, cullBuf, ssn0, 0x61, 0xc, 0, 1);

			// Unbind (FUN_140b03d60's post-resolve pattern), then finish our accumulator.
			RENDER_STEP(13);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x61, 3);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, -1, 3);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 2, -1, 3);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 3, -1, 3);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 4, -1, 3);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 5, -1, 0);
			Fn<SelectDS_t>(0x1db9e40)(rtm, 1, 3, 0);
			Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
			Fn<FinishAccum_t>(0x281e750)(g_accum);

			// Vanilla lens delivery — FUN_14284e370's per-frame copy (0x61 -> 0x62).
			// The resolve's own 0x61->0x62 combine is refraction-only; this is the real one.
			RENDER_STEP(14);
			Fn<VanillaLensCopy_t>(0x27b08c0)(0x61, Addr::kRT_ScopeLens, 0);

			RENDER_STEP(15);
			Fn<CullDtor_t>(0x1d4d960)(cullBuf);
			RENDER_STEP(16);
		}

		// SEH wrapper — POD frame only. Captures code, faulting address, register
		// context, and probable return addresses (in-module qwords near RSP) so a fault
		// inside an engine call names the object being drawn and the internal call path.
		std::uint32_t g_lastExcCode = 0;
		std::uintptr_t g_lastExcAddr = 0;
		std::uintptr_t g_lastRegs[16] = {};   // rax rbx rcx rdx rsi rdi rbp rsp r8..r15
		std::uintptr_t g_lastStack[8] = {};   // in-module qwords scanned from RSP
		std::uint32_t g_lastStackCount = 0;

		int RenderFilter(EXCEPTION_POINTERS* a_ep) noexcept
		{
			g_lastExcCode = a_ep->ExceptionRecord->ExceptionCode;
			g_lastExcAddr = reinterpret_cast<std::uintptr_t>(a_ep->ExceptionRecord->ExceptionAddress);
			if (const auto* c = a_ep->ContextRecord) {
				const std::uintptr_t regs[16] = {
					c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp,
					c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15
				};
				std::memcpy(g_lastRegs, regs, sizeof(regs));
				// Probable return addresses: module-range qwords in the top of the stack.
				const auto base = REL::Module::get().base();
				const auto end = base + 0x3000000;  // generous .text upper bound
				g_lastStackCount = 0;
				const auto* sp = reinterpret_cast<const std::uintptr_t*>(c->Rsp);
				for (std::uint32_t i = 0; i < 0x100 && g_lastStackCount < 8; ++i) {
					std::uintptr_t v = 0;
					__try {
						v = sp[i];
					} __except (EXCEPTION_EXECUTE_HANDLER) {
						break;
					}
					if (v >= base && v < end) {
						g_lastStack[g_lastStackCount++] = v - base;
					}
				}
			}
			return EXCEPTION_EXECUTE_HANDLER;
		}

		bool RenderGuarded(float a_fovDeg) noexcept
		{
			__try {
				RenderImpl(a_fovDeg);
				return true;
			} __except (RenderFilter(GetExceptionInformation())) {
				return false;
			}
		}
	}

	bool Init()
	{
		// SSN array: SetShadowSceneNode @ +0x1427f54c3: lea rcx, [rip+disp]
		g_ssnArray = RipResolve(0x27f54c3, { 0x48, 0x8D, 0x0D }, 3, 7, "SSN array"sv);
		// SetCameraFOV @ +0x142804bb0: cmp byte ptr [rip+disp], r14b   (44 38 35 disp)
		g_fovMode738 = RipResolve(0x2804bb0, { 0x44, 0x38, 0x35 }, 3, 7, "fov-mode byte"sv);
		// SetCameraFOV @ +0x142804bb9: mov eax, dword ptr [rip+disp]  (8B 05 disp)
		g_fovMode750 = RipResolve(0x2804bb9, { 0x8B, 0x05 }, 2, 6, "fov-mode view index"sv);

		if (!g_ssnArray || !g_fovMode738 || !g_fovMode750) {
			return false;
		}

		// Persistent accumulator. Plain aligned alloc is fine: we hold a permanent ref
		// so the engine's MemoryManager-based DeleteThis can never run on it.
		g_accum = _aligned_malloc(0xf6e0, 16);
		if (!g_accum) {
			return false;
		}
		std::memset(g_accum, 0, 0xf6e0);
		Fn<AccumCtor_t>(0x281b790)(g_accum);
		InterlockedIncrement(reinterpret_cast<volatile long*>(reinterpret_cast<std::uintptr_t>(g_accum) + 8));

		logger::info(
			FMT_STRING("ScopeRender init: ssnArray={:016X} fovModeByte={:016X} fovModeView={:016X} accum={:016X}"),
			g_ssnArray, g_fovMode738, g_fovMode750, reinterpret_cast<std::uintptr_t>(g_accum));

		g_available = true;
		return true;
	}

	bool Render()
	{
		if (!g_available) {
			return false;
		}

		// Scope-pass bracket: renderer+4 = 1 makes every +4-aware call site inside the
		// resolve route exactly like the vanilla scoped world render (light buffers
		// 0x6a/0x6b, DS 0xC). Restored unconditionally — a leaked 1 would redirect the
		// NEXT frame's world draw.
		auto* scopePassFlag = reinterpret_cast<std::uint8_t*>(REL::Module::get().base() + kRendererRVA + 4);
		// renderer+2: every engine render path brackets its drawing with this byte set
		// (FUN_141d94760 writes renderer+2; FUN_140c875f0 / FUN_14284e370 do it). It is
		// already 1 in VR at all times per the load-time flag log, so this is belt and
		// suspenders.
		auto* renderActiveFlag = scopePassFlag - 2;
		const auto savedFlag = *scopePassFlag;
		const auto savedActive = *renderActiveFlag;
		*scopePassFlag = 1;
		*renderActiveFlag = 1;
		const bool ok = RenderGuarded(static_cast<float>(*Settings::scopeFovDegrees));
		*scopePassFlag = savedFlag;
		*renderActiveFlag = savedActive;

		if (!ok) {
			g_available = false;
			static constexpr std::string_view kSteps[] = {
				"entry"sv, "SetCameraFOV"sv, "accum prep"sv, "cull ctor"sv, "SetAccumulator"sv,
				"clear prev-cam cache"sv, "bind MRT+depth"sv, "ProcessQueuedLights"sv,
				"AccumulateScene"sv, "capture pass counts"sv, "clear decal groups"sv,
				"flush"sv, "deferred resolve"sv, "unbind+finish accum"sv, "vanilla lens copy"sv,
				"cull dtor"sv, "done"sv
			};
			const auto step = g_lastStep;
			const auto base = REL::Module::get().base();
			const auto rva = g_lastExcAddr >= base ? g_lastExcAddr - base : 0;
			logger::critical(
				FMT_STRING("ScopeRender FAULTED at step {} ({}), code {:08X} at {:016X} (rva {:X}) — disabled for this session, falling back to copy fill"),
				step, step >= 0 && step < 17 ? kSteps[step] : "?"sv, g_lastExcCode, g_lastExcAddr, rva);
			logger::critical(
				FMT_STRING("  regs: rax={:016X} rbx={:016X} rcx={:016X} rdx={:016X} rsi={:016X} rdi={:016X} rbp={:016X} rsp={:016X}"),
				g_lastRegs[0], g_lastRegs[1], g_lastRegs[2], g_lastRegs[3], g_lastRegs[4], g_lastRegs[5], g_lastRegs[6], g_lastRegs[7]);
			logger::critical(
				FMT_STRING("  regs: r8={:016X} r9={:016X} r10={:016X} r11={:016X} r12={:016X} r13={:016X} r14={:016X} r15={:016X}"),
				g_lastRegs[8], g_lastRegs[9], g_lastRegs[10], g_lastRegs[11], g_lastRegs[12], g_lastRegs[13], g_lastRegs[14], g_lastRegs[15]);
			{
				std::string frames;
				for (std::uint32_t i = 0; i < g_lastStackCount; ++i) {
					fmt::format_to(std::back_inserter(frames), FMT_STRING("{}{:X}"), i ? " " : "", g_lastStack[i]);
				}
				logger::critical(FMT_STRING("  stack rvas: [{}]"), frames);
			}
			{
				// Pass counts were captured before the resolve — log them on fault too.
				std::string groups;
				for (std::uint32_t g = 0; g < kPassGroupCount; ++g) {
					if (g_passCounts[g] != 0) {
						fmt::format_to(std::back_inserter(groups), FMT_STRING("{}{:X}:{}"), groups.empty() ? "" : " ", g, g_passCounts[g]);
					}
				}
				logger::critical(FMT_STRING("  accumulated passes total={} [{}]"), g_passTotal, groups);
			}
			return false;
		}

		// Accumulation diagnostics: first few renders + a heartbeat. "total=0" means
		// nothing accumulated (coverage problem); nonzero groups that still resolve
		// black point at the resolve/bind side instead.
		static std::uint64_t renders = 0;
		++renders;
		if (renders <= 5 || renders % 300 == 0) {
			std::string groups;
			for (std::uint32_t g = 0; g < kPassGroupCount; ++g) {
				if (g_passCounts[g] != 0) {
					fmt::format_to(std::back_inserter(groups), FMT_STRING("{}{:X}:{}"), groups.empty() ? "" : " ", g, g_passCounts[g]);
				}
			}
			logger::info(
				FMT_STRING("ScopeRender #{}: accumulated passes total={} [{}] foundPort=({}, {}, {}, {})"),
				renders, g_passTotal, groups, g_foundPort[0], g_foundPort[1], g_foundPort[2], g_foundPort[3]);
		}
		return true;
	}

	bool Available()
	{
		return g_available;
	}
}
