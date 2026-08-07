#include "TrueScopes/ScopeRender.h"

#include <DirectXMath.h>

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
//     run once per frame by the engine; we do NOT re-run ProcessQueuedLights.
//   * BSShaderUtil::SetCameraFOV params 3/4 are the frustum NEAR/FAR planes. We
//     passed 1,1 from v0.2.0 to v0.2.19: near==far -> NaN projection depth rows ->
//     no geometry ever rasterized. THE root cause of the black/phantom lens era.
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
		using SetCameraFOV_t = void (*)(std::uintptr_t, float, float, float);                          // 0x2804a90  BSShaderUtil::SetCameraFOV(cam, fovDeg, NEAR, FAR) — params 3/4 are the frustum near/far planes (live-proven: passing 1,1 gave near==far → NaN projection rows → nothing ever rasterized)
		using ClearPrevCam_t = void (*)(std::uintptr_t);                                               // 0x1d95240  clear prev-frame camera cache(renderer); also used for 0x1d94990 ResetState
		using AccumScene_t = void (*)(std::uintptr_t, std::uintptr_t, void*, std::uint32_t);           // 0x27ff370  BSShaderUtil::AccumulateScene(cam, node, cull, 1)
		using DeferredResolve_t = void (*)(std::uintptr_t, void*, void*, std::uintptr_t,              // 0x27ff8b0  full deferred render: G-buffer group draws + lighting + composite
			std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);                              //            (cam, accum, cull, ssn, outTargetIdx=0x61, dsIdx=0xC, 0, 1)
		using FinishAccum_t = void (*)(void*);                                                        // 0x281e750  thunk_FUN_14281ec00: finish + clear pass lists
		using CommitTargetsAlt_t = void (*)(std::uintptr_t);                                          // 0x1db9f80  commit target/DS selection
		using VanillaLensCopy_t = void (*)(std::uint32_t, std::uint32_t, std::uint8_t);               // 0x27b08c0  vanilla lens delivery (0x61, 0x62, 0) — effect 0xf = HDR->display tonemap
		using SetCurRT_t = void (*)(std::uintptr_t, std::uint32_t, std::int32_t, std::uint32_t);       // 0x1db9dd0  SetCurrentRenderTarget(mgr, slot, logicalIdx, mode); logical->physical via rtm+0x13bc
		using SelectDS_t = void (*)(std::uintptr_t, std::int32_t, std::uint32_t, std::uint32_t);       // 0x1db9e40  select depth-stencil(mgr, dsIdx, mode, slice); logical->physical via rtm+0x15fc
		using Flush_t = void (*)(std::uintptr_t);                                                      // 0x1d8dc70  Renderer::Flush(renderer)
		using IsmCopy_t = void (*)(std::uint32_t, std::uint32_t);                                      // 0x27b0880  ImageSpaceManager::Copy(src, dst) — RAW copy, no tonemap
		using SetClearColor_t = void (*)(std::uintptr_t, float, float, float, float);                  // 0x1d8dc80  BSGraphics::Renderer::SetClearColor(renderer, r, g, b, a)
		using ClearColorNow_t = void (*)(std::uintptr_t);                                              // 0x1d8dd80  BSGraphics::Renderer::ClearColor(renderer): immediate CRTV of current slot-0 target

		// --- sun pass (v0.2.26) ---
		// The sun is a BSDFLightShader "Dir" technique pass (flags 0x202|filter when
		// shadowed, 0x201 unshadowed — namer FUN_142922370). It is drawn ONCE per frame
		// into the light accum MRT by the pre-world stage (FUN_142846d60 builds/refreshes
		// the persistent pass config, job FUN_142849990 executes it), NOT by the resolve —
		// the resolve's ssn+0x1a8/+0x1c0 loops are point/spot volumes only (cone geometry,
		// BSShaderUtil::GenerateCone in FUN_14286ffa0). That is why v0.2.25 had working
		// local lights but no sun: nothing ever drew the sun into OUR 0x6a.
		using StateSetCamData_t = void (*)(std::uintptr_t, std::uintptr_t, std::uint8_t);              // 0x1da8c40  BSGraphics::State: update camera-data block (state, cam, slotSel)
		using StateSetViewport_t = void (*)(std::uintptr_t, std::uintptr_t, std::uint8_t,             // 0x1da8bf0  BSGraphics::State: viewport from camera port +0x214
			std::uint8_t, float);                                                                     //            (state, cam, a, b, scale) — resolve calls (state, cam, param_8, 0, 1.0f)
		using DepthMode_t = void (*)(std::uintptr_t, std::uint32_t);                                  // 0x1d8dd60 / 0x1d8de10: depth/texture mode setters the resolve runs before lighting
		using CtxCtor_t = void* (*)(void*, std::uintptr_t, std::uintptr_t);                            // 0x2812be0  render-context ctor (ctx[0x2d0], camera, accumulator)
		using FindCamBlock_t = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uint8_t);       // 0x1daaf30  find CameraStateData block (state, camera, sel) in the state+0x140 array (stride 0x480); 0 if absent
		using ExecPassConfig_t = void (*)(std::uintptr_t, std::uint8_t, void*);                        // 0x2891040  execute pass/pass-config (also takes the persistent sun config directly — FUN_142849990 does exactly that)
		using FlushBatch_t = void (*)(void*);                                                          // 0x2891300  flush batched instances for the context

		template <class T>
		[[nodiscard]] T Fn(std::uintptr_t a_rva)
		{
			return reinterpret_cast<T>(REL::Module::get().base() + a_rva);
		}

		// --- data (fixed, byte/live-verified this session) ---
		constexpr std::uintptr_t kPlayerGlobal = 0x5b043f0;      // g_player (F4SEVR, live-verified)
		constexpr std::uintptr_t kRTManager = 0x38ac010;         // RenderTargetManager (decoded from SetCurrentRenderTarget + slot-6 bind call sites)
		constexpr std::uintptr_t kRendererRVA = 0x6239340;       // BSGraphics::Renderer (live-verified)
		constexpr std::uintptr_t kCamOffsetInPlayer = 0x720;     // PrimaryWeaponScopeCamera (VR camera type: eye count +0x208, port +0x214, frusta array +0x1a0)

		// --- data resolved at runtime from code anchors ---
		std::uintptr_t g_ssnArray = 0;    // BSShaderManager SSN slot array (slot 0 = world); anchor: lea in SetShadowSceneNode
		std::uintptr_t g_fovMode738 = 0;  // byte: force symmetric-FOV frusta in SetCameraFOV (vanilla scope pass forces 1)
		std::uintptr_t g_fovMode750 = 0;  // dword: which view gets symmetric FOV; 2 = all (vanilla scope pass forces 2)
		std::uintptr_t g_ctxPtrA = 0;     // &deferred-context ptr (DAT_146235ac8); anchor in FUN_141db9f80
		std::uintptr_t g_ctxPtrB = 0;     // &immediate-context ptr (DAT_146235ac0); anchor in FUN_141db9f80
		std::uintptr_t g_sunConfig = 0;   // persistent sun BSDFLightDir pass config (Ghidra DAT_146886758); anchor: lea in job FUN_142849990
		std::uintptr_t g_gfxState = 0;    // BSGraphics::State (real RVA 0x65A2AB0 — Ghidra's DAT_146541ef0 label is section-shifted); anchor: first lea in the resolve

		void* g_accum = nullptr;
		bool g_available = false;
		bool g_sunBindHooksInstalled = false;      // set by Hooks::Install when the two resolve bind sites are hooked
		std::atomic_bool g_inOwnResolve = false;   // true only while OUR resolve call is on the stack (the hooks key off this)

		// Fault forensics: which step the render was in when the SEH guard fired.
		volatile long g_lastStep = 0;
