#include "TrueScopes/ScopeRender.h"

#include "Settings/Settings.h"
#include "TrueScopes/Addresses.h"

// Phase 2: LocalMapRenderer-pattern mono world render from PrimaryWeaponScopeCamera.
// Full recipe + provenance: ROUTE_B_STATIC_MAP_2026-08-06.md section 3.2 in the
// investigation repo. All raw offsets below were decoded from code bytes this session
// (Ghidra data labels in the high .data region are unreliable; code bytes are ground
// truth and match the live process).

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

			// Zoom FOV: force SetCameraFOV's symmetric-frustum path (instead of HMD eye
			// projections) exactly like the vanilla scope pass does, then restore.
			auto* mode738 = reinterpret_cast<std::uint8_t*>(g_fovMode738);
			auto* mode750 = reinterpret_cast<std::int32_t*>(g_fovMode750);
			const auto saved738 = *mode738;
			const auto saved750 = *mode750;
			*mode738 = 1;
			*mode750 = 2;
			Fn<SetCameraFOV_t>(0x2804a90)(cam, a_fovDeg, 1.0f, 1.0f);
			*mode738 = saved738;
			*mode750 = saved750;

			// Accumulator: renderMode 0 (normal lit world), eye position = camera world pos.
			const auto accum = reinterpret_cast<std::uintptr_t>(g_accum);
			*reinterpret_cast<std::uint32_t*>(accum + 0xf688) = 0;
			const auto* eyePos = reinterpret_cast<const float*>(cam + 0xa0);  // NiAVObject::world.translate
			std::memcpy(reinterpret_cast<void*>(accum + 0xf690), eyePos, 12);
			std::memcpy(reinterpret_cast<void*>(accum + 0xf6a0), eyePos, 12);

			// Stack culling process bound to our accumulator.
			alignas(16) std::uint8_t cullBuf[0x1a0];
			Fn<CullCtor_t>(0x1d4d8e0)(cullBuf, 0);
			Fn<CullSetAccum_t>(0x1d4d9c0)(cullBuf, g_accum);

			alignas(16) std::uint8_t ispBuf[0x2d0];
			Fn<BuildIsp_t>(0x2812be0)(ispBuf, cam, g_accum);

			Fn<ClearPrevCam_t>(0x1d95240)(renderer);

			// Accumulate the world: portal graph (what the main draw uses), the world
			// ShadowSceneNode's object subtrees, then queued lights.
			if (const auto entry = Fn<GetPortalEntry_t>(0xd878f0)()) {
				Fn<AccumSceneArray_t>(0x27ff5d0)(cam, entry + 0x58, cullBuf, 0);
			}
			if (const auto ssn0 = *reinterpret_cast<std::uintptr_t*>(g_ssnArray)) {
				if (const auto sub = *reinterpret_cast<std::uintptr_t*>(ssn0 + 0x168)) {
					if (const auto nodeA = *reinterpret_cast<std::uintptr_t*>(sub + 0x48)) {
						Fn<AccumScene_t>(0x27ff370)(cam, nodeA, cullBuf, 0);
					}
					if (const auto nodeB = *reinterpret_cast<std::uintptr_t*>(sub + 0x40)) {
						Fn<AccumScene_t>(0x27ff370)(cam, nodeB, cullBuf, 0);
					}
				}
				Fn<ProcessLights_t>(0x27eab40)(ssn0, cullBuf);
			}

			// Temp targets (LocalMap's), bind, clear, draw, deliver, release.
			Fn<AcquireTarget_t>(0x1dbad90)(rtm, kTempRT);
			Fn<AcquireTarget_t>(0x1dbaea0)(rtm, kTempDS);
			Fn<SelectDS_t>(0x1db9e40)(rtm, kTempDS, 5, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, kTempRT, 0);
			for (std::uint32_t slot = 1; slot < 6; ++slot) {
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, slot, -1, 3);
			}
			Fn<CommitTargets_t>(0x1db9fe0)(rtm);
			Fn<ClearColor_t>(0x1d8dd80)(renderer);
			Fn<Flush_t>(0x1d8dc70)(renderer);

			Fn<DrawAccum_t>(0x27ff820)(cam, g_accum, ispBuf, 0);

			Fn<IsmCopy_t>(0x27b0880)(kTempRT, Addr::kRT_ScopeLens);

			Fn<ReleaseTarget_t>(0x1dbae00)(rtm, kTempRT);
			Fn<ReleaseTarget_t>(0x1dbaf10)(rtm, kTempDS);

			Fn<CullDtor_t>(0x1d4d960)(cullBuf);
		}

		// SEH wrapper — POD frame only.
		bool RenderGuarded(float a_fovDeg) noexcept
		{
			__try {
				RenderImpl(a_fovDeg);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
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
		if (!RenderGuarded(static_cast<float>(*Settings::scopeFovDegrees))) {
			g_available = false;
			logger::critical("ScopeRender FAULTED — disabled for this session, falling back to copy fill"sv);
			return false;
		}
		return true;
	}

	bool Available()
	{
		return g_available;
	}
}