#define RENDER_STEP(n) g_lastStep = (n)

		// Accumulator layout (from FUN_14281ec00): 38 pass groups, stride 0x678, base
		// accum+0x18; per-group sub-bucket counts at +0x608 + i*0x18 + 0x10 (i = 0..3).
		// Captured POD-side inside the SEH frame, logged from Render() afterwards.
		constexpr std::uint32_t kPassGroupCount = 38;
		std::uint32_t g_passCounts[kPassGroupCount] = {};
		std::uint32_t g_passTotal = 0;

		// Live camera / viewport diagnostics captured after the resolve.
		std::int32_t g_diagLightsA = 0;  // *(short*)(ssn+0x1a8): resolve's shadowed-light loop count
		std::int32_t g_diagLightsB = 0;  // *(short*)(ssn+0x1c0): resolve's queued-light loop count
		std::int32_t g_diagSunSlotPre = -2;   // sun (shadowed light 0) +0x18 shadow-map slot BEFORE resolve
		std::int32_t g_diagSunSlotPost = -2;  // ... and AFTER (0xff = no slot -> the resolve skips the light)
		std::uint64_t g_diagSunFlags = 0;     // sun light +0x108 flags qword
		std::int32_t g_diagSunPass = -1;      // -1 not attempted, 0 config invalid, 1 executed
		std::uint32_t g_diagSunCfgFlags = 0;  // sun config technique flags (+0x48): 0x202|filter = shadowed Dir, 0x201 = unshadowed
		std::int32_t g_diagSunIsSSNSun = -1;  // config light[0] == *(ssn+0x248)?
		std::int32_t g_diagEyeCount = 0;
		float g_diagPort[4] = {};      // VR camera port @ +0x214 (SetCameraFOV forces {0,1,1,0})
		float g_diagRect[6] = {};      // camera-data rect *(ctx+0x25d0)[0..5] — feeds FUN_141d8d480
		std::int32_t g_diagViewport[6] = {};  // computed viewport ints @ ctx+0x1ee0+0x90

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

		// THE v0.2.28 bisect result: with the sun pass in, accum DIFFUSE (0x6a) is a
		// clean sun-lit image but accum SPECULAR (0x6b) is flat saturated garbage —
		// and the composite output is dominated by it. Diffuse needs only N·L;
		// specular additionally needs the VIEW vector, reconstructed from depth via
		// the INVERSE VIEW MATRIX at camData+0x1d0 — which only the engine's scene
		// renderers write MANUALLY (FUN_140c875f0: XMMatrixInverse of the view at
		// camData+0x90, stored TRANSPOSED at +0x1d0..+0x20c). The state camera-data
		// update (0x1da8c40) does NOT touch it, so ours held the stale MAIN camera
		// inverse → garbage world positions → exploded specular. This was flagged as
		// the "next lever" in the v0.2.8 notes and never fired because diffuse looked
		// fine. Replicated here 1:1.
		// THE v0.2.29/30 POST-MORTEM (settled live, x64dbg session 2026-08-07): the
		// camera commit FUN_141daa860 (inside the viewport setter 0x1da8bf0) copies
		// the camera's CameraStateData block into a FIXED STAGING block at
		// *(ctx+0x25d0) — but the copy spans offsets +0x20..+0x1c8 ONLY. The
		// inverse-projection slot at +0x1d0 is NEVER copied; the engine's scene
		// renderers write it into STAGING manually and get away with a stale-source
		// inverse because they re-commit the same camera every frame. v0.2.29 wrote
		// staging from a stale source BEFORE the commit; v0.2.30 wrote a perfect
		// inverse into the camera BLOCK, which the commit never propagates. The
		// draws (BSDFLight spec world-pos reconstruction, composite) consume
		// STAGING+0x1d0 — which still held the MAIN view's inverse-projection.
		// Correct recipe: source = OUR block+0x90 (proj, valid after SetCamData),
		// destination = *(ctx+0x25d0)+0x1d0, written any time before the draws
		// (subsequent commits leave +0x1d0 untouched).
		void WriteInverseProj(std::uintptr_t a_state, std::uintptr_t a_cam)
		{
			using namespace DirectX;
			const auto block = Fn<FindCamBlock_t>(0x1daaf30)(a_state, a_cam, 1);
			if (!block) {
				return;
			}
			const auto ctxA = g_ctxPtrA ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrA) : 0;
			const auto ctxB = g_ctxPtrB ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrB) : 0;
			const auto ctx = ctxA ? ctxA : ctxB;
			if (!ctx) {
				return;
			}
			const auto staging = *reinterpret_cast<std::uintptr_t*>(ctx + 0x25d0);
			if (!staging) {
				return;
			}
			const auto* proj = reinterpret_cast<const XMFLOAT4X4*>(block + 0x90);
			const XMMATRIX inv = XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(proj)));
			XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(staging + 0x1d0), inv);

			// THE EYE-1 STALENESS (root cause, live session pt.2): BSDFLightShader::
			// SetupGeometry builds the Dir-light constants from the staging block's
			// EYE-1 slots (+0x260/+0x2a0 — second 0x210-stride view block) and the
			// ctx eye-1 position (+0x25a0). The camera commit only fills as many eye
			// slots as cam+0x208 — our mono camera fills eye 0 ONLY, so eye 1 kept
			// the MAIN view's right-eye matrices: wrong view-space sun direction,
			// garbage spec. (Same stereo-view-block disease Addendum 2 found for the
			// composite CB.) Mirror eye 0 -> eye 1 after the commit; the resolve's
			// internal re-commit also only writes eye 0, so the mirror survives.
			std::memcpy(
				reinterpret_cast<void*>(staging + 0x230),
				reinterpret_cast<const void*>(staging + 0x20),
				0x210);
			std::memcpy(reinterpret_cast<void*>(ctx + 0x25a0), reinterpret_cast<const void*>(ctx + 0x2590), 12);
			std::memcpy(reinterpret_cast<void*>(ctx + 0x25c0), reinterpret_cast<const void*>(ctx + 0x25b0), 12);
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
			Fn<SetCameraFOV_t>(0x2804a90)(
				cam, a_fovDeg,
				static_cast<float>(*Settings::scopeNearClip),
				static_cast<float>(*Settings::scopeFarClip));
			*mode738 = saved738;
			*mode750 = saved750;

			// NOTE: this VR camera type is NOT flatrim NiCamera. SetCameraFOV's own code
			// shows: eye/frustum count @ +0x208, aspect @ +0x210, port @ +0x214..0x220
			// (which it forces to full-frame {0,1,1,0} itself — do not touch +0x184,
			// that is per-eye frustum data on this type).

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

			// Stack culling process bound to our accumulator, with OUR camera at +0x18 —
			// UpdateLightList dereferences it (the v0.2.8 fault was this being null, not
			// a camera-type problem; the engine's own world path sets cull+0x18 = camera,
			// FUN_14284e370).
			RENDER_STEP(3);
			alignas(16) std::uint8_t cullBuf[0x1a0];
			Fn<CullCtor_t>(0x1d4d8e0)(cullBuf, 0);
			*reinterpret_cast<std::uintptr_t*>(cullBuf + 0x18) = cam;
			RENDER_STEP(4);
			Fn<CullSetAccum_t>(0x1d4d9c0)(cullBuf, g_accum);

			RENDER_STEP(5);
			Fn<ClearPrevCam_t>(0x1d95240)(renderer);

			// SCOPE G-buffer setup, decoded from the world path's own +4 remap site
			// (FUN_142844180): when renderer+4 is set the engine binds the dedicated
			// scope-sized mono G-buffer 0x63/0x64/0x66/0x67/0x68/0x69 with DS 0xC, all
			// mode 0 (clear). Binding the stereo double-wide 0x1c..0x23 with the mono
			// DS 0xC is an RTV/DSV size mismatch D3D11 rejects silently — our geometry
			// never drew at all in v0.2.8-16; the lens showed main-view G-buffer residue.
			RENDER_STEP(6);
			Fn<ClearPrevCam_t>(0x1d94990)(renderer);  // Renderer::ResetState

			// Pre-clear the composite target. The resolve binds 0x61 mode 3 (never
			// clears) and the composite only shades G-buffer-covered pixels — with no
			// sky pass yet, empty regions otherwise keep stale frames (ghosting) or
			// primordial black. Engine pattern (FUN_1401f8bb0): bind + ClearColor.
			Fn<SetClearColor_t>(0x1d8dc80)(renderer, 0.0f, 0.0f, 0.0f, 1.0f);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x61, 3);
			Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
			Fn<ClearColorNow_t>(0x1d8dd80)(renderer);

			Fn<SelectDS_t>(0x1db9e40)(rtm, 0xc, 0, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x63, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, 0x64, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 2, 0x66, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 3, 0x67, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 4, 0x68, 0);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 5, 0x69, 0);
			Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);

			// Re-fit the lights for OUR camera. The main frame's light update fitted
			// every light's screen proxy volume to the MAIN camera; drawn through the
			// zoomed scope projection those volumes cover only part of the screen —
			// the camera-independent rounded cutoff + ambient-only darkness of
			// v0.2.22-24 (sunSlot=255 pre-resolve confirmed the sun's shadow slot was
			// released too). With cull+0x18 = our camera this is safe (the v0.2.8
			// fault was the null camera), and the next main frame re-fits for its own
			// camera, so the mutation self-heals.
			RENDER_STEP(7);
			using ProcessLights_t = void (*)(std::uintptr_t, void*);
			Fn<ProcessLights_t>(0x27eab40)(ssn0, cullBuf);

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

			// Sun diagnostics: shadowed light 0 (accessor FUN_1427ec150) — the resolve
			// skips any light whose +0x18 shadow-map slot is 0xff.
			using GetShadowedLight_t = std::uintptr_t (*)(std::uintptr_t, std::uint32_t);
			if (*reinterpret_cast<const std::int16_t*>(ssn0 + 0x1a8) > 0) {
				if (const auto sun = Fn<GetShadowedLight_t>(0x27ec150)(ssn0, 0)) {
					g_diagSunSlotPre = *reinterpret_cast<const std::int32_t*>(sun + 0x18);
					g_diagSunFlags = *reinterpret_cast<const std::uint64_t*>(sun + 0x108);
				}
			}

			RENDER_STEP(11);
			Fn<Flush_t>(0x1d8dc70)(renderer);

			// --- SUN (v0.2.26) ---
			// Pre-draw the sun's BSDFLightDir pass into the scope light-accum MRT. The
			// engine draws the sun once per frame (pre-world stage) into the MAIN view's
			// 0x24/0x25; the resolve only draws point/spot volumes, so our 0x6a stayed
			// sunless (v0.2.25: local lights fine, no directional). Recipe = queued job
			// FUN_142849990 verbatim: bind accum MRT, camera state, render states
			// (base ctx+0x1ee0: +0xb0=5, +0xbc=1, +0xc8=5(additive), +0xd0=1), execute
			// the persistent config, flush, restore +0xb0=0. The resolve's own accum
			// clear (bind mode 0) is forced to mode 3 by the Hooks::Install call-site
			// hooks while g_inOwnResolve — without them the sun would be wiped, so we
			// skip the draw entirely if they are not installed.
			RENDER_STEP(12);
			g_diagSunPass = -1;
			if (g_sunBindHooksInstalled && g_sunConfig && g_gfxState && *Settings::sunEnabled) {
				const auto cfgClean = *reinterpret_cast<const std::uint8_t*>(g_sunConfig + 0x0) == 0;
				const auto cfgBuilt = *reinterpret_cast<const std::uintptr_t*>(g_sunConfig + 0x8) != 0;
				g_diagSunCfgFlags = *reinterpret_cast<const std::uint32_t*>(g_sunConfig + 0x48);
				if (const auto lights = *reinterpret_cast<std::uintptr_t*>(g_sunConfig + 0x38)) {
					const auto cfgSun = *reinterpret_cast<const std::uintptr_t*>(lights);
					g_diagSunIsSSNSun = cfgSun == *reinterpret_cast<const std::uintptr_t*>(ssn0 + 0x248) ? 1 : 0;
				}
				if (cfgClean && cfgBuilt) {
					// Clear the accum RTs DETERMINISTICALLY (v0.2.23 pattern: bind as
					// slot 0 + commit + immediate CRTV) — but to the FOG/AMBIENT color,
					// not black. Vanilla's mode-0 clear-on-apply executes lazily at the
					// first DRAW, by which point the resolve has re-set the clear color
					// to the fog RGB (FUN_1427aeeb0()+0x1d4..0x1dc, alpha 1) — the accum
					// starts at the ambient base light level. Our v0.2.27 black clear
					// (plus the hooks suppressing the engine clear) deleted that base
					// term: the whole scene lost ambient and read near-black except
					// sun-facing surfaces.
					{
						using GetFogSingleton_t = std::uintptr_t (*)();
						const auto fog = Fn<GetFogSingleton_t>(0x27aeeb0)();
						const float r = fog ? *reinterpret_cast<const float*>(fog + 0x1d4) : 0.0f;
						const float g = fog ? *reinterpret_cast<const float*>(fog + 0x1d8) : 0.0f;
						const float b = fog ? *reinterpret_cast<const float*>(fog + 0x1dc) : 0.0f;
						Fn<SetClearColor_t>(0x1d8dc80)(renderer, r, g, b, 1.0f);
					}
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x6a, 3);
					Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
					Fn<ClearColorNow_t>(0x1d8dd80)(renderer);
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x6b, 3);
					Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
					Fn<ClearColorNow_t>(0x1d8dd80)(renderer);

					// Bind the accum MRT (no clear now) with our scene depth, targets
					// 2..5 unbound like the resolve. The specular target is optional:
					// the sun pass's spec output is the poisoned buffer (v0.2.28 bisect)
					// and root-causing its constants needs a live session — with spec
					// unbound the sun contributes diffuse only and 0x6b stays cleared,
					// which the composite reads as "no sun specular" (correct-looking
					// minus highlights).
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x6a, 3);
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, *Settings::sunSpecEnabled ? 0x6b : -1, 3);
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 2, -1, 3);
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 3, -1, 3);
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 4, -1, 3);
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 5, -1, 3);
					Fn<SelectDS_t>(0x1db9e40)(rtm, 0xc, 3, 0);
					Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);

					// Camera state exactly as the resolve sets it before its light loops,
					// plus the manual inverse-view write the engine's scene renderers do
					// (specular/world-pos reconstruction input; see WriteInverseView).
					Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 1);
					Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 0);
					Fn<StateSetViewport_t>(0x1da8bf0)(g_gfxState, cam, 1, 0, 1.0f);
					WriteInverseProj(g_gfxState, cam);  // AFTER the commit (see note above)
					Fn<DepthMode_t>(0x1d8dd60)(renderer, 0);
					Fn<Flush_t>(0x1d8dc70)(renderer);
					Fn<DepthMode_t>(0x1d8de10)(renderer, 2);

					// Render states (dirty-mask at ctx+0x1ee0: |4 = depth-stencil group,
					// |8 = 0xbc group, |0x10 = blend group — byte-verified in the job).
					const auto ctxA = g_ctxPtrA ? *reinterpret_cast<std::uintptr_t*>(g_ctxPtrA) : 0;
					const auto ctxB = g_ctxPtrB ? *reinterpret_cast<std::uintptr_t*>(g_ctxPtrB) : 0;
					if (const auto sctx = (ctxA ? ctxA : ctxB) + 0x1ee0; sctx != 0x1ee0) {
						auto* dirty = reinterpret_cast<std::uint32_t*>(sctx);
						auto set = [&](std::uintptr_t a_off, std::uint32_t a_val, std::uint32_t a_bit) {
							auto* f = reinterpret_cast<std::uint32_t*>(sctx + a_off);
							if (*f != a_val) {
								*dirty |= a_bit;
								*f = a_val;
							}
						};
						set(0xb0, 5, 4);   // depth mode 5 (job value for the sun pass)
						set(0xbc, 1, 8);
						set(0xc8, 5, 0x10);  // additive blend into the accum buffers
						set(0xd0, 1, 0x10);

						// Brightness diagnostic/tuning: scale the sun NiLight's diffuse
						// RGB (NiLight+0x16c..0x174 — the floats FUN_14286d890 reads)
						// around the exec, restore after. If a small scale does NOT dim
						// the lens, the overbright isn't coming through the light color.
						const auto sunLight = *reinterpret_cast<const std::uintptr_t*>(ssn0 + 0x248);
						const auto niLight = sunLight ? *reinterpret_cast<std::uintptr_t*>(sunLight + 0xb8) : 0;
						float savedRGB[3] = {};
						const auto scale = static_cast<float>(*Settings::sunBrightnessScale);
						const bool scaling = niLight && scale != 1.0f;
						if (scaling) {
							auto* rgb = reinterpret_cast<float*>(niLight + 0x16c);
							std::memcpy(savedRGB, rgb, sizeof(savedRGB));
							rgb[0] *= scale;
							rgb[1] *= scale;
							rgb[2] *= scale;
						}

						alignas(16) std::uint8_t sunCtx[0x2d0];
						Fn<CtxCtor_t>(0x2812be0)(sunCtx, cam, accum);
						Fn<ExecPassConfig_t>(0x2891040)(g_sunConfig, 0, sunCtx);
						Fn<FlushBatch_t>(0x2891300)(sunCtx);

						if (scaling) {
							std::memcpy(reinterpret_cast<float*>(niLight + 0x16c), savedRGB, sizeof(savedRGB));
						}

						set(0xb0, 0, 4);  // job's own restore
						g_diagSunPass = 1;
					}
				} else {
					g_diagSunPass = 0;
				}
			}

			// Deferred G-buffer group draw + lighting + composite into 0x61. With
			// renderer+4 = 1 (set by our caller) the resolve's internal routing matches
			// the vanilla scoped frame: light buffers 0x6a/0x6b, DS 0xC. param_6 = 0xC
			// keeps its tail bind consistent with that. g_inOwnResolve arms the two
			// bind-site hooks so the resolve inherits (not clears) our sun in 0x6a/0x6b.
			RENDER_STEP(13);
			// Ensure the staging inverse-projection is OURS for the resolve's own
			// lighting/composite too. The resolve re-commits the camera internally,
			// but the commit never touches staging+0x1d0, so this write survives it.
			if (g_gfxState) {
				Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 1);
				Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 0);
				WriteInverseProj(g_gfxState, cam);
			}
			g_inOwnResolve.store(g_diagSunPass == 1);
			Fn<DeferredResolve_t>(0x27ff8b0)(cam, g_accum, cullBuf, ssn0, 0x61, 0xc, 0, 1);
			g_inOwnResolve.store(false);

			// Unbind (FUN_140b03d60's post-resolve pattern), then finish our accumulator.
			RENDER_STEP(14);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x61, 3);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, -1, 3);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 2, -1, 3);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 3, -1, 3);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 4, -1, 3);
			Fn<SetCurRT_t>(0x1db9dd0)(rtm, 5, -1, 0);
			Fn<SelectDS_t>(0x1db9e40)(rtm, 1, 3, 0);
			Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
			Fn<FinishAccum_t>(0x281e750)(g_accum);

			// Capture what the resolve actually rendered with: camera eye count/port,
			// the camera-data rect that feeds viewport computation, and the last
			// computed viewport ints in the context block.
			g_diagLightsA = *reinterpret_cast<const std::int16_t*>(ssn0 + 0x1a8);
			g_diagLightsB = *reinterpret_cast<const std::int16_t*>(ssn0 + 0x1c0);
			if (g_diagLightsA > 0) {
				if (const auto sun = Fn<GetShadowedLight_t>(0x27ec150)(ssn0, 0)) {
					g_diagSunSlotPost = *reinterpret_cast<const std::int32_t*>(sun + 0x18);
				}
			}
			g_diagEyeCount = *reinterpret_cast<const std::int32_t*>(cam + 0x208);
			std::memcpy(g_diagPort, reinterpret_cast<const void*>(cam + 0x214), sizeof(g_diagPort));
			if (g_ctxPtrA || g_ctxPtrB) {
				auto ctx = g_ctxPtrA ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrA) : 0;
				if (!ctx && g_ctxPtrB) {
					ctx = *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrB);
				}
				if (ctx) {
					if (const auto camData = *reinterpret_cast<const std::uintptr_t*>(ctx + 0x25d0)) {
						std::memcpy(g_diagRect, reinterpret_cast<const void*>(camData), sizeof(g_diagRect));
					}
					std::memcpy(g_diagViewport, reinterpret_cast<const void*>(ctx + 0x1ee0 + 0x90), sizeof(g_diagViewport));
				}
			}

			// Lens delivery 0x61 -> 0x62 via the VANILLA copy (FUN_1427b08c0, effect
			// 0xf) — it is the HDR->display tonemap, not a plain copy. The composite
			// writes linear HDR into 0x61; delivering with the raw ImageSpaceManager::
			// Copy (v0.2.15-20) showed un-tonemapped values: the faint/dark lens.
			// Diagnostics (raw copies, no tonemap): 3 = diffuse G-buffer 0x63,
			// 4 = light accum diffuse 0x6a, 5 = light accum specular 0x6b,
			// 6 = G-buffer normals 0x64. 4/5 bisect the sun-overbright pipeline: if
			// the accum itself is blown flat, the sun PASS writes garbage; if the
			// accum looks sane, the COMPOSITE consumption is at fault.
			RENDER_STEP(15);
			switch (*Settings::lensMode) {
			case 3:
				Fn<IsmCopy_t>(0x27b0880)(0x63, Addr::kRT_ScopeLens);
				break;
			case 4:
				Fn<IsmCopy_t>(0x27b0880)(0x6a, Addr::kRT_ScopeLens);
				break;
			case 5:
				Fn<IsmCopy_t>(0x27b0880)(0x6b, Addr::kRT_ScopeLens);
				break;
			case 6:
				Fn<IsmCopy_t>(0x27b0880)(0x64, Addr::kRT_ScopeLens);
				break;
			case 7:
				// Raw (un-tonemapped) composite output — separates "composite wrote
				// darkness" from "the tonemap crushed it".
				Fn<IsmCopy_t>(0x27b0880)(0x61, Addr::kRT_ScopeLens);
				break;
			default:
				Fn<VanillaLensCopy_t>(0x27b08c0)(0x61, Addr::kRT_ScopeLens, 0);
				break;
			}

			RENDER_STEP(16);
			Fn<CullDtor_t>(0x1d4d960)(cullBuf);
			RENDER_STEP(17);
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
		// FUN_141db9f80 @ +0x141db9f84: mov rax, [rip+disp] → &ctxA (DAT_146235ac8)
		g_ctxPtrA = RipResolve(0x1db9f84, { 0x48, 0x8B, 0x05 }, 3, 7, "context ptr A"sv);
		// FUN_141db9f99: mov rdx, [rip+disp] → &ctxB (DAT_146235ac0)
		g_ctxPtrB = RipResolve(0x1db9f99, { 0x48, 0x8B, 0x15 }, 3, 7, "context ptr B"sv);
		// Sun pass config: job FUN_142849990 @ +0x142849c85: lea rcx, [rip+disp] before
		// its FUN_142891040(&sunConfig, 0, ctx) call (the third exec in the job).
		g_sunConfig = RipResolve(0x2849c85, { 0x48, 0x8D, 0x0D }, 3, 7, "sun pass config"sv);
		// BSGraphics::State: first lea in the resolve @ +0x1427ff926 (FUN_141da8c40 arg).
		// NOTE: resolves to +0x65A2AB0 in the live process — Ghidra's DAT_146541ef0 label
		// for this block is section-shifted; the code bytes are ground truth.
		g_gfxState = RipResolve(0x27ff926, { 0x48, 0x8D, 0x0D }, 3, 7, "graphics state"sv);

		if (!g_ssnArray || !g_fovMode738 || !g_fovMode750) {
			return false;
		}
		// Context/sun anchors are optional; a mismatch just disables that feature.

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
			FMT_STRING("ScopeRender init: ssnArray={:016X} fovModeByte={:016X} fovModeView={:016X} accum={:016X} sunConfig={:016X} gfxState={:016X}"),
			g_ssnArray, g_fovMode738, g_fovMode750, reinterpret_cast<std::uintptr_t>(g_accum),
			g_sunConfig, g_gfxState);

		g_available = true;
		return true;
	}

	bool Render()
	{
		if (!g_available) {
			return false;
		}

		// Scope-pass bracket, mirroring Main::Swap's scoped branch exactly (the only
		// writer of renderer+1 in the binary, FUN_141d94750, is called just there):
		//   +4 = 1   route RT/DS through the scope set (0x61/0x62/0x6a/0x6b/0xC)
		//   +1 = 0   STEREO MASTER OFF — every draw becomes mono DrawIndexed and the
		//            VS stereo constant (b8 float = +1 && +2, uploaded by the state
		//            flush from ctx+0x1ec0) goes 0, so no per-instance NDC half-shift.
		//   FUN_141d94c10 rebinds the constant buffers (incl. b8) around both edges.
		// Do NOT bracket +2 instead: the deferred technique setup (FUN_142918fc0 and
		// ~30 siblings) unconditionally re-writes +2=1 mid-resolve — that is why the
		// v0.2.18 +2=0 bracket still composited stereo-instanced with stale view-1
		// data (the split lens). +1 is never touched by pass setup.
		const auto renderer = REL::Module::get().base() + kRendererRVA;
		using RendererFn_t = void (*)(std::uintptr_t);
		auto* scopePassFlag = reinterpret_cast<std::uint8_t*>(renderer + 4);
		auto* stereoMaster = reinterpret_cast<std::uint8_t*>(renderer + 1);
		const auto savedFlag = *scopePassFlag;
		const auto savedStereo = *stereoMaster;
		*scopePassFlag = 1;
		*stereoMaster = 0;
		Fn<RendererFn_t>(0x1d94c10)(renderer);  // rebind CBs (stereo b8 included)
		const bool ok = RenderGuarded(static_cast<float>(*Settings::scopeFovDegrees));
		g_inOwnResolve.store(false);  // fault path may have skipped the in-function reset
		*scopePassFlag = savedFlag;
		*stereoMaster = savedStereo;
		Fn<RendererFn_t>(0x1d94c10)(renderer);  // rebind for the rest of the frame
		Fn<RendererFn_t>(0x1d95240)(renderer);  // clear prev-cam cache (vanilla does)

		if (!ok) {
			g_available = false;
			static constexpr std::string_view kSteps[] = {
				"entry"sv, "SetCameraFOV"sv, "accum prep"sv, "cull ctor"sv, "SetAccumulator"sv,
				"clear prev-cam cache"sv, "bind MRT+depth"sv, "ProcessQueuedLights"sv,
				"AccumulateScene"sv, "capture pass counts"sv, "clear decal groups"sv,
				"flush"sv, "sun dir-light pass"sv, "deferred resolve"sv, "unbind+finish accum"sv,
				"vanilla lens copy"sv, "cull dtor"sv, "done"sv
			};
			const auto step = g_lastStep;
			const auto base = REL::Module::get().base();
			const auto rva = g_lastExcAddr >= base ? g_lastExcAddr - base : 0;
			logger::critical(
				FMT_STRING("ScopeRender FAULTED at step {} ({}), code {:08X} at {:016X} (rva {:X}) — disabled for this session, falling back to copy fill"),
				step, step >= 0 && step < 18 ? kSteps[step] : "?"sv, g_lastExcCode, g_lastExcAddr, rva);
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
				FMT_STRING("ScopeRender #{}: passes total={} [{}] lights={}+{} sunPass={} sunCfgFlags={:X} sunIsSSN={} sunSlot={}/{} sunFlags={:016X} eyes={} port=({},{},{},{}) camRect=({},{},{},{},{},{}) viewport=({},{},{},{},{},{})"),
				renders, g_passTotal, groups, g_diagLightsA, g_diagLightsB,
				g_diagSunPass, g_diagSunCfgFlags, g_diagSunIsSSNSun,
				g_diagSunSlotPre, g_diagSunSlotPost, g_diagSunFlags, g_diagEyeCount,
				g_diagPort[0], g_diagPort[1], g_diagPort[2], g_diagPort[3],
				g_diagRect[0], g_diagRect[1], g_diagRect[2], g_diagRect[3], g_diagRect[4], g_diagRect[5],
				g_diagViewport[0], g_diagViewport[1], g_diagViewport[2], g_diagViewport[3], g_diagViewport[4], g_diagViewport[5]);
		}
		return true;
	}

	bool Available()
	{
		return g_available;
	}

	bool InOwnResolve()
	{
		return g_inOwnResolve.load();
	}

	void SetSunBindHooksInstalled(bool a_installed)
	{
		g_sunBindHooksInstalled = a_installed;
	}
}
