#include "TrueScopes/ScopeRender.h"

#include <DirectXMath.h>
#include <d3d11.h>

#include <fstream>

#include "Settings/Settings.h"
#include "TrueScopes/Addresses.h"
#include "TrueScopes/ScopeIdent.h"
#include "TrueScopes/LensComposite.h"
#include "TrueScopes/OneEuro.h"

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
//   * BSShaderUtil::SetCameraFOV params 3/4 are the frustum FAR/NEAR planes — in
//     that order (v0.2.36 static proof; every vanilla caller passes (far, near)).
//     We passed 1,1 from v0.2.0 to v0.2.19: near==far -> NaN projection depth rows
//     -> no geometry rasterized (the black/phantom lens era). Then v0.2.20-35
//     passed (near, far) = swapped -> reversed projection under the engine's
//     standard-Z LESS_EQUAL states -> farthest-fragment-wins depth inversion.
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
		using CullSetFrustum_t = void (*)(void*, const void*);                                         // 0x1c452b0  NiCullingProcess::SetFrustum(cull, NiFrustum*) — needs cull+0x18 (camera) set first; fills the 6 planes at cull+0x3c and the enable mask at cull+0x9c
		using SetCameraFOV_t = void (*)(std::uintptr_t, float, float, float);                          // 0x2804a90  BSShaderUtil::SetCameraFOV(cam, fovDeg, FAR, NEAR) — param_3=far, param_4=near (v0.2.36 static proof; the v0.2.0-19 "1,1" bug was near==far → NaN projection)
		using ClearPrevCam_t = void (*)(std::uintptr_t);                                               // 0x1d95240  clear prev-frame camera cache(renderer); also used for 0x1d94990 ResetState
		using AccumScene_t = void (*)(std::uintptr_t, std::uintptr_t, void*, std::uint32_t);           // 0x27ff370  BSShaderUtil::AccumulateScene(cam, node, cull, 1)
		using DeferredResolve_t = void (*)(std::uintptr_t, void*, void*, std::uintptr_t,              // 0x27ff8b0  full deferred render: G-buffer group draws + lighting + composite
			std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);                              //            (cam, accum, cull, ssn, outTargetIdx=0x61, dsIdx=0xC, 0, 1)
		using FinishAccum_t = void (*)(void*);                                                        // 0x281e750  thunk_FUN_14281ec00: finish + clear pass lists
		using GroupEmpty_t = std::uint32_t (*)(std::uintptr_t);                                       // 0x281f2c0  BSShaderAccumulator: group empty check (arg = accum+0x18+idx*0x678; returns 1 iff all four sub-buckets empty)
		using DrawGroupNow_t = void (*)(void*, std::uint32_t, void*, std::uint32_t);                  // 0x281e400  immediate group renderer (accum, groupIdx, renderCtx, 0) — walks group pass array, execs each via 0x2891040, restores state; skips silently if empty (vanilla sky-draw site: FUN_14284bd00 tail via queued twin 0x28bc680)
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
		// 0x1da8bf0  BSGraphics::State::SetViewportFromCamera(state, cam, slotSel, minDepth, maxDepth)
		// v0.2.64 SIGNATURE FIX. Args 4 and 5 are the D3D11 viewport MIN/MAX DEPTH, and arg 4
		// is a FLOAT in XMM3 — not the uint8 this was declared as. Static proof:
		//   0x141da8bf0  MOVAPS XMM6,XMM3 / ... / MOVAPS XMM3,XMM6   (passes XMM3 straight through)
		//   0x141da9200  MOVSS [RCX+0x10],XMM2   -> camData rect[4] = MinDepth
		//                MOVSS [RCX+0x14],XMM3   -> camData rect[5] = MaxDepth
		//   0x141d8d480  param_2[0x28] = rect[4]; param_2[0x29] = rect[5]  (the D3D11_VIEWPORT)
		// Every vanilla caller sets up exactly this pair, e.g. FUN_140b03d60:
		//   XORPS XMM3,XMM3            -> minDepth 0.0   (arg 4, XMM3)
		//   MOVSS [RSP+0x20],XMM7      -> maxDepth 1.0   (arg 5, stack; 0x142c7f640 == 1.0f, read live)
		// Declaring arg 4 as uint8 sent our 0 to R9, which the callee never reads, leaving XMM3
		// holding leftover garbage from earlier FP work. Field evidence: MinDepth = -4.7747655
		// with MaxDepth = 1.0 (our stack arg landed correctly by luck). D3D11 requires
		// 0 <= Min <= Max <= 1 and drops the whole RSSetViewports call otherwise, leaving the
		// PREVIOUS viewport in effect for our entire scope render.
		using StateSetViewport_t = void (*)(std::uintptr_t, std::uintptr_t, std::uint8_t, float, float);
		using DepthMode_t = void (*)(std::uintptr_t, std::uint32_t);                                  // 0x1d8dd60 / 0x1d8de10: depth/texture mode setters the resolve runs before lighting
		using CtxCtor_t = void* (*)(void*, std::uintptr_t, std::uintptr_t);                            // 0x2812be0  render-context ctor (ctx[0x2d0], camera, accumulator)
		using FindCamBlock_t = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uint8_t);       // 0x1daaf30  find CameraStateData block (state, camera, sel) in the state+0x140 array (stride 0x480); 0 if absent
		// v0.2.76: RETURNS char, and it is not decorative — see the gate note at the call
		// site. Declared void until now, so the one bit that says whether the sun pass
		// actually drew was not merely unchecked, it was unrepresentable.
		using ExecPassConfig_t = char (*)(std::uintptr_t, std::uint8_t, void*);                        // 0x2891040  execute pass/pass-config (also takes the persistent sun config directly — FUN_142849990 does exactly that)
		using NiAVObjectUpdate_t = void (*)(std::uintptr_t, void*);                                    // 0x1c22fb0  NiAVObject::Update(obj, NiUpdateData) — recomputes world transforms down the subtree
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
		constexpr std::uintptr_t kIsmInstanceRVA = 0x68789e8;    // ImageSpaceManager::pInstance (RIP-decoded from FUN_1427b08c0 +0x63)
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
		bool g_faulted = false;  // fault latch (vs never-initialized) — clearable via RetryAfterFault
		std::atomic<std::uint32_t> g_renderTid{ 0 };  // thread id while our renderer+4 bracket is live (see Hooks::ScopePassReadHook)
		// v0.2.65 DevBench surface: render counter promoted out of Render()'s local
		// static, the per-render engine pointers, and the on-demand dump handshake.
		std::uint64_t              g_renders = 0;
		std::uintptr_t             g_lastRtm = 0, g_lastRenderer = 0, g_lastCam = 0;
		std::atomic_bool           g_dumpRequest{ false };
		std::atomic<std::uint64_t> g_dumpEvents{ 0 };
		std::atomic<std::uint64_t> g_lastDumpIndex{ 0 };
		bool g_sunBindHooksInstalled = false;      // set by Hooks::Install when the two resolve bind sites are hooked
		// v0.2.78: the sun exec, deferred to INSIDE the resolve. Holds a lambda that
		// closes over Render()'s locals; Render() is on the stack for the whole resolve
		// call, so those references stay valid. Cleared unconditionally after the resolve
		// (and on the fault path) so a stale closure can never be invoked on a later frame.
		std::function<void()> g_pendingSunExec;
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
		std::int32_t g_diagLightsClamp = -1;  // v0.2.73 perfLightsMax actually applied this render (-1 = none)
		// v0.2.75: the sun NiLight's world basis / position / color as our exec sees them.
		float g_diagSunBasis[9] = {};  // rows at niLight+0x70/+0x80/+0x90
		float g_diagSunPos[3] = {};
		float g_diagSunRGB[3] = {};
		float g_diagSunScale = 0.0f;
		std::int32_t g_diagSunSlotPre = -2;   // sun (shadowed light 0) +0x18 shadow-map slot BEFORE resolve
		std::int32_t g_diagSunSlotPost = -2;  // ... and AFTER (0xff = no slot -> the resolve skips the light)
		std::uint64_t g_diagSunFlags = 0;     // sun light +0x108 flags qword
		std::int32_t g_diagSunPass = -1;      // -1 not attempted, 0 config invalid, 1 executed
		// v0.2.76: "executed" above only ever meant "we made the call". These are the
		// return value of FUN_142891040 — whether the draw actually happened.
		std::int32_t g_diagSunDrew = -1;      // -1 not attempted, 0 gated off, 1 drew
		std::uint64_t g_sunDrewCount = 0;
		std::uint64_t g_sunGatedCount = 0;
		std::uint32_t g_diagSunCfgFlags = 0;  // sun config technique flags (+0x48): 0x202|filter = shadowed Dir, 0x201 = unshadowed
		std::int32_t g_diagSunIsSSNSun = -1;  // config light[0] == *(ssn+0x248)?
		std::int32_t g_diagEyeCount = 0;
		// v0.2.71 culling forensics: the two frusta the engine keeps on a camera.
		// eye0 = [cam+0x1a0][0], the one SetCameraFOV writes and the projection uses;
		// comb = *(cam+0x200), the COMBINED frustum BSCullingGroup::SetCamera culls
		// with. On a mono camera the engine never refreshes comb's lateral extents,
		// which is exactly the §3.7e bug. `combPre` is comb as found BEFORE our
		// mirror, so the log records the stale value that was doing the culling.
		float g_diagFrustumEye0[7] = {};
		float g_diagFrustumCombPre[7] = {};
		std::int32_t g_diagFrustumAliased = -1;  // 1 = [cam+0x200] == [cam+0x1a0] (mirror is a no-op), 0 = distinct, -1 = unknown
		std::int32_t g_diagCullFix = -1;         // -1 not attempted, 0 skipped (setting off / null ptr), 1 applied
		float g_fogRGB[3] = { 0.05f, 0.05f, 0.05f };  // last-good fog color (ambient base); dim gray until first read
		std::uint64_t g_diagFogNulls = 0;             // frames where the fog singleton was null (stutter forensics)

		// --- v0.2.43 black-burst forensics: 1-pixel GPU readback of the lens chain.
		// D3D11 immediate context global (RIP-derived from Renderer::ClearColor
		// 0x141d8dd80: ID3D11DeviceContext* at 0x146235ab0; RTV at renderer+0xa78+
		// idx*0x30 => live ID3D11Texture2D* at renderer+0xa70+idx*0x30, the fields
		// the mode-4 CopyResource restore uses). Each sampled render costs two
		// Map() sync stalls — diagnostic only, off by default.
		constexpr std::uintptr_t kD3DContextRVA = 0x6235ab0;
		ID3D11Texture2D* g_stage61 = nullptr;
		ID3D11Texture2D* g_stage6a = nullptr;
		std::uint32_t g_rbFormat61 = 0;
		std::uint32_t g_rbFormat6a = 0;
		std::uint32_t g_rbW61 = 0, g_rbH61 = 0, g_rbW6a = 0, g_rbH6a = 0;
		// v0.2.77 ordering probe: the G-buffer as it stands AT THE MOMENT of the sun exec.
		// The sun is a deferred directional light -- it shades by sampling the G-buffer.
		// Vanilla fills the G-buffer in the stages BEFORE its sun stage (FUN_14284e9e0
		// call #22); OUR G-buffer geometry is drawn INSIDE the resolve, which we call
		// AFTER the sun exec. If that is the defect, albedo/normals read empty here and
		// populated after the resolve -- and a light shading an empty G-buffer contributes
		// exactly zero, which is what 0x6a has measured all along.
		ID3D11Texture2D* g_stage63 = nullptr;
		ID3D11Texture2D* g_stage64 = nullptr;
		std::uint32_t g_rbFormat63 = 0, g_rbFormat64 = 0;
		std::uint32_t g_rbW63 = 0, g_rbH63 = 0, g_rbW64 = 0, g_rbH64 = 0;
		std::uint64_t g_rbDark61 = 0;   // frames where the composite center pixel was near-black
		std::uint64_t g_rbDark6a = 0;   // ... and the light-accum center pixel
		std::uint64_t g_rbSamples = 0;

		// ---- v0.2.73 THE STOPWATCH — per-stage GPU + CPU timing ------------------
		//
		// The v0.2.72 culling fix took the render 27.3 ms -> 13.6 ms, and the shape of
		// what is left says the remaining cost is not geometry: draw passes fell 19x
		// (14,441 -> ~700) while time fell 2x. The standing suspects — 1024^2 render
		// resolution and 385 shadowed lights — are guesses. So were the last two perf
		// theories, and both were wrong. This measures the stages directly.
		//
		// GPU: D3D11 timestamp queries. Eight marks bracket the seven stages of
		// Render(); a TIMESTAMP_DISJOINT query per render carries the tick frequency
		// and the validity flag (the GPU clock can change frequency mid-flight, and
		// the D3D contract is to DISCARD such a frame — counted, never averaged in).
		// Results are collected 2-3 renders later with DONOTFLUSH, so nothing here
		// ever stalls the pipeline the way the 1-pixel readbacks do.
		//
		// CPU: QueryPerformanceCounter at the same marks. Both are needed —
		// AccumulateScene is CPU pass-list building with almost no GPU work, while
		// the resolve is the reverse. A single number could not tell those apart, and
		// which one it is decides whether the fix is fewer objects or fewer pixels.
		constexpr std::uint32_t kMarkCount = 8;
		constexpr std::uint32_t kSegCount = kMarkCount - 1;  // == kStageCount in the header
		constexpr std::uint32_t kTimerRing = 4;              // renders in flight before we skip timing one

		struct TimerSlot
		{
			ID3D11Query* disjoint = nullptr;
			ID3D11Query* ts[kMarkCount] = {};
			bool         inFlight = false;
		};
		TimerSlot     g_timers[kTimerRing];
		std::uint32_t g_timerHead = 0;      // slot being written this render
		std::uint32_t g_timerTail = 0;      // oldest slot awaiting collection
		std::uint32_t g_timerLive = 0;      // slots in flight
		bool          g_timersReady = false;
		bool          g_timersFailed = false;
		bool          g_timerActive = false;               // this render is being timed
		ID3D11DeviceContext* g_timerCtx = nullptr;

		double        g_gpuSum[kSegCount] = {};
		double        g_cpuSum[kSegCount] = {};
		double        g_gpuTotalSum = 0.0;
		double        g_cpuTotalSum = 0.0;
		std::uint64_t g_gpuSamples = 0;
		std::uint64_t g_cpuSamples = 0;
		std::uint64_t g_timerDisjoint = 0;  // frames discarded because the GPU clock moved
		std::int64_t  g_cpuMarks[kMarkCount] = {};
		std::int64_t  g_qpcFreq = 0;

		// v0.2.74: set by ResetStageTimers() from the DevBench thread; consumed on the
		// render thread. v0.2.73 detected the reset as an OFF->ON edge of perfTimers seen
		// by the render thread, so two /config/set calls back to back landed between
		// renders and the reset never happened -- the first ladder of the 08-09 session
		// silently reported session-long running means and damped a 9 ms effect to 1 ms.
		// A latch cannot miss the window: whenever the flag is set, the next render clears
		// its accumulators. (Gotcha #3 again: a probe that reports something plausible and
		// wrong is worse than one that reports nothing.)
		std::atomic_bool g_timerResetRequest{ false };

		void ResetTimerStats() noexcept
		{
			for (std::uint32_t i = 0; i < kSegCount; ++i) {
				g_gpuSum[i] = 0.0;
				g_cpuSum[i] = 0.0;
			}
			g_gpuTotalSum = g_cpuTotalSum = 0.0;
			g_gpuSamples = g_cpuSamples = g_timerDisjoint = 0;
		}

		bool EnsureTimers(ID3D11DeviceContext* a_ctx) noexcept
		{
			if (g_timersReady) {
				return true;
			}
			if (g_timersFailed || !a_ctx) {
				return false;
			}
			ID3D11Device* dev = nullptr;
			a_ctx->GetDevice(&dev);
			if (!dev) {
				g_timersFailed = true;
				return false;
			}
			bool ok = true;
			for (auto& s : g_timers) {
				D3D11_QUERY_DESC qd{};
				qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
				ok = ok && SUCCEEDED(dev->CreateQuery(&qd, &s.disjoint));
				qd.Query = D3D11_QUERY_TIMESTAMP;
				for (auto& q : s.ts) {
					ok = ok && SUCCEEDED(dev->CreateQuery(&qd, &q));
				}
			}
			dev->Release();
			if (!ok) {
				// Partial creation is worse than none: release what we got and stay off
				// for the session rather than reporting timings from half a ring.
				for (auto& s : g_timers) {
					if (s.disjoint) {
						s.disjoint->Release();
						s.disjoint = nullptr;
					}
					for (auto& q : s.ts) {
						if (q) {
							q->Release();
							q = nullptr;
						}
					}
				}
				g_timersFailed = true;
				logger::warn(FMT_STRING("PERF TIMERS: CreateQuery failed — stage timing unavailable this session"));
				return false;
			}
			LARGE_INTEGER f{};
			::QueryPerformanceFrequency(&f);
			g_qpcFreq = f.QuadPart;
			g_timersReady = true;
			logger::info(FMT_STRING("PERF TIMERS: {} timestamp queries ready (ring of {})"), kTimerRing * (kMarkCount + 1), kTimerRing);
			return true;
		}

		// Drain every slot whose results have landed. Non-blocking by construction:
		// DONOTFLUSH means "answer only if it is already back", so a slot that is not
		// ready simply stays in flight — and if the ring fills, the next render goes
		// untimed rather than waiting on the GPU.
		void CollectTimers(ID3D11DeviceContext* a_ctx) noexcept
		{
			while (g_timerLive > 0) {
				auto& s = g_timers[g_timerTail];
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
				if (a_ctx->GetData(s.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
					break;
				}
				std::uint64_t t[kMarkCount] = {};
				bool          have = true;
				for (std::uint32_t i = 0; i < kMarkCount && have; ++i) {
					have = a_ctx->GetData(s.ts[i], &t[i], sizeof(t[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
				}
				if (!have) {
					break;  // disjoint landed first; retry the whole slot next render
				}
				if (dj.Disjoint || dj.Frequency == 0) {
					++g_timerDisjoint;
				} else {
					const double toMs = 1000.0 / static_cast<double>(dj.Frequency);
					for (std::uint32_t i = 0; i < kSegCount; ++i) {
						g_gpuSum[i] += static_cast<double>(t[i + 1] - t[i]) * toMs;
					}
					g_gpuTotalSum += static_cast<double>(t[kMarkCount - 1] - t[0]) * toMs;
					++g_gpuSamples;
				}
				s.inFlight = false;
				g_timerTail = (g_timerTail + 1) % kTimerRing;
				--g_timerLive;
			}
		}

		[[nodiscard]] std::int64_t QpcNow() noexcept
		{
			LARGE_INTEGER c{};
			::QueryPerformanceCounter(&c);
			return c.QuadPart;
		}

		void TimerMark(std::uint32_t a_mark) noexcept
		{
			if (a_mark >= kMarkCount) {
				return;
			}
			g_cpuMarks[a_mark] = QpcNow();
			if (g_timerActive && g_timerCtx) {
				g_timerCtx->End(g_timers[g_timerHead].ts[a_mark]);
			}
		}

		void TimersBegin() noexcept
		{
			// Reset the window on either signal: an explicit request (DevBench
			// /perf/reset or a perfTimers write) or an off->on edge this thread actually
			// observed. The latch is what makes it reliable -- see g_timerResetRequest.
			static bool wasEnabled = false;
			const bool  enabled = *Settings::perfTimers;
			if (g_timerResetRequest.exchange(false) || (enabled && !wasEnabled)) {
				ResetTimerStats();
			}
			wasEnabled = enabled;

			// A render that returned early (null SSN) or faulted inside the SEH guard
			// left a disjoint query open. Close and abandon that slot instead of
			// issuing Begin on top of it — an unbalanced Begin/Begin makes every later
			// reading suspect, and a stopwatch you cannot trust is worse than none.
			if (g_timerActive && g_timerCtx) {
				g_timerCtx->End(g_timers[g_timerHead].disjoint);
				g_timerActive = false;
			}

			g_timerActive = false;
			if (!enabled) {
				return;
			}
			if (g_qpcFreq == 0) {
				// CPU timing must not depend on the D3D queries coming up — if
				// CreateQuery fails we still want the CPU/GPU split's CPU half.
				LARGE_INTEGER f{};
				::QueryPerformanceFrequency(&f);
				g_qpcFreq = f.QuadPart;
			}
			auto* const ctx = *reinterpret_cast<ID3D11DeviceContext**>(REL::Module::get().base() + kD3DContextRVA);
			if (!EnsureTimers(ctx)) {
				// CPU marks still work without D3D queries — record them anyway.
				g_timerCtx = nullptr;
				TimerMark(0);
				return;
			}
			g_timerCtx = ctx;
			CollectTimers(ctx);
			if (g_timerLive < kTimerRing) {
				ctx->Begin(g_timers[g_timerHead].disjoint);
				g_timerActive = true;
			}
			TimerMark(0);
		}

		void TimersEnd() noexcept
		{
			if (!*Settings::perfTimers) {
				return;
			}
			if (g_qpcFreq > 0 && g_cpuMarks[0] != 0 && g_cpuMarks[kMarkCount - 1] != 0) {
				const double toMs = 1000.0 / static_cast<double>(g_qpcFreq);
				for (std::uint32_t i = 0; i < kSegCount; ++i) {
					g_cpuSum[i] += static_cast<double>(g_cpuMarks[i + 1] - g_cpuMarks[i]) * toMs;
				}
				g_cpuTotalSum += static_cast<double>(g_cpuMarks[kMarkCount - 1] - g_cpuMarks[0]) * toMs;
				++g_cpuSamples;
			}
			if (!g_timerActive) {
				return;
			}
			g_timerCtx->End(g_timers[g_timerHead].disjoint);
			g_timers[g_timerHead].inFlight = true;
			g_timerHead = (g_timerHead + 1) % kTimerRing;
			++g_timerLive;
			g_timerActive = false;
		}

		bool EnsureStaging(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src,
			ID3D11Texture2D** a_stage, std::uint32_t& a_fmt, std::uint32_t& a_w, std::uint32_t& a_h) noexcept
		{
			if (*a_stage) {
				return true;
			}
			D3D11_TEXTURE2D_DESC desc{};
			a_src->GetDesc(&desc);
			a_fmt = desc.Format;
			a_w = desc.Width;
			a_h = desc.Height;
			D3D11_TEXTURE2D_DESC sd = desc;
			sd.Width = 1;
			sd.Height = 1;
			sd.MipLevels = 1;
			sd.ArraySize = 1;
			sd.SampleDesc = { 1, 0 };
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			ID3D11Device* dev = nullptr;
			a_ctx->GetDevice(&dev);
			if (!dev) {
				return false;
			}
			const auto ok = SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, a_stage));
			dev->Release();
			return ok;
		}

		std::uint64_t SampleCenterPixel(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src,
			ID3D11Texture2D* a_stage, std::uint32_t a_w, std::uint32_t a_h) noexcept
		{
			const D3D11_BOX box{ a_w / 2, a_h / 2, 0, a_w / 2 + 1, a_h / 2 + 1, 1 };
			a_ctx->CopySubresourceRegion(a_stage, 0, 0, 0, 0, a_src, 0, &box);
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(a_ctx->Map(a_stage, 0, D3D11_MAP_READ, 0, &m))) {
				return ~0ull;
			}
			const auto v = *reinterpret_cast<const std::uint64_t*>(m.pData);
			a_ctx->Unmap(a_stage, 0);
			return v;
		}

		// Near-black test for RGBA16F-family pixels: each of R/G/B half's magnitude
		// below ~0.03 (half 0x2A00). Raw hex is logged anyway, so a wrong format
		// just means classification is off while the data still tells the story.
		bool PixelDarkF16(std::uint64_t a_raw) noexcept
		{
			for (int c = 0; c < 3; ++c) {
				const auto h = static_cast<std::uint16_t>(a_raw >> (16 * c)) & 0x7fff;
				if (h >= 0x2a00) {
					return false;
				}
			}
			return true;
		}

		// v0.2.55 — THE BLIND SPOT. R11G11B10_FLOAT stores exponent 0x1f for Inf/NaN.
		// PixelDark tests "exponent < 12", so a NaN pixel is classified LIT — which is
		// why 12000+ readbacks across five sessions never once reported dark while the
		// lens was visibly black. NaN reaching the display shows up as BLACK on screen
		// but as "bright" to every probe we had. Detect it explicitly.
		bool PixelNonFinite(std::uint32_t a_fmt, std::uint64_t a_raw) noexcept
		{
			switch (a_fmt) {
			case 26: {  // R11G11B10F: 5-bit exponent per channel, all-ones = Inf/NaN
				const auto px = static_cast<std::uint32_t>(a_raw);
				return ((px >> 6) & 0x1f) == 0x1f || ((px >> 17) & 0x1f) == 0x1f || ((px >> 27) & 0x1f) == 0x1f;
			}
			case 28:
			case 29:
			case 87:
			case 91:
				return false;  // integer formats cannot hold NaN
			default: {  // RGBA16F
				for (int c = 0; c < 3; ++c) {
					if ((static_cast<std::uint16_t>(a_raw >> (16 * c)) & 0x7c00) == 0x7c00) {
						return true;
					}
				}
				return false;
			}
			}
		}

		// Format-aware near-black test (v0.2.47): the scope chain RTs are fmt 26
		// (R11G11B10_FLOAT, verified live); 0x62 may be an 8-bit display format.
		bool PixelDark(std::uint32_t a_fmt, std::uint64_t a_raw) noexcept
		{
			switch (a_fmt) {
			case 26: {  // R11G11B10F: dark = every channel exponent < 12 (~< 0.125)
				const auto px = static_cast<std::uint32_t>(a_raw);
				return ((px >> 6) & 0x1f) < 12 && ((px >> 17) & 0x1f) < 12 && ((px >> 27) & 0x1f) < 12;
			}
			case 28:
			case 29:
			case 87:
			case 91:  // RGBA8/BGRA8 (+sRGB)
				return (a_raw & 0xff) < 0x18 && ((a_raw >> 8) & 0xff) < 0x18 && ((a_raw >> 16) & 0xff) < 0x18;
			default:
				return PixelDarkF16(a_raw);
			}
		}

		// --- v0.2.54 FULL-TEXTURE DUMP -------------------------------------------
		// One-pixel sampling has answered "lit" in every mode while the lens is
		// visibly black, so it has run out of information: it cannot distinguish
		// "the texture is fine" from "the texture is black everywhere except where
		// we happen to sample". Dumping the whole surface to a BMP settles it by
		// inspection — including WHERE any black region sits relative to the
		// delivery footprint and the crescent.
		ID3D11Texture2D* g_dumpStage = nullptr;
		std::uint32_t g_dumpW = 0, g_dumpH = 0, g_dumpFmt = 0;
		std::uint32_t g_dumpFiles = 0;

		// R11G11B10_FLOAT channel decode: 5-bit exponent (bias 15) + 6/6/5-bit
		// mantissa, no sign bit.
		[[nodiscard]] float DecodeSmallFloat(std::uint32_t a_bits, std::uint32_t a_mantBits) noexcept
		{
			const auto mantMask = (1u << a_mantBits) - 1u;
			const auto mant = a_bits & mantMask;
			const auto exp = (a_bits >> a_mantBits) & 0x1f;
			if (exp == 0) {
				return mant == 0 ? 0.0f : std::ldexp(static_cast<float>(mant) / static_cast<float>(mantMask + 1), -14);
			}
			if (exp == 0x1f) {
				return 65504.0f;  // inf/nan -> clamp, we only want to LOOK at it
			}
			return std::ldexp(1.0f + static_cast<float>(mant) / static_cast<float>(mantMask + 1),
				static_cast<int>(exp) - 15);
		}

		// HDR -> viewable 8-bit: Reinhard + gamma. Keeps both a dark scene and an
		// overbright one distinguishable from true black, which is the whole point.
		[[nodiscard]] std::uint8_t ToByte(float a_v) noexcept
		{
			const auto tone = a_v <= 0.0f ? 0.0f : a_v / (1.0f + a_v);
			const auto gamma = std::pow(tone, 1.0f / 2.2f);
			return static_cast<std::uint8_t>(std::clamp(gamma, 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		// v0.2.55: NaN/Inf pixels are painted MAGENTA in the dump instead of being
		// clamped to white — otherwise they are indistinguishable from a legitimately
		// overbright frame, which is how the first dump session read as "blown out"
		// rather than "full of NaN". Counted so the log states the coverage.
		std::uint64_t g_dumpNonFinite = 0;

		void WriteBMP(const std::filesystem::path& a_path, const std::uint8_t* a_rows,
			std::uint32_t a_w, std::uint32_t a_h, std::uint32_t a_pitch, std::uint32_t a_fmt) noexcept
		{
			std::ofstream out(a_path, std::ios::binary);
			if (!out) {
				return;
			}
			const std::uint32_t rowBytes = ((a_w * 3u) + 3u) & ~3u;
			const std::uint32_t imageBytes = rowBytes * a_h;
			const std::uint32_t headerBytes = 14u + 40u;
			const auto put16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
			const auto put32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
			out.write("BM", 2);
			put32(headerBytes + imageBytes);
			put32(0);
			put32(headerBytes);
			put32(40);
			put32(a_w);
			put32(a_h);
			put16(1);
			put16(24);
			put32(0);
			put32(imageBytes);
			put32(2835);
			put32(2835);
			put32(0);
			put32(0);
			std::vector<std::uint8_t> row(rowBytes, 0);
			for (std::uint32_t y = 0; y < a_h; ++y) {
				// BMP is bottom-up.
				const auto* src = a_rows + static_cast<std::size_t>(a_h - 1u - y) * a_pitch;
				for (std::uint32_t x = 0; x < a_w; ++x) {
					std::uint8_t b = 0, g = 0, r = 0;
					switch (a_fmt) {
					case 26: {  // R11G11B10_FLOAT
						const auto px = *reinterpret_cast<const std::uint32_t*>(src + x * 4u);
						if (((px >> 6) & 0x1f) == 0x1f || ((px >> 17) & 0x1f) == 0x1f ||
							((px >> 27) & 0x1f) == 0x1f) {
							++g_dumpNonFinite;
							r = 255;
							g = 0;
							b = 255;  // MAGENTA = NaN/Inf, never a real scene color
							break;
						}
						r = ToByte(DecodeSmallFloat(px & 0x7ffu, 6));
						g = ToByte(DecodeSmallFloat((px >> 11) & 0x7ffu, 6));
						b = ToByte(DecodeSmallFloat((px >> 22) & 0x3ffu, 5));
						break;
					}
					case 28:
					case 29: {  // R8G8B8A8
						const auto* p = src + x * 4u;
						r = p[0];
						g = p[1];
						b = p[2];
						break;
					}
					case 87:
					case 91: {  // B8G8R8A8
						const auto* p = src + x * 4u;
						b = p[0];
						g = p[1];
						r = p[2];
						break;
					}
					// v0.2.59: RGBA16F is 8 BYTES per pixel, so this must never be the
					// fallback for an unrecognized format — a 4-byte surface decoded at
					// 8 bytes/pixel reads twice the row length and runs off the end of
					// the mapped staging surface. That is what crashed the dump exactly
					// 180 renders (= diagDumpLensEveryNRenders) after every scope-in,
					// misreported as a step-17 "vanilla lens copy" fault because the
					// dump sits inside that step's span. Unknown formats are now
					// skipped by DumpLogicalRT before we ever map them.
					// v0.2.65: the G-buffer NORMALS target (0x64) is DXGI format 35 =
				// R16G16_UNORM — two channels, 4 bytes/pixel, i.e. an encoded
				// (octahedral or hemi) normal with Z reconstructed in-shader. It had
				// no case here, so every normals dump was silently skipped and that
				// buffer has never actually been looked at.
				// Shown as R=x, G=y, B=0 deliberately: no Z is reconstructed, because
				// guessing the wrong encoding would produce a plausible-looking image
				// that lies. Two channels of truth beat three of speculation.
				// UNORM cannot be NaN, so this buffer never contributes to NaN%.
				case 35: {  // R16G16_UNORM
					const auto* p = reinterpret_cast<const std::uint16_t*>(src + x * 4u);
					r = static_cast<std::uint8_t>(p[0] >> 8);
					g = static_cast<std::uint8_t>(p[1] >> 8);
					b = 0;
					break;
				}
				case 10: {  // R16G16B16A16_FLOAT
						const auto* p = reinterpret_cast<const std::uint16_t*>(src + x * 8u);
						const auto half = [](std::uint16_t h) {
							const auto sign = (h >> 15) & 1u;
							const auto exp = (h >> 10) & 0x1fu;
							const auto mant = h & 0x3ffu;
							float v = exp == 0 ? std::ldexp(static_cast<float>(mant) / 1024.0f, -14) :
												 std::ldexp(1.0f + static_cast<float>(mant) / 1024.0f, static_cast<int>(exp) - 15);
							return sign ? -v : v;
						};
						r = ToByte(half(p[0]));
						g = ToByte(half(p[1]));
						b = ToByte(half(p[2]));
						break;
					}
					default:
						break;  // unreachable: DumpLogicalRT rejects unknown formats
					}
					row[x * 3u + 0] = b;
					row[x * 3u + 1] = g;
					row[x * 3u + 2] = r;
				}
				out.write(reinterpret_cast<const char*>(row.data()), rowBytes);
			}
		}

		void DumpLogicalRT(std::uintptr_t a_rtm, std::uintptr_t a_renderer, std::uint32_t a_logical,
			std::string_view a_label, std::uint64_t a_index) noexcept
		{
			if (g_dumpFiles >= 240) {
				return;
			}
			const auto d3dCtx = *reinterpret_cast<ID3D11DeviceContext**>(REL::Module::get().base() + kD3DContextRVA);
			const auto phys = *reinterpret_cast<const std::int32_t*>(a_rtm + 0x13bc + static_cast<std::uintptr_t>(a_logical) * 4);
			ID3D11RenderTargetView* rtv = phys >= 0 ? *reinterpret_cast<ID3D11RenderTargetView**>(a_renderer + 0xa78 + static_cast<std::uintptr_t>(phys) * 0x30) : nullptr;
			if (!d3dCtx || !rtv) {
				return;
			}
			ID3D11Resource* res = nullptr;
			rtv->GetResource(&res);
			auto* tex = static_cast<ID3D11Texture2D*>(res);
			if (!tex) {
				return;
			}
			D3D11_TEXTURE2D_DESC desc{};
			tex->GetDesc(&desc);
			// v0.2.59: only decode formats whose pixel stride we actually know. The
			// old catch-all assumed 8 bytes/pixel and walked off the end of any 4-byte
			// surface — the crash that faulted every dump 180 renders after scope-in.
			switch (desc.Format) {
			case 26:  // R11G11B10_FLOAT
			case 28:  // R8G8B8A8_UNORM
			case 29:  // R8G8B8A8_UNORM_SRGB
			case 87:  // B8G8R8A8_UNORM
			case 91:  // B8G8R8A8_UNORM_SRGB
			case 35:  // R16G16_UNORM — the G-buffer normals target (v0.2.65)
			case 10:  // R16G16B16A16_FLOAT
				break;
			default: {
				static std::uint32_t skipLogs = 0;
				if (skipLogs < 10) {
					++skipLogs;
					logger::warn(FMT_STRING("DUMP {} SKIPPED — unhandled format {} ({}x{})"),
						a_label, static_cast<std::uint32_t>(desc.Format), desc.Width, desc.Height);
				}
				res->Release();
				return;
			}
			}
			if (!g_dumpStage || g_dumpW != desc.Width || g_dumpH != desc.Height ||
				g_dumpFmt != static_cast<std::uint32_t>(desc.Format)) {
				if (g_dumpStage) {
					g_dumpStage->Release();
					g_dumpStage = nullptr;
				}
				D3D11_TEXTURE2D_DESC sd = desc;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.BindFlags = 0;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				sd.MiscFlags = 0;
				sd.MipLevels = 1;
				sd.ArraySize = 1;
				sd.SampleDesc = { 1, 0 };
				ID3D11Device* dev = nullptr;
				d3dCtx->GetDevice(&dev);
				if (dev) {
					if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &g_dumpStage))) {
						g_dumpW = desc.Width;
						g_dumpH = desc.Height;
						g_dumpFmt = desc.Format;
					}
					dev->Release();
				}
			}
			if (g_dumpStage) {
				d3dCtx->CopyResource(g_dumpStage, tex);
				D3D11_MAPPED_SUBRESOURCE m{};
				if (SUCCEEDED(d3dCtx->Map(g_dumpStage, 0, D3D11_MAP_READ, 0, &m))) {
					auto path = logger::log_directory();
					if (path) {
						const auto gamepath = REL::Module::IsVR() ? "Fallout4VR/F4SE" : "Fallout4/F4SE";
						if (!path->generic_string().ends_with(gamepath)) {
							path = path->parent_path().append(gamepath);
						}
						*path /= "TrueScopesDumps";
						std::error_code ec;
						std::filesystem::create_directories(*path, ec);
						*path /= fmt::format("{:06}_{}.bmp"sv, a_index, a_label);
						g_dumpNonFinite = 0;
						WriteBMP(*path, static_cast<const std::uint8_t*>(m.pData), g_dumpW, g_dumpH, m.RowPitch, g_dumpFmt);
						++g_dumpFiles;
						const auto total = static_cast<double>(g_dumpW) * static_cast<double>(g_dumpH);
						logger::info(FMT_STRING("DUMP {} -> {} ({}x{} fmt={}) NaN/Inf={:.2f}%"),
							a_label, path->string(), g_dumpW, g_dumpH, g_dumpFmt,
							total > 0.0 ? static_cast<double>(g_dumpNonFinite) * 100.0 / total : 0.0);
					}
					d3dCtx->Unmap(g_dumpStage, 0);
				}
			}
			res->Release();
		}

		// One-call sampler: logical RT -> physical (rtm+0x13bc) -> RTV (renderer+
		// 0xa78+phys*0x30) -> GetResource -> 1px staging copy+map.
		std::uint64_t SampleLogicalRT(std::uintptr_t a_rtm, std::uintptr_t a_renderer, std::uint32_t a_logical,
			ID3D11Texture2D** a_stage, std::uint32_t& a_fmt, std::uint32_t& a_w, std::uint32_t& a_h) noexcept
		{
			const auto d3dCtx = *reinterpret_cast<ID3D11DeviceContext**>(REL::Module::get().base() + kD3DContextRVA);
			const auto phys = *reinterpret_cast<const std::int32_t*>(a_rtm + 0x13bc + static_cast<std::uintptr_t>(a_logical) * 4);
			ID3D11RenderTargetView* rtv = phys >= 0 ? *reinterpret_cast<ID3D11RenderTargetView**>(a_renderer + 0xa78 + static_cast<std::uintptr_t>(phys) * 0x30) : nullptr;
			if (!d3dCtx || !rtv) {
				static bool failLogged = false;
				if (!failLogged) {
					failLogged = true;
					logger::warn(FMT_STRING("READBACK sample failed: logical={:X} phys={} ctx={} rtv={}"),
						a_logical, phys, reinterpret_cast<const void*>(d3dCtx), reinterpret_cast<const void*>(rtv));
				}
				return ~0ull;
			}
			ID3D11Resource* res = nullptr;
			rtv->GetResource(&res);
			auto* tex = static_cast<ID3D11Texture2D*>(res);
			std::uint64_t v = ~0ull;
			if (tex && EnsureStaging(d3dCtx, tex, a_stage, a_fmt, a_w, a_h)) {
				v = SampleCenterPixel(d3dCtx, tex, *a_stage, a_w, a_h);
			}
			if (res) {
				res->Release();
			}
			return v;
		}
		ID3D11Texture2D* g_stage62 = nullptr;
		std::uint32_t g_rbFormat62 = 0;
		std::uint32_t g_rbW62 = 0, g_rbH62 = 0;
		std::uint64_t g_rbDark61Sky = 0;  // 0x61 dark AFTER the sky draw (post-resolve was lit)
		std::uint64_t g_rbDark62 = 0;     // delivered lens (0x62) dark after the tonemap copy
		// v0.2.52: 0x62 sampled at fill-hook ENTRY = end of the PREVIOUS frame, after
		// every other writer in the frame had its turn (see SamplePreFillLens).
		std::uint64_t g_rbPre62Samples = 0;
		std::uint64_t g_rbPre62Dark = 0;
		std::int32_t g_rbPre62State = -1;  // last classified state, for edge-only logging
		// v0.2.55: NaN/Inf counters — the state every previous probe scored as "lit".
		std::uint64_t g_rbNaN61 = 0;
		std::uint64_t g_rbNaN62 = 0;
		std::int32_t g_rbNaNState = -1;
		std::int32_t g_diagSkyEmptyPre = -1;   // FUN_14281f2c0(group 0xC) after world accum: 1 = no sky passes
		std::int32_t g_diagSkyEmptyPost = -1;  // ... after the fallback sky-root accumulation
		std::int32_t g_diagSkyRoots = 0;       // how many sky root globals validated + accumulated
		std::int32_t g_diagSkyDrawn = 0;       // 1 = group 0xC drawn into 0x61 this render
		float g_diagPort[4] = {};      // VR camera port @ +0x214 (SetCameraFOV forces {0,1,1,0})
		float g_diagRect[6] = {};      // camera-data rect *(ctx+0x25d0)[0..5] — feeds FUN_141d8d480
		std::int32_t g_diagViewport[6] = {};  // computed viewport ints @ ctx+0x1ee0+0x90

		void CapturePassCountsInto(std::uintptr_t a_accum, std::uint32_t* a_counts, std::uint32_t& a_total) noexcept
		{
			a_total = 0;
			for (std::uint32_t g = 0; g < kPassGroupCount; ++g) {
				const auto groupBase = a_accum + 0x18 + static_cast<std::uintptr_t>(g) * 0x678;
				std::uint32_t n = 0;
				for (std::uint32_t i = 0; i < 4; ++i) {
					n += *reinterpret_cast<const std::uint32_t*>(groupBase + 0x608 + i * 0x18 + 0x10);
				}
				a_counts[g] = n;
				a_total += n;
			}
		}

		void CapturePassCounts(std::uintptr_t a_accum) noexcept
		{
			CapturePassCountsInto(a_accum, g_passCounts, g_passTotal);
		}

		// Sky accumulation forensics (v0.2.38): group counts immediately before and
		// after the sky-root accumulation — the delta says which groups the sky passes
		// actually landed in (0xC expected; anything else = routing surprise).
		std::uint32_t g_skyBaseCounts[kPassGroupCount] = {};
		std::uint32_t g_skyBaseTotal = 0;
		std::uint32_t g_skyAfterCounts[kPassGroupCount] = {};
		std::uint32_t g_skyAfterTotal = 0;

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
		// v0.2.58: how often the inverse-projection source was singular (each one
		// would previously have written a full NaN matrix into the staging block).
		// FIELD RESULT: never fires (invproj=0, det ~745) — XMMatrixInverse was NOT
		// the NaN source. Check kept: it is correct hygiene and costs nothing.
		std::uint64_t g_invProjRejects = 0;
		std::int32_t g_invProjState = -1;

		// v0.2.59: find the NaN at its SOURCE instead of inferring it from buffers.
		// 0x6a comes out 100% NaN from a single fullscreen additive pass while every
		// input buffer is clean, so the poison is a CONSTANT the pass reads — i.e. a
		// non-finite float in the staging camera block or the sun light. Scan the
		// floats we feed it, right before the exec, and name the exact offset.
		std::uint64_t g_camDataBad = 0;
		std::int32_t g_camDataState = -1;

		[[nodiscard]] std::int32_t FirstNonFiniteFloat(std::uintptr_t a_base, std::size_t a_bytes) noexcept
		{
			const auto* f = reinterpret_cast<const float*>(a_base);
			const auto n = a_bytes / sizeof(float);
			for (std::size_t i = 0; i < n; ++i) {
				if (!std::isfinite(f[i])) {
					return static_cast<std::int32_t>(i * sizeof(float));
				}
			}
			return -1;
		}

		// v0.2.60: bracket the sun exec with 1-pixel readbacks of the accumulation
		// buffer. camdata=0 says the constants we FEED the pass are finite, yet 0x6a
		// still comes out 100% NaN — so stop inferring and measure causally: sample
		// 0x6a right after our clear (must be the fog color) and again right after the
		// exec. Clean-then-NaN means the sun draw generates it internally, and the next
		// stop is a breakpoint inside the pass. NaN already after the clear means the
		// writer is something else and the sun has been a red herring throughout.
		std::uint64_t g_sunPreNaN = 0;
		std::uint64_t g_sunPostNaN = 0;
		std::int32_t g_sunNaNState = -1;

		// v0.2.81 — WHAT IS ACTUALLY BOUND when the sun pass draws.
		// The sun NaNs on exactly the pixels it shades (0x6a magenta below the horizon,
		// sky clean) from inputs that all probe healthy. The remaining per-pixel inputs
		// are the G-buffer textures themselves -- and a pass sampling an UNBOUND SRV reads
		// zeros, which is a fine way to divide by zero and produce NaN. Our hook fires at
		// the resolve's light-accum BIND, which may be before the resolve has bound the
		// G-buffer as shader resources for its own light volumes.
		//
		// Ask D3D rather than reason about it. Logged at BOTH call sites so the old
		// placement is the control beside the new one (gotcha #3: always print a
		// known-good control beside the unknown).
		void ProbeBoundResources(std::string_view a_where) noexcept
		{
			// v0.2.82: budget PER LABEL. A single shared counter was consumed entirely by
			// the control condition within 70 ms (~90 renders/s), so the case the probe
			// existed to capture never logged at all. Same family as gotcha #5.
			static std::map<std::string, std::uint32_t, std::less<>> logs;
			if (auto it = logs.find(a_where); it != logs.end()) {
				if (it->second >= 4) {
					return;
				}
				++it->second;
			} else {
				logs.emplace(std::string{ a_where }, 1u);
			}
			auto* const d3d = *reinterpret_cast<ID3D11DeviceContext**>(REL::Module::get().base() + kD3DContextRVA);
			if (!d3d) {
				return;
			}
			ID3D11ShaderResourceView* srv[16] = {};
			d3d->PSGetShaderResources(0, 16, srv);
			std::string bound;
			int count = 0;
			for (int i = 0; i < 16; ++i) {
				if (srv[i]) {
					++count;
					fmt::format_to(std::back_inserter(bound), FMT_STRING("{}{}"), bound.empty() ? "" : ",", i);
					srv[i]->Release();  // PSGetShaderResources AddRefs every returned view
				}
			}
			ID3D11RenderTargetView* rtv[4] = {};
			ID3D11DepthStencilView* dsv = nullptr;
			d3d->OMGetRenderTargets(4, rtv, &dsv);
			int rtCount = 0;
			for (auto*& r : rtv) {
				if (r) {
					++rtCount;
					r->Release();
				}
			}
			const bool haveDS = dsv != nullptr;
			if (dsv) {
				dsv->Release();
			}
			logger::info(
				FMT_STRING("SUN BINDINGS [{}]: PS SRV slots bound = {} of 16 [{}], RTs bound = {}, DS bound = {} — zero SRVs here means the pass samples unbound textures"),
				a_where, count, bound, rtCount, haveDS);
		}

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
			// v0.2.58 — THE NaN ROOT CAUSE. XMMatrixInverse returns a matrix of QNaN
			// when the source is SINGULAR, and passing nullptr for the determinant
			// meant nothing ever checked. That NaN landed in staging+0x1d0 — the
			// inverse-projection constant the sun's fullscreen BSDFLightDir pass and
			// the composite both consume — so one poisoned draw turned the ENTIRE
			// light accumulation buffer to NaN (measured: 0x6a 100% NaN with a clean
			// G-buffer, clean specular accum and a clean fog clear). NaN then reached
			// the lens on exactly the pixels that sample the accum (geometry yes, sky
			// no, since the sky is our separate forward pass) and displayed as BLACK —
			// while probing as LIT, because PixelDark tests exponent < 12 and NaN's is
			// 31. That is the whole "black burst" mystery, five sessions of it.
			//
			// The source is a CACHED camera-state block (FindCamBlock), so it can be
			// stale, recycled or not yet populated for our mono camera — which is why
			// the black was view-dependent, sticky while the view held still, and
			// impossible to correlate with anything in our own render.
			//
			// Validate, and on failure leave the previous value in place: a stale
			// inverse-projection is wrong-looking at worst, NaN is a black lens.
			const auto* proj = reinterpret_cast<const XMFLOAT4X4*>(block + 0x90);
			const XMMATRIX projM = XMLoadFloat4x4(proj);
			XMVECTOR det{};
			const XMMATRIX inv = XMMatrixTranspose(XMMatrixInverse(&det, projM));
			const auto detX = XMVectorGetX(det);
			const bool bad = !std::isfinite(detX) || detX == 0.0f ||
				XMMatrixIsNaN(inv) || XMMatrixIsInfinite(inv);
			if (bad) {
				++g_invProjRejects;
				if (g_invProjState != 1) {
					g_invProjState = 1;
					static std::uint32_t logs = 0;
					if (logs < 40) {
						++logs;
						logger::warn(
							FMT_STRING("INVPROJ singular/non-finite — SKIPPED (det={}, rejects={}) proj rows: "
									   "[{} {} {} {}] [{} {} {} {}] [{} {} {} {}] [{} {} {} {}]"),
							detX, g_invProjRejects,
							proj->m[0][0], proj->m[0][1], proj->m[0][2], proj->m[0][3],
							proj->m[1][0], proj->m[1][1], proj->m[1][2], proj->m[1][3],
							proj->m[2][0], proj->m[2][1], proj->m[2][2], proj->m[2][3],
							proj->m[3][0], proj->m[3][1], proj->m[3][2], proj->m[3][3]);
					}
				}
				return;  // leave staging+0x1d0 and the eye-1 mirror as they were
			}
			if (g_invProjState != 0) {
				g_invProjState = 0;
				logger::info(FMT_STRING("INVPROJ recovered (det={}, total rejects={})"), detX, g_invProjRejects);
			}
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

		// --- widget fit (v0.2.68) ------------------------------------------------
		// The vanilla VR scope widget (Data\Meshes\VR\Scope\world_scope.nif) hangs off
		// the engine's "ScopeParent" NiNode at player+0x7d0: TS_SetupScopeRig
		// (0x140ef21a0) does ScopeParent->AttachChild(WSScopeModel->model). Its render
		// surface `render_circle:0` is a flat disc of radius 7.852 centred exactly on
		// ScopeParent's origin (measured with tools/nif-inspect.py), so
		//        scale = aperture_radius / 7.852
		// fits the widget to a real scope's lens. Shipped scopes measure 0.76–4.56, i.e.
		// scale 0.097–0.581 — the vanilla widget is 2–6x oversized, which is why the
		// real scope mesh shows through the middle of it.
		//
		// TWO THINGS THIS MUST GET RIGHT, both established by measurement:
		//
		// 1. ScopeParent's WORLD transform is NOT recomputed per frame. Read live
		//    2026-08-09 it was bit-for-bit identical across 3 s while the camera's moved.
		//    The engine only refreshes it at equip/3D-change. So writing the local
		//    transform alone does NOTHING VISIBLE — NiAVObject::Update must follow,
		//    exactly as the engine's own path (FUN_140f0a9f0) does after writing it.
		//
		// 2. The engine REWRITES that local transform at equip. Offsets are applied to a
		//    captured BASELINE, never accumulated onto the current value.
		//
		//    *** v0.2.69 BUG FIX — v0.2.68 invalidated that baseline on the WRONG EVENT. ***
		//    It dropped the baseline on SCOPE-IN, assuming the engine had rewritten the
		//    node by then. It has not: the engine rewrites at EQUIP. So every
		//    scope-out/scope-in cycle re-captured OUR OWN previously written value as the
		//    new baseline and stacked the offset on top. Field evidence:
		//        WIDGET FIT baseline: translate=(0.000,-59.800,16.000)
		//    against a true engine value of (0, 0, 4) — the widget had walked ~60 units off
		//    the gun and could not be found anywhere in the scene.
		//
		//    The correct invalidation signal is not an event at all: compare the node's
		//    current values with the exact ones we last wrote. Identical => still ours, keep
		//    the baseline. Different => something else (the engine, at equip) wrote it, so
		//    what is there now IS pristine and becomes the new baseline. Exact float
		//    comparison is right here precisely because we wrote those bits ourselves.
		constexpr std::uintptr_t kScopeParentInPlayer = 0x7d0;
		constexpr float          kVanillaRenderCircleRadius = 7.852f;

		struct WidgetState
		{
			float baseTx = 0.0f, baseTy = 0.0f, baseTz = 0.0f, baseScale = 1.0f;
			float wroteTx = 0.0f, wroteTy = 0.0f, wroteTz = 0.0f, wroteScale = 0.0f;
			float lastScale = 0.0f, lastOx = 0.0f, lastOy = 0.0f, lastOz = 0.0f;
			bool  captured = false;
			bool  applied = false;
		};
		WidgetState g_widget;
		// v0.2.119: cross-thread mirror of g_widget.applied (read by the game-thread
		// presence gate; written on the render thread wherever `applied` changes).
		std::atomic_bool g_fitAppliedAtomic{ false };

		void WriteScopeParent(std::uintptr_t a_sp, float a_tx, float a_ty, float a_tz, float a_scale)
		{
			auto* t = reinterpret_cast<float*>(a_sp + 0x60);  // NiAVObject local translate
			t[0] = a_tx;
			t[1] = a_ty;
			t[2] = a_tz;
			*reinterpret_cast<float*>(a_sp + 0x6c) = a_scale;  // local scale
			// NiUpdateData, zeroed — the engine's own call site builds a zeroed block of
			// this shape before calling. Oversized on purpose; zeros are safe.
			alignas(16) std::uint8_t upd[0x30]{};
			Fn<NiAVObjectUpdate_t>(0x1c22fb0)(a_sp, upd);
		}

		// --- derived scope FOV (v0.2.90) -----------------------------------------
		// Stop hand-tuning scopeFovDegrees. What the player should perceive is the
		// scope's real magnification M, and that fixes the render FOV completely:
		//
		//   the lens disc has world radius  R = 7.852 * ScopeParent.worldScale
		//   the eye sits distance           d  from the disc centre
		//   so the disc subtends            tan(theta_disc / 2) = R / d
		//   and a point rendered at angle b lands where the eye sees it at
		//   tan-1( tan(b) * (R/d) / tan(theta_render/2) ), i.e.
		//                                   M = tan(theta_disc/2) / tan(theta_render/2)
		//   therefore                       theta_render = 2*atan( (R/d) / M )
		//
		// M comes from the weapon's zoomData fovMult (6.0 on the hunting rifle's
		// long scope, read live), so this generalises to every optic for free.
		//
		// Computing d per render is not an approximation of a fixed value, it is
		// more correct than one: a real scope's magnification does not change as
		// you move your head back, but its visible field narrows. Recomputing does
		// exactly that. It also costs nothing -- SetCameraFOV already runs every render.
		//
		// Layout, all read out of the VR binary rather than assumed:
		//   NiAVObject world transform +0x70, world translate +0xa0, world scale +0xac
		//     (NiAVObject::UpdateWorldData 0x141c23740 copies local 0x30..0x6c into
		//      exactly those slots when a node has no parent)
		//   PlayerCamera singleton  [0x145930608]   (RIP-decoded at 0x1412c55f6)
		//   camera root = PlayerCamera+0x20         (TESCamera::GetCameraRoot 0x14081ee70
		//      is nothing but a refcounted read of that field)
		constexpr std::uintptr_t kPlayerCameraPtr = 0x5930608;
		constexpr std::uintptr_t kCameraRootInCamera = 0x20;
		constexpr std::uintptr_t kWorldTranslate = 0xa0;
		constexpr std::uintptr_t kWorldScale = 0xac;

		float g_derivedFovDeg = 0.0f;  // last derived value, reported whether used or not
		float g_lastFovDeg = 0.0f;     // the FOV actually handed to SetCameraFOV
		float g_derivedEyeDist = 0.0f;
		float g_derivedDiscR = 0.0f;

		// Returns 0 when the geometry is not available or not sane, so the caller
		// keeps the configured value rather than pointing the camera at a guess.
		float DeriveScopeFovDegrees(std::uintptr_t a_player)
		{
			g_derivedFovDeg = 0.0f;
			g_derivedEyeDist = 0.0f;
			g_derivedDiscR = 0.0f;

			const auto sp = *reinterpret_cast<std::uintptr_t*>(a_player + kScopeParentInPlayer);
			if (!sp) {
				return 0.0f;
			}
			const auto playerCam = *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + kPlayerCameraPtr);
			if (!playerCam) {
				return 0.0f;
			}
			const auto camRoot = *reinterpret_cast<std::uintptr_t*>(playerCam + kCameraRootInCamera);
			if (!camRoot) {
				return 0.0f;
			}

			const auto* disc = reinterpret_cast<const float*>(sp + kWorldTranslate);
			const auto  discScale = *reinterpret_cast<const float*>(sp + kWorldScale);
			const auto* eye = reinterpret_cast<const float*>(camRoot + kWorldTranslate);

			const float dx = eye[0] - disc[0];
			const float dy = eye[1] - disc[1];
			const float dz = eye[2] - disc[2];
			const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
			const float R = kVanillaRenderCircleRadius * discScale;

			// Bounds are deliberately wide: they exist to catch a stale or garbage
			// transform (a node the engine has not updated reads as the origin, which
			// would make d enormous or zero), not to second-guess a real measurement.
			if (!std::isfinite(d) || !std::isfinite(R) || d < 1.0f || d > 200.0f || R <= 0.001f) {
				return 0.0f;
			}

			const float m = ScopeIdent::FovMult();
			if (!(m > 0.01f)) {
				return 0.0f;
			}

			const float fovDeg = 2.0f * std::atan((R / d) / m) * (180.0f / 3.14159265358979f);
			if (!std::isfinite(fovDeg) || fovDeg < 0.05f || fovDeg > 170.0f) {
				return 0.0f;
			}

			g_derivedFovDeg = fovDeg;
			g_derivedEyeDist = d;
			g_derivedDiscR = R;
			return fovDeg;
		}

		// --- automatic widget placement (v0.2.92) --------------------------------
		//
		// One hand-tuned offset per scope does not scale, and the 2026-08-10
		// screenshots showed what a missing one looks like: the disc floating above
		// and behind the optic. So derive the placement instead.
		//
		// The target is the OCULAR FACE — the rear end of the scope, the end the
		// player looks into. Two facts make that computable without mesh data:
		//
		//   * the walk gives a world-space bounding sphere for the P-Scope subtree,
		//     centre C and radius r;
		//   * when the scope is raised the eye is very nearly ON the tube axis, so
		//     the direction from C toward the eye E IS the tube axis, to within the
		//     small angle the player's head is off-centre.
		//
		// so   target = C + normalize(E - C) * r.
		//
		// This is a HEURISTIC and is written down as one. A bounding sphere over a
		// long tube has radius ~= half its length, so the target lands near the rear
		// face; the error is the sphere's overshoot past a flat face (about
		// tube_radius^2 / length, well under a unit for a real scope) PLUS whatever
		// mounts and rails inflate the sphere by. The second term is the one to
		// distrust, and it is exactly why this is computed but not applied by
		// default: `widgetAutoPlace` is off, the candidate is reported every probe,
		// and it can be checked against the hunting rifle's VR-confirmed -1.8 before
		// anyone trusts it on a scope nobody has tuned.
		//
		// ScopeParent's translate is in its PARENT's space, so the world-space target
		// has to come back through that transform: v = R^T * (W - T) / s.
		//
		// v0.2.94 — WHY THIS IS NOW CLOSED-LOOP.
		//
		// v0.2.92/93 computed one offset through that chain and applied it. The
		// 2026-08-11 bench refuted the result (predicted |offset| ~1.8 on the
		// hunting rifle, got 3.07) and, worse, the chain could not be checked from
		// outside the process: reads taken over separate DevBench round trips
		// sample DIFFERENT FRAMES, and a rifle held in VR moves units between them,
		// so every "verification" of it was measuring hand tremor as much as
		// geometry. An open-loop transform that cannot be validated is exactly the
		// shape of defect this project keeps shipping.
		//
		// So stop asserting the transform and MEASURE it. Aim at the target, then
		// each frame read where the disc actually landed and correct by the
		// residual:
		//
		//     err   = target - ScopeParent.world
		//     local += R^T * err / s          (best estimate of the mapping)
		//
		// The estimate only has to be roughly right for this to converge — it is a
		// contraction as long as the assumed mapping is within ~90 degrees of the
		// real one. A transposed matrix, a wrong parent offset, an intermediate
		// node or a non-unit scale all get absorbed instead of silently mis-aiming
		// the disc. Divergence is detected and backed out rather than left running.
		//
		// Everything the loop reads is sampled inside ONE render, so it is immune
		// to the cross-frame sampling error that invalidated the manual check.
		constexpr std::uintptr_t kParentInNiAVObject = 0x28;
		constexpr std::uintptr_t kWorldTransform = 0x70;
		constexpr std::size_t    kMatrixRowStride = 4;  // NiMatrix3 is 3 rows of 4

		struct PlacementInfo
		{
			bool  valid = false;
			float offset[3] = {};       // candidate local offset, relative to the baseline
			float target[3] = {};       // the world point it aims at
			float baseWorld[3] = {};    // where the untouched engine baseline puts the disc
			float boundCenter[3] = {};  // the optic's world bounding sphere
			float boundRadius = 0.0f;
			float miss = 0.0f;  // |target - baseWorld|: how far off today's placement is
			char  reason[72] = "not computed";
			// Which of the two targets was taken, and how far apart they were.
			// The heuristic is what an unmeasured (modded) scope must fall back
			// on, so the gap between it and the exact census answer on a scope we
			// DO have measured is the only honest estimate of its error.
			char  method[16] = "none";
			// True when the target came from the census face, which is pure weapon
			// geometry and does not involve the eye. Such a target can be recomputed
			// every frame without jitter -- and MUST be, see ApplyWidgetFit.
			bool  eyeIndependent = false;
			bool  haveBoth = false;
			float agreement = 0.0f;  // |exact - heuristic|, world units
			// --- closed loop state (v0.2.94) ---
			bool  converged = false;   // kept for the report; open loop is done at once
			bool  diverged = false;
			bool  warnedResidual = false;
			int   steps = 0;
			float residual = 0.0f;      // |target - where the disc actually is|
			float bestResidual = 1.0e9f;
			float bestOffset[3] = {};   // the best offset seen, restored on divergence
			float discWorld[3] = {};    // where the disc actually is, THIS frame
			// Single-frame consistency check on the assumed parent transform:
			// |parent.world(ScopeParent.local) - ScopeParent.world|. Large means the
			// layout assumption is wrong -- which the loop survives, but it is worth
			// knowing rather than inferring.
			float parentResidual = -1.0f;
		};
		PlacementInfo    g_place;
		std::atomic_bool g_placeDirty{ false };

		void PlaceDecline(const char* a_why)
		{
			g_place = PlacementInfo{};
			std::snprintf(g_place.reason, sizeof(g_place.reason), "%s", a_why);
		}

		// Always runs; never writes anything. The caller decides whether to use it.
		//
		// a_keepLoopState is set when the closed loop calls this to refresh the
		// target for a new frame: the geometry must be re-read (the weapon moves)
		// but the loop's accumulated offset, step count and best-so-far must NOT be
		// reset, or the servo restarts from scratch every frame and never converges.
		// v0.2.124 — ONE EURO CAMERA DAMPING (STATUS 3.7d). Filters the scope
		// camera's WORLD orientation once per live fill, at the single coherent
		// point after SetCameraFOV's Update has derived a fresh weapon-parented
		// pose and before anything downstream consumes it (culling planes, the
		// camera-state commits, AccumulateScene all read the filtered basis).
		// Feed-forward and self-healing: next fill regenerates raw pose from
		// the parent, the engine never accumulates our correction. Orientation
		// only — positional tremor is unamplified parallax; filtering position
		// risks the camera swimming inside the tube. Never touches weapon or
		// hand nodes (FRIK's mistake), never ScopeParent: the widget disc
		// tracks the honest weapon, only the lens IMAGE is damped, and the
		// composited reticle marks the damped camera's boresight so image and
		// reticle stay mutually coherent (both lag true aim by <= maxLag).
		// While pose-frozen no renders happen, so thaw arrives as a dt gap
		// > camSmoothResetMs and snaps to raw — no lurch, no bogus velocity.
		struct CamSmoothState
		{
			OneEuro::QuatFilter f;
			long long           lastQpc = 0;
			long long           qpcFreq = 0;
			std::uintptr_t      cam = 0;
			std::uint32_t       resets = 0;
		};
		CamSmoothState   g_camSmooth{};
		std::atomic_bool g_camSmoothResetReq{ false };

		void ApplyCamSmooth(std::uintptr_t a_cam) noexcept
		{
			if (!*Settings::camSmoothEnabled) {
				g_camSmooth.f.primed = false;
				return;
			}
			auto* m = reinterpret_cast<float*>(a_cam + 0x70);
			// Reject mid-teardown garbage: 9 floats finite, rows near unit.
			for (const int base : { 0, 4, 8 }) {
				const float l2 = m[base] * m[base] + m[base + 1] * m[base + 1] + m[base + 2] * m[base + 2];
				if (!std::isfinite(l2) || std::fabs(l2 - 1.0f) > 0.01f) {
					g_camSmooth.f.primed = false;
					return;
				}
			}
			float qraw[4];
			OneEuro::QuatFromBasis(m, qraw);
			::LARGE_INTEGER now{};
			::QueryPerformanceCounter(&now);
			if (!g_camSmooth.qpcFreq) {
				// own static — g_qpcFreq only initializes under perfTimers
				::LARGE_INTEGER fr{};
				::QueryPerformanceFrequency(&fr);
				g_camSmooth.qpcFreq = fr.QuadPart;
			}
			const double dtRaw = g_camSmooth.lastQpc
			                         ? double(now.QuadPart - g_camSmooth.lastQpc) / double(g_camSmooth.qpcFreq)
			                         : 1.0;
			g_camSmooth.lastQpc = now.QuadPart;
			const double resetS = double(*Settings::camSmoothResetMs) / 1000.0;
			if (!g_camSmooth.f.primed || dtRaw > resetS || a_cam != g_camSmooth.cam ||
				g_camSmoothResetReq.exchange(false)) {
				std::memcpy(g_camSmooth.f.q, qraw, sizeof(qraw));
				std::memcpy(g_camSmooth.f.qRawPrev, qraw, sizeof(qraw));
				g_camSmooth.f.omegaHat = 0.0f;
				g_camSmooth.f.primed = true;
				g_camSmooth.cam = a_cam;
				++g_camSmooth.resets;
				static bool s_logged = false;
				if (!s_logged) {
					s_logged = true;
					logger::info(FMT_STRING("CAM SMOOTH: enabled minCutoff={} beta={} dCutoff={} "
					                        "resetMs={} snap={} maxLag={}"),
						*Settings::camSmoothMinCutoffHz, *Settings::camSmoothBeta,
						*Settings::camSmoothDCutoffHz, *Settings::camSmoothResetMs,
						*Settings::camSmoothSnapDegrees, *Settings::camSmoothMaxLagDegrees);
				}
				return;  // pose already raw — nothing to write
			}
			const float dt = (std::clamp)(float(dtRaw), 1.0e-4f, 0.1f);
			// Adaptive core (Casiez 2012). NOTE: the magnitude-only derivative
			// cannot average oscillating tremor toward zero — at rest the
			// effective cutoff is minCutoff + beta*mean|tremor rate|. Tune
			// minCutoff FIRST with beta=0 (the ladder in the TOML).
			const float ang = OneEuro::Angle(g_camSmooth.f.qRawPrev, qraw);
			std::memcpy(g_camSmooth.f.qRawPrev, qraw, sizeof(qraw));
			const float omega = ang / dt;
			const float aD = OneEuro::Alpha(dt, float(*Settings::camSmoothDCutoffHz));
			g_camSmooth.f.omegaHat += aD * (omega - g_camSmooth.f.omegaHat);
			const float fc = float(*Settings::camSmoothMinCutoffHz) +
			                 float(*Settings::camSmoothBeta) * g_camSmooth.f.omegaHat;
			const float a = OneEuro::Alpha(dt, fc);
			const float snapRad = float(*Settings::camSmoothSnapDegrees) * 0.017453292f;
			const float lagRad = float(*Settings::camSmoothMaxLagDegrees) * 0.017453292f;
			if (OneEuro::Angle(g_camSmooth.f.q, qraw) > snapRad) {
				// Discontinuity (snap turn / teleport): adopt raw instantly.
				// Seeding omegaHat = omega leaves the cutoff elevated for a few
				// hundred ms after — motion-plausible, not a bug (see TOML).
				std::memcpy(g_camSmooth.f.q, qraw, sizeof(qraw));
				g_camSmooth.f.omegaHat = omega;
			} else {
				OneEuro::Slerp(g_camSmooth.f.q, qraw, a, g_camSmooth.f.q);
			}
			// Aim honesty: filtered-vs-raw divergence hard-bounded; at rest the
			// filter converges to raw, so precision shots see the true pose.
			const float lag = OneEuro::Angle(g_camSmooth.f.q, qraw);
			if (lag > lagRad && lag > 1.0e-6f) {
				OneEuro::Slerp(g_camSmooth.f.q, qraw, 1.0f - lagRad / lag, g_camSmooth.f.q);
			}
			OneEuro::BasisFromQuat(g_camSmooth.f.q, m);
		}

		void ComputeAutoPlacement(std::uintptr_t a_player, bool a_keepLoopState = false)
		{
			if (!g_widget.captured) {
				PlaceDecline("no ScopeParent baseline yet");
				return;
			}
			const auto sp = *reinterpret_cast<std::uintptr_t*>(a_player + kScopeParentInPlayer);
			if (!sp) {
				PlaceDecline("no ScopeParent");
				return;
			}
			const auto parent = *reinterpret_cast<std::uintptr_t*>(sp + kParentInNiAVObject);
			if (!parent) {
				PlaceDecline("ScopeParent has no parent to convert through");
				return;
			}

			float      center[3] = {};
			float      radius = 0.0f;
			const bool haveBound = ScopeIdent::ScopeBound(center, radius);

			const auto playerCam = *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + kPlayerCameraPtr);
			if (!playerCam) {
				PlaceDecline("no PlayerCamera");
				return;
			}
			const auto camRoot = *reinterpret_cast<std::uintptr_t*>(playerCam + kCameraRootInCamera);
			if (!camRoot) {
				PlaceDecline("no camera root");
				return;
			}
			const auto* eye = reinterpret_cast<const float*>(camRoot + kWorldTranslate);

			// Parent transform: world = T + s * (R * v).
			const auto* pm = reinterpret_cast<const float*>(parent + kWorldTransform);
			const auto* pt = reinterpret_cast<const float*>(parent + kWorldTranslate);
			const float ps = *reinterpret_cast<const float*>(parent + kWorldScale);
			if (!std::isfinite(ps) || ps < 1.0e-4f) {
				PlaceDecline("parent scale is degenerate");
				return;
			}
			// TRANSPOSED ON READ (v0.2.96) — the engine stores this column-major, so
			// the true mapping is world = T + s * (M_stored^T * v). See the proof in
			// ScopeIdent::ReadGeom. With R read this way both the forward use below
			// and the R^T inverse further down are the textbook forms.
			float R[3][3];
			for (std::size_t r = 0; r < 3; ++r) {
				for (std::size_t c = 0; c < 3; ++c) {
					R[r][c] = pm[c * kMatrixRowStride + r];
					if (!std::isfinite(R[r][c])) {
						PlaceDecline("parent rotation is not finite");
						return;
					}
				}
			}

			// Where the engine's own baseline puts the disc, in world space, per the
			// ASSUMED transform.
			const float L[3] = { g_widget.baseTx, g_widget.baseTy, g_widget.baseTz };
			float       baseWorld[3];
			for (std::size_t r = 0; r < 3; ++r) {
				baseWorld[r] = pt[r] + ps * (R[r][0] * L[0] + R[r][1] * L[1] + R[r][2] * L[2]);
			}

			// Single-frame check on that assumption: push ScopeParent's CURRENT local
			// translate through the parent and compare against its actual world
			// translate. Both are read in this render, so unlike an external probe
			// this cannot be confounded by the weapon moving between samples. A large
			// residual does not stop anything — the closed loop absorbs it — but it
			// says plainly that the layout is not what the code believes.
			const auto* curLocal = reinterpret_cast<const float*>(sp + 0x60);
			const auto* curWorld = reinterpret_cast<const float*>(sp + kWorldTranslate);
			float       predicted[3];
			for (std::size_t r = 0; r < 3; ++r) {
				predicted[r] = pt[r] + ps * (R[r][0] * curLocal[0] + R[r][1] * curLocal[1] +
				                                R[r][2] * curLocal[2]);
			}
			const float prx = predicted[0] - curWorld[0];
			const float pry = predicted[1] - curWorld[1];
			const float prz = predicted[2] - curWorld[2];
			const float parentResidual = std::sqrt(prx * prx + pry * pry + prz * prz);

			// Candidate 1, the HEURISTIC: target = C + normalize(E - C) * r.
			float      heuristic[3] = {};
			bool       haveHeuristic = false;
			const float ax = eye[0] - center[0];
			const float ay = eye[1] - center[1];
			const float az = eye[2] - center[2];
			const float alen = std::sqrt(ax * ax + ay * ay + az * az);
			if (haveBound && std::isfinite(alen) && alen >= 1.0f) {
				heuristic[0] = center[0] + ax / alen * radius;
				heuristic[1] = center[1] + ay / alen * radius;
				heuristic[2] = center[2] + az / alen * radius;
				haveHeuristic = true;
			}

			// Candidate 2, the EXACT answer: the census-measured face centre pushed
			// through the live shape's world transform. Preferred when available —
			// it is a real measurement of the glass rather than an inference from a
			// sphere that mounts and rails inflate.
			float      exact[3] = {};
			const bool haveExact = ScopeIdent::OcularFaceWorld(exact);

			if (!haveExact && !haveHeuristic) {
				PlaceDecline(haveBound ? "eye is at the optic centre; no axis"
				                       : "no census row and no usable optic bound");
				return;
			}

			const float* chosen = haveExact ? exact : heuristic;
			const float  target[3] = { chosen[0], chosen[1], chosen[2] };

			// Back into the parent's space, then express as a delta from the baseline
			// so it drops straight into the existing offset path.
			float u[3];
			for (std::size_t r = 0; r < 3; ++r) {
				u[r] = (target[r] - pt[r]) / ps;
			}
			float local[3];
			for (std::size_t c = 0; c < 3; ++c) {
				local[c] = R[0][c] * u[0] + R[1][c] * u[1] + R[2][c] * u[2];
			}

			PlacementInfo p;
			for (std::size_t k = 0; k < 3; ++k) {
				p.offset[k] = local[k] - L[k];
				p.target[k] = target[k];
				p.baseWorld[k] = baseWorld[k];
				p.boundCenter[k] = center[k];
				if (!std::isfinite(p.offset[k])) {
					PlaceDecline("computed offset is not finite");
					return;
				}
			}
			p.boundRadius = radius;
			p.eyeIndependent = haveExact;
			p.parentResidual = parentResidual;
			for (std::size_t k = 0; k < 3; ++k) {
				p.discWorld[k] = curWorld[k];
				p.bestOffset[k] = p.offset[k];
			}
			std::snprintf(p.method, sizeof(p.method), "%s", haveExact ? "census" : "bound");
			p.haveBoth = haveExact && haveHeuristic;
			if (p.haveBoth) {
				const float gx = exact[0] - heuristic[0];
				const float gy = exact[1] - heuristic[1];
				const float gz = exact[2] - heuristic[2];
				p.agreement = std::sqrt(gx * gx + gy * gy + gz * gz);
			}
			const float mx = target[0] - baseWorld[0];
			const float my = target[1] - baseWorld[1];
			const float mz = target[2] - baseWorld[2];
			p.miss = std::sqrt(mx * mx + my * my + mz * mz);

			// A scope is centimetres from the mount. An offset the size of a room
			// means a transform this code misread, and applying it would shove the
			// disc somewhere it can never be seen — which reads as "the render broke".
			const float mag = std::sqrt(p.offset[0] * p.offset[0] + p.offset[1] * p.offset[1] +
			                            p.offset[2] * p.offset[2]);
			if (mag > 40.0f) {
				PlaceDecline("offset implausibly large; refusing");
				return;
			}

			// v0.2.122 — STALENESS GUARD. The post-load probe walks fine but its
			// WORLD transforms still hold the pre-placement rig pose (the same
			// (34.7,15.5,78.1) in every session's log) until the first skeleton
			// update lands. That garbage produced a 30.7-unit offset in the 17:03
			// field run — under the 40-unit misread cap above — which latched a
			// census placement and parked the disc out of sight. The eye can never
			// be more than arm's length + weapon (~60 units) from an equipped
			// scope, so distance-from-eye is the principled test; the decline
			// feeds PresenceFit's bounded retry (v0.2.121), which converges as
			// soon as the transforms are real (26 ms after load in the logs).
			{
				const float dx = target[0] - eye[0];
				const float dy = target[1] - eye[1];
				const float dz = target[2] - eye[2];
				const float eyeDist = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (eyeDist > static_cast<float>(*Settings::widgetPlaceMaxEyeDist)) {
					PlaceDecline("target implausibly far from eye (stale pre-placement transforms)");
					return;
				}
			}

			p.valid = true;
			p.converged = true;  // open loop: the answer is exact, there is no second pass
			p.steps = 1;
			p.bestResidual = 0.0f;
			std::snprintf(p.reason, sizeof(p.reason), "ok");
			if (a_keepLoopState && g_place.valid) {
				// Carry the loop forward: its offset IS the accumulated correction,
				// and re-deriving it open-loop here would discard everything the
				// closed loop has learned.
				for (std::size_t k = 0; k < 3; ++k) {
					p.offset[k] = g_place.offset[k];
					p.bestOffset[k] = g_place.bestOffset[k];
				}
				p.steps = g_place.steps;
				p.converged = g_place.converged;
				p.diverged = g_place.diverged;
				p.residual = g_place.residual;
				p.bestResidual = g_place.bestResidual;
			}
			g_place = p;
		}

		// OBSERVE the achieved placement. Does NOT feed back (v0.2.99).
		//
		// v0.2.94-98 ran this as a closed loop: measure where the disc landed,
		// correct the offset by the residual, repeat. That was a REGRESSION, and the
		// log made it plain -- the offset marched instead of settling:
		//
		//   (0.45,4.58,-0.43) -> (0.53,4.83,-1.02) -> (0.64,5.29,-2.15)
		//   ... twelve steps ... -> (1.45,6.95,-6.40)   then hit the step cap
		//
		// against an open-loop answer of (0.35,4.33,0.15). It walked the disc clean
		// off the weapon, on both scopes, and the guard missed it because the
		// RESIDUAL stayed small (~0.3-0.5) the whole time while the OFFSET ran away.
		// I guarded the wrong quantity.
		//
		// Two causes, both cross-frame errors of exactly the kind this project keeps
		// meeting:
		//
		//  1. It compared a target read THIS frame against a disc position produced
		//     by LAST frame's write. While the weapon moves -- scope-in animation, or
		//     just hands in VR -- that difference is mostly the weapon's motion, so
		//     the loop integrated the motion into the offset. An integrator with no
		//     reference can only drift.
		//  2. The target was itself built from shape transforms cached at PROBE time,
		//     so it lagged reality by however long ago the probe ran. That is the
		//     persistent residual the loop was chasing -- it was never a transform
		//     error at all.
		//
		// Fix (2) properly (ScopeIdent::OcularFaceWorld now re-reads the LIVE shape
		// transform) and the open-loop answer is exact: every term -- face, shape
		// transform, parent transform, baseline -- is read in ONE frame and combined
		// into a LOCAL-space offset, so weapon motion cancels identically. There is
		// nothing left for a servo to fix.
		//
		// The loop existed to paper over a transform that could not be trusted. That
		// transform is now proven correct both statically (the transpose convention
		// read out of NiTransform::operator*) and live (parentResidual = 0.000). So
		// this measures and reports; it does not act. The number is still worth
		// having -- it is what would catch the transform going wrong again -- and
		// unlike the old loop it cannot make anything worse by being large.
		void ObserveAutoPlacement(std::uintptr_t a_sp)
		{
			if (!g_place.valid) {
				return;
			}
			const auto* world = reinterpret_cast<const float*>(a_sp + kWorldTranslate);
			const float ex = g_place.target[0] - world[0];
			const float ey = g_place.target[1] - world[1];
			const float ez = g_place.target[2] - world[2];
			const float err = std::sqrt(ex * ex + ey * ey + ez * ez);
			if (!std::isfinite(err)) {
				return;
			}
			g_place.residual = err;
			for (std::size_t k = 0; k < 3; ++k) {
				g_place.discWorld[k] = world[k];
			}
			// One frame of weapon motion is a legitimate part of this, so the
			// threshold is loose: it is looking for a broken transform, not tenths of
			// a unit. The lens is ~1.3 units in radius.
			if (err > 2.0f && !g_place.warnedResidual) {
				g_place.warnedResidual = true;
				logger::warn(FMT_STRING("WIDGET AUTO-PLACE: disc landed {:.2f} units from its target "
				                        "(offset=({:.2f},{:.2f},{:.2f})). Some of that is one frame of "
				                        "weapon motion; a persistently large value means the placement "
				                        "transform is wrong."),
					err, g_place.offset[0], g_place.offset[1], g_place.offset[2]);
			}
		}


		// v0.2.121: what a fit call actually did with PLACEMENT — the load-in
		// defect was PresenceFit latching "done" on a call that had (correctly)
		// declined a garbage post-load placement, which made the good probe 38 ms
		// later invisible. Only the callee knows whether the census gate consumed
		// a placement, so it says so.
		enum class FitOutcome
		{
			kNotWritten,       // no ScopeParent / fit disabled / scale refused
			kPlacedCensus,     // eye-independent census target applied
			kPlacedHeuristic,  // eye-relative bound heuristic applied (live path only)
			kFallbackOffsets   // TOML/global offsets only — auto-place declined or off
		};

		FitOutcome ApplyWidgetFit(std::uintptr_t a_player, bool a_censusPlacementOnly = false)
		{
			const auto sp = *reinterpret_cast<std::uintptr_t*>(a_player + kScopeParentInPlayer);
			if (!sp) {
				return FitOutcome::kNotWritten;
			}

			auto*      t = reinterpret_cast<float*>(sp + 0x60);
			auto*      s = reinterpret_cast<float*>(sp + 0x6c);
			const bool stillOurs = g_widget.applied &&
			                       t[0] == g_widget.wroteTx && t[1] == g_widget.wroteTy &&
			                       t[2] == g_widget.wroteTz && *s == g_widget.wroteScale;

			// Re-baseline whenever the node holds something we did NOT write — that is the
			// engine having rewritten it at equip, and only then is the value pristine.
			// Doing this on an event (v0.2.68 used scope-in) re-captured our own output and
			// compounded the offset every cycle; see the note above.
			const bool rebaselined = !stillOurs;
			if (!stillOurs) {
				g_widget.baseTx = t[0];
				g_widget.baseTy = t[1];
				g_widget.baseTz = t[2];
				g_widget.baseScale = *s;
				g_widget.captured = true;
				g_widget.applied = false;
				g_fitAppliedAtomic.store(false, std::memory_order_relaxed);
				// v0.2.119: an engine rewrite of ScopeParent IS the equip signal —
				// re-identify the scope so a weapon swap can never keep the old
				// weapon's aperture/placement (field: first-draw giant widget).
				ScopeIdent::Request();
				logger::info(FMT_STRING("WIDGET FIT baseline: translate=({:.3f},{:.3f},{:.3f}) scale={:.3f}"),
					g_widget.baseTx, g_widget.baseTy, g_widget.baseTz, g_widget.baseScale);
				// Sanity tripwire for exactly the failure that produced this fix: the engine
				// parks ScopeParent within a few units of the weapon. A baseline far from
				// that means we captured corrupted state, and every offset from here is
				// measured from the wrong origin. Say so loudly rather than fit to garbage.
				const float d2 = g_widget.baseTx * g_widget.baseTx +
				                 g_widget.baseTy * g_widget.baseTy +
				                 g_widget.baseTz * g_widget.baseTz;
				if (d2 > 50.0f * 50.0f) {
					logger::warn(FMT_STRING("WIDGET FIT baseline looks CORRUPT (|t|={:.1f} > 50) — "
					                        "re-equip the weapon to let the engine restore ScopeParent"),
						std::sqrt(d2));
				}
			}

			if (!*Settings::widgetFitEnabled) {
				// Turned off after being applied: restore the engine's own values once, so
				// toggling it live is a clean A/B rather than a one-way door.
				if (stillOurs && g_widget.captured) {
					WriteScopeParent(sp, g_widget.baseTx, g_widget.baseTy, g_widget.baseTz, g_widget.baseScale);
					g_widget.applied = false;
					g_fitAppliedAtomic.store(false, std::memory_order_relaxed);
					logger::info("WIDGET FIT off — restored engine transform"sv);
				}
				return FitOutcome::kNotWritten;
			}

			// v0.2.85: per-scope, from the equipped weapon's node names. Falls back
			// to the widgetApertureRadius setting for an unrecognised scope, so an
			// optic the table has never seen behaves exactly as it did before.
			const auto  aperture = ScopeIdent::ApertureRadius();
			const auto  scaleOverride = static_cast<float>(*Settings::widgetScaleOverride);
			// v0.2.123 — under-aperture sizing (encapsulation layer 2): shrink the
			// disc slightly inside the housing hole so the seam is never a bright
			// picture pixel. Applies after the per-scope table, so every entry
			// keeps its relative fit; the override (a bisect tool) bypasses it.
			const float apScale = std::clamp(static_cast<float>(*Settings::widgetApertureScale), 0.8f, 1.1f);
			const float scale = scaleOverride > 0.0f ? scaleOverride : (aperture * apScale) / kVanillaRenderCircleRadius;
			// A zero/absurd scale makes the lens vanish or swallow the view, and the user
			// cannot tell that apart from a broken render — refuse instead of guessing.
			if (!(scale > 0.001f && scale < 8.0f)) {
				static bool warned = false;
				if (!warned) {
					warned = true;
					logger::warn(FMT_STRING("WIDGET FIT refused: scale {} out of range (aperture={} override={})"),
						scale, aperture, scaleOverride);
				}
				return FitOutcome::kNotWritten;
			}

			// v0.2.91: per-scope, falling back per axis to the global settings.
			float ox = 0.0f, oy = 0.0f, oz = 0.0f;
			ScopeIdent::WidgetOffsets(ox, oy, oz);

			// v0.2.92: the automatic placement is LATCHED, not tracked. The ocular
			// face is fixed on the weapon; only the axis ESTIMATE uses the eye, so
			// recomputing per frame would make the disc creep with head motion — a
			// jitter at 6x magnification, and a full NiAVObject::Update every frame
			// to produce it. Recompute on equip (the engine re-baselining
			// ScopeParent), on demand, or until it first succeeds.
			// RECOMPUTE EVERY FRAME when the target is eye-independent (v0.2.100).
			//
			// ScopeParent hangs off PrimaryUIAttachNode, NOT off the weapon, so the
			// local offset that puts the disc on the lens depends on the RELATIVE
			// pose of the weapon and that UI node. That relationship is not fixed:
			// it changes through the scope-raise animation and as the player's hands
			// move relative to their head. Computing the offset once at equip and
			// holding it therefore bakes in whatever transitional pose happened to
			// exist at that instant -- which is exactly what the user hit, and why
			// forcing a fresh probe made the disc snap back onto the lens.
			//
			// Latched at equip: (-0.11, -0.76, -1.15).  Recomputed live: (-0.00,
			// -0.86, -0.42) -- stable to two decimals across five forced latches,
			// and identical to the value confirmed by eye on 2026-08-11.
			//
			// A census target is pure weapon geometry with no eye term, so
			// recomputing it per frame is stable, not jittery. The bound HEURISTIC
			// does use the eye, so that one stays latched -- recomputing it per
			// frame would creep the disc with head motion, which is the concern that
			// (correctly) motivated latching in the first place. It just should
			// never have been applied to the census path.
			// v0.2.120: a completed probe is new ident data - anything latched from
			// before it (a bound-heuristic placement computed in the probe gap, the
			// field defect of 2026-08-24) must re-latch. The generation counter makes
			// that structural instead of hoping the paths happen to order correctly.
			static std::uint32_t s_probeGen = 0;
			if (const auto gen = ScopeIdent::ProbeCount(); gen != s_probeGen) {
				s_probeGen = gen;
				g_placeDirty.store(true);
			}
			const bool relatch = !g_place.valid || rebaselined || g_placeDirty.exchange(false);
			if (relatch || g_place.eyeIndependent) {
				ComputeAutoPlacement(a_player);
				if (relatch)
				logger::info(FMT_STRING("WIDGET AUTO-PLACE: {} via {} offset=({:.2f},{:.2f},{:.2f}) "
				                        "miss={:.2f} bound=({:.2f},{:.2f},{:.2f}) r={:.2f} [{}]"),
					g_place.valid ? "candidate" : "declined", g_place.method,
					g_place.offset[0], g_place.offset[1], g_place.offset[2], g_place.miss,
					g_place.boundCenter[0], g_place.boundCenter[1], g_place.boundCenter[2],
					g_place.boundRadius, g_place.reason);
				// Single-frame test of the assumed parent transform. Anything much
				// above zero means ScopeParent.world is NOT parent.world composed
				// with its local translate — i.e. the layout is not what this code
				// believes. The closed loop survives that; the number is here so it
				// is diagnosed rather than inferred from a misplaced disc.
				if (relatch && g_place.valid && g_place.parentResidual >= 0.0f) {
					logger::info(FMT_STRING("WIDGET AUTO-PLACE: parent-transform residual {:.3f} "
					                        "(0 = ScopeParent.world matches parent o local; large = the "
					                        "assumed +0x28 parent / matrix layout is wrong)"),
						g_place.parentResidual);
				}
				// The gap between the two independent targets is the only honest
				// estimate of the heuristic's error, and the heuristic is what every
				// modded scope falls back on. Worth a line whenever both exist.
				if (relatch && g_place.haveBoth) {
					logger::info(FMT_STRING("WIDGET AUTO-PLACE: census and bound-heuristic targets differ "
					                        "by {:.2f} units"),
						g_place.agreement);
				}
			}
			auto outcome = FitOutcome::kFallbackOffsets;
			if (*Settings::widgetAutoPlace && g_place.valid &&
				(!a_censusPlacementOnly || g_place.eyeIndependent)) {
				outcome = g_place.eyeIndependent ? FitOutcome::kPlacedCensus
				                                 : FitOutcome::kPlacedHeuristic;
				// censusPlacementOnly (the presence path, v0.2.120): the bound
				// HEURISTIC uses the eye position and is garbage at hip poses - the
				// disc-by-the-hammer field defect. Presence applies placement only
				// from the eye-independent census target; heuristic scopes keep the
				// engine baseline until the first live aim places them, as pre-.119.
				// Open loop, computed once per equip from same-frame data. See
				// ObserveAutoPlacement for why the closed loop it replaces was a
				// regression rather than an improvement.
				ox = g_place.offset[0];
				oy = g_place.offset[1];
				oz = g_place.offset[2];
				ObserveAutoPlacement(sp);  // measures only; never feeds back
			}

			// Only touch the node when something actually changed — NiAVObject::Update walks
			// the subtree, and the engine itself only does this at equip.
			if (g_widget.applied && g_widget.lastScale == scale && g_widget.lastOx == ox &&
				g_widget.lastOy == oy && g_widget.lastOz == oz) {
				return outcome;
			}

			const float nx = g_widget.baseTx + ox;
			const float ny = g_widget.baseTy + oy;
			const float nz = g_widget.baseTz + oz;
			WriteScopeParent(sp, nx, ny, nz, scale);
			g_widget.wroteTx = nx;
			g_widget.wroteTy = ny;
			g_widget.wroteTz = nz;
			g_widget.wroteScale = scale;
			g_widget.lastScale = scale;
			g_widget.lastOx = ox;
			g_widget.lastOy = oy;
			g_widget.lastOz = oz;
			g_widget.applied = true;
			g_fitAppliedAtomic.store(true, std::memory_order_relaxed);
			// v0.2.122 — the census path recomputes per frame (v0.2.100), so this
			// wrote (and logged) every ~9 ms while aiming: 2000+ identical lines
			// per minute in the 17:03 field log. The WRITE stays per-frame (that
			// is the live tracking); the LOG only speaks when something meaningful
			// changed: a rebaseline, a scale change, or the offset moving more
			// than a quarter unit since the last logged value.
			static float s_logScale = -1.0f, s_logOx = 0.0f, s_logOy = 0.0f, s_logOz = 0.0f;
			const bool logWorthy = rebaselined || scale != s_logScale ||
			                       std::fabs(ox - s_logOx) > 0.25f ||
			                       std::fabs(oy - s_logOy) > 0.25f ||
			                       std::fabs(oz - s_logOz) > 0.25f;
			if (logWorthy) {
				s_logScale = scale;
				s_logOx = ox;
				s_logOy = oy;
				s_logOz = oz;
				logger::info(FMT_STRING("WIDGET FIT applied: scale={:.4f} (aperture={:.3f} / {:.3f}) "
				                        "base=({:.2f},{:.2f},{:.2f}) offset=({:.2f},{:.2f},{:.2f})"),
					scale, aperture, kVanillaRenderCircleRadius,
					g_widget.baseTx, g_widget.baseTy, g_widget.baseTz, ox, oy, oz);
			}
			return outcome;
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

			// v0.2.65: publish the per-render engine pointers for DevBench /addresses,
			// so an ad-hoc /read can target the camera or the RT manager directly.
			g_lastRtm = rtm;
			g_lastRenderer = renderer;
			g_lastCam = cam;

			// Place the camera at the objective end of the scope tube (weapon-local
			// offset; the node is parented under PrimaryWeaponOffsetNode). SetCameraFOV
			// ends with NiAVObject::Update, which propagates this to world space.
			{
				auto* localTranslate = reinterpret_cast<float*>(cam + 0x60);  // NiAVObject::local(0x30).translate(+0x30)
				localTranslate[0] = static_cast<float>(*Settings::scopeCamOffsetX);
				localTranslate[1] = static_cast<float>(*Settings::scopeCamOffsetY);
				localTranslate[2] = static_cast<float>(*Settings::scopeCamOffsetZ);
			}

			// Identify the equipped scope (v0.2.85). Runs only when something asked
			// for it — scope-in, or a DevBench request — because it walks the weapon's
			// 3D and calls into the engine's inventory code, neither of which belongs
			// in a per-frame path. Must precede ApplyWidgetFit, which consumes the
			// aperture it resolves.
			ScopeIdent::RunIfRequested(player);

			// v0.2.90: derive the FOV from the scope's real magnification and the
			// lens geometry. ALWAYS computed so it can be compared against the
			// hand-tuned value in the log, but only USED when scopeFovDegrees is 0 —
			// a new derivation that quietly replaces a VR-confirmed calibration is
			// how you lose a known-good state without noticing.
			if (const float derived = DeriveScopeFovDegrees(player); derived > 0.0f && a_fovDeg <= 0.0f) {
				a_fovDeg = derived;
			}
			if (a_fovDeg <= 0.0f) {
				return;  // asked to derive, could not; a zero FOV renders nothing useful
			}
			g_lastFovDeg = a_fovDeg;

			// Fit the vanilla widget to the real scope's lens. Cheap: it compares against
			// the last applied values and only touches the node (and runs
			// NiAVObject::Update) when a setting actually changed, so live tuning through
			// DevBench works without paying a subtree update every frame.
			ApplyWidgetFit(player);

			// v0.2.73: mark 0 — everything from here to the light fit is "setup".
			TimersBegin();

			// Zoom FOV: force SetCameraFOV's symmetric-frustum path (instead of HMD eye
			// projections) exactly like the vanilla scope pass does, then restore.
			RENDER_STEP(1);
			auto* mode738 = reinterpret_cast<std::uint8_t*>(g_fovMode738);
			auto* mode750 = reinterpret_cast<std::int32_t*>(g_fovMode750);
			const auto saved738 = *mode738;
			const auto saved750 = *mode750;
			*mode738 = 1;
			*mode750 = 2;
			// v0.2.36 THE DEPTH-INVERSION FIX: params 3/4 are (FAR, NEAR) — NOT
			// (near, far). Static proof: SetCameraFOV stores param_4 into frustum
			// near-slot / param_3 into far-slot, and every vanilla caller passes the
			// pair (far, near) — FUN_140c875f0's param_4 is the literal 1.0f constant.
			// The engine is standard-Z everywhere (proj z-row f/(f-n) in
			// FUN_141da8e60, DS clear = 1.0, geometry depth mode 3 = LESS_EQUAL +
			// write). Passing (near, far) built a reversed projection: near→depth 1,
			// far→depth 0 — under LESS_EQUAL the FARTHEST fragment always won (the
			// locker-behind-wall bug), and the "near clip slices the gun" complaint
			// was actually the FAR plane sitting at 15 units.
			Fn<SetCameraFOV_t>(0x2804a90)(
				cam, a_fovDeg,
				static_cast<float>(*Settings::scopeFarClip),
				static_cast<float>(*Settings::scopeNearClip));
			*mode738 = saved738;
			*mode750 = saved750;

			// v0.2.124: damp the scope camera's orientation HERE — the Update
			// above just derived a fresh world pose; everything downstream
			// (culling planes, camera-state commits, AccumulateScene) consumes
			// the filtered basis coherently. See ApplyCamSmooth.
			ApplyCamSmooth(cam);

			// NOTE: this VR camera type is NOT flatrim NiCamera. SetCameraFOV's own code
			// shows: eye/frustum count @ +0x208, aspect @ +0x210, port @ +0x214..0x220
			// (which it forces to full-frame {0,1,1,0} itself — do not touch +0x184,
			// that is per-eye frustum data on this type).

			// v0.2.73 LEVER 1 — RENDER RESOLUTION. Shrink the viewport to the top-left
			// sub-rect of the fixed-size scope G-buffer. MUST come after SetCameraFOV,
			// which rewrites this port to full-frame on every call.
			//
			// rect = {left, right, top, bottom} and the engine builds
			//   X = w*left   Y = (1-top)*h   W = (right-left)*w   H = (top-bottom)*h
			// (TS_BSGraphicsState_BuildViewportFromCamDataRect, 0x141d8d480 — read, not
			// guessed). {0, s, 1, 1-s} therefore gives an s*w by s*h viewport anchored
			// at the origin, which is the anchor a future delivery-UV fix wants.
			//
			// The PROJECTION is deliberately left alone, so this squeezes the same
			// image into fewer pixels and the lens shows it shrunk into a corner. That
			// makes the picture wrong and the measurement right: it prices the whole
			// G-buffer -> lighting -> composite chain at reduced resolution before any
			// effort is spent teaching the delivery to sample a sub-rect.
			{
				auto scale = static_cast<float>(*Settings::perfRenderScale);
				if (scale < 1.0f) {
					scale = scale < 0.05f ? 0.05f : scale;  // a 0 viewport is a D3D error, not an experiment
					auto* const port = reinterpret_cast<float*>(cam + 0x214);
					port[0] = 0.0f;
					port[1] = scale;
					port[2] = 1.0f;
					port[3] = 1.0f - scale;
				}
			}

			// ============================ v0.2.71 — THE PERF FIX (§3.7e) ==========
			// Publish the frustum we just built to the slot the CULLER actually reads.
			//
			// AccumulateScene (0x27ff370) ignores the BSCullingProcess's own frustum
			// for visibility: it builds a stack BSCullingGroup and calls
			// BSCullingGroup::SetCamera (0x638270), which derives its six clip planes
			// from *(NiFrustum**)(cam + 0x200) — the COMBINED (all-eye union) frustum
			// — together with the camera's world transform at cam+0x70/+0xa0.
			//
			// SetCameraFOV writes the per-eye frusta at [cam+0x1a0] + eye*0x1c and
			// rebuilds the combined one ONLY in its `if (1 < eyeCount)` tail
			// (FUN_141c2bf80, fed by the HMD eye projections). Our scope camera is
			// mono (eyes=1), so that tail never runs and FUN_141c2bee0 refreshes just
			// the combined frustum's NEAR field. Its left/right/top/bottom/far stayed
			// at whatever the engine last left there, so every scope render culled
			// against a frustum that had nothing to do with the scope: the 2026-08-09
			// sweep measured 14,411 passes at 2.4° vs 13,672 at 120°, a 50× FOV change
			// moving the workload ~5% — i.e. no culling at all, ~21 ms a render.
			//
			// Mirroring eye 0 into the combined slot is the whole fix. Safe to mutate:
			// this camera is PrimaryWeaponScopeCamera, which only the vanilla scope
			// redirect (disarmed under Route B) and we consume, and the engine
			// rewrites both frusta from scratch on the next SetCameraFOV.
			{
				auto* const eye0 = *reinterpret_cast<float**>(cam + 0x1a0);
				auto* const comb = *reinterpret_cast<float**>(cam + 0x200);
				if (eye0) {
					std::memcpy(g_diagFrustumEye0, eye0, sizeof(g_diagFrustumEye0));
				}
				if (comb) {
					std::memcpy(g_diagFrustumCombPre, comb, sizeof(g_diagFrustumCombPre));
				}
				g_diagFrustumAliased = (eye0 && comb) ? (eye0 == comb ? 1 : 0) : -1;

				if (*Settings::cullToScopeFrustum && eye0 && comb && eye0 != comb) {
					// NiFrustum = { left, right, top, bottom, near, far, bool ortho }
					// = 6 floats + a bool = 0x1c bytes (the per-eye array stride).
					std::memcpy(comb, eye0, 0x1c);
					g_diagCullFix = 1;
				} else {
					g_diagCullFix = 0;
				}

				// One-shot evidence line: what the culler was using, and what it gets now.
				static bool loggedCullFrustum = false;
				if (!loggedCullFrustum) {
					loggedCullFrustum = true;
					logger::info(
						FMT_STRING("CULL FRUSTUM: eyes={} eye0={} comb={} aliased={} applied={} "
						           "eye0(l,r,t,b,n,f)=({:.4f},{:.4f},{:.4f},{:.4f},{:.1f},{:.1f}) "
						           "combPre(l,r,t,b,n,f)=({:.4f},{:.4f},{:.4f},{:.4f},{:.1f},{:.1f})"),
						*reinterpret_cast<const std::int32_t*>(cam + 0x208),
						static_cast<const void*>(eye0), static_cast<const void*>(comb),
						g_diagFrustumAliased, g_diagCullFix,
						g_diagFrustumEye0[0], g_diagFrustumEye0[1], g_diagFrustumEye0[2],
						g_diagFrustumEye0[3], g_diagFrustumEye0[4], g_diagFrustumEye0[5],
						g_diagFrustumCombPre[0], g_diagFrustumCombPre[1], g_diagFrustumCombPre[2],
						g_diagFrustumCombPre[3], g_diagFrustumCombPre[4], g_diagFrustumCombPre[5]);
				}
			}
			// =====================================================================

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
			// v0.2.71: also give the culling process its own frustum, exactly as the
			// engine's cull helper FUN_141d4dc50 does (set +0x18, then SetFrustum).
			// The ctor zeroes the NiFrustum at +0x20 and the six planes at +0x3c, so
			// every consumer of them — BSCullingProcess::TestBaseVisibility, which
			// hands cull+0x3c to the object's vtable+0x170 visibility test — has been
			// testing against degenerate planes. This is separate from the camera
			// mirror above: AccumulateScene's own culling uses the CAMERA's combined
			// frustum, not this. Both are needed and both hang off the one setting so
			// the §3.7e ladder stays a single-flag A/B.
			if (*Settings::cullToScopeFrustum) {
				if (const auto* const eye0 = *reinterpret_cast<const float**>(cam + 0x1a0)) {
					Fn<CullSetFrustum_t>(0x1c452b0)(cullBuf, eye0);
				}
			}
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
			TimerMark(1);  // end "setup" (camera, binds, clears)
			RENDER_STEP(7);
			using ProcessLights_t = void (*)(std::uintptr_t, void*);
			Fn<ProcessLights_t>(0x27eab40)(ssn0, cullBuf);
			TimerMark(2);  // end "lights" (ProcessQueuedLights — the 385-light fit)

			// ===================== v0.2.72 — WHY NOTHING WAS EVER CULLED =========
			// The frustum was never the problem (v0.2.71's mirror logged aliased=1:
			// [cam+0x200] and [cam+0x1a0] are the SAME buffer, and it already held the
			// correct 2.4° frustum). The planes are built correctly. They are simply
			// never TESTED.
			//
			// Inside AccumulateScene, the per-object frustum test is gated:
			//   BSCuller::ProcessVectorFrustum(culler, 0)   -- 0x1d4b9d0, the SIMD
			//   6-plane test -- runs only if
			//       culler.count != 0 &&
			//       (BSPreCulledObjects::QEnabled() == false || culler[+0x3a6f] != 0)
			// and culler[+0x3a6f] is copied from BSCullingGroup+0x17a, which that
			// group's constructor (0x6373e0) sets to 0 and nothing in this path ever
			// sets otherwise. The group is a stack local inside AccumulateScene, so we
			// cannot reach it.
			//
			// That leaves QEnabled(), read live 2026-08-09 as TRUE:
			//   [0x146878ad0]=1 (enabled) && [0x14391d830]=1 (want) && [0x146878ad1]=0 (temp-disable)
			// So the engine skips frustum culling entirely here and leans on PREVIS —
			// precomputed per-cell visibility, computed once per frame for the MAIN
			// camera in the frame prep (FUN_1427dff70). Our scope render inherits that
			// whole visible set, which is why a 27x FOV change moved the workload 1.9%
			// and a far plane of 300 units at Diamond City moved it 0.06%.
			//
			// We flip the TEMP-DISABLE byte across our accumulation only. Deliberately
			// the raw byte and NOT BSPreCulledObjects::SetTempDisabled (0x1427e0de0):
			// that setter also walks every registered visibility callback and un-hides
			// the objects previs had hidden. Writing the byte keeps those objects
			// hidden (their app-cull flags are untouched), so we KEEP previs occlusion
			// and ADD real frustum culling — which is what we want, not a trade.
			//
			// ⚠️ UNVERIFIED PREDICTION, and the honest doubt: ProcessVectorFrustum
			// writes a per-object VISIBLE mask (255 = inside all six planes), and the
			// arena reserve zeroes those bytes. If the accumulation consumed that mask,
			// skipping the test would accumulate NOTHING — yet we accumulate 14,401.
			// So either the emission ignores the mask (and enabling the test will cut
			// the pass count), or this whole path is not what produces our passes (and
			// nothing will change). The heartbeat's `passes total=` discriminates in
			// one reading, which is why this ships behind a live flag instead of as a
			// silent change.
			const auto previsTempDisable = base + 0x6878ad1;
			std::uint8_t savedPrevis = 0;
			const bool bypassPrevis = *Settings::cullToScopeFrustum;
			if (bypassPrevis) {
				savedPrevis = *reinterpret_cast<volatile std::uint8_t*>(previsTempDisable);
				*reinterpret_cast<volatile std::uint8_t*>(previsTempDisable) = 1;
			}

			RENDER_STEP(8);
			Fn<AccumScene_t>(0x27ff370)(cam, ssn0, cullBuf, 1);

			// Restore immediately: this is a process-global the main view reads too.
			// Worst case a concurrent engine cull sees it disabled for a few hundred
			// microseconds and does a real frustum test instead of skipping one —
			// more work, never wrong output.
			if (bypassPrevis) {
				*reinterpret_cast<volatile std::uint8_t*>(previsTempDisable) = savedPrevis;
			}
			TimerMark(3);  // end "accum" (AccumulateScene — CPU pass-list building)
			// =====================================================================

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

			// --- SKY group check (v0.2.40 — corrected premise) ---
			// GROUND TRUTH (static dive 2026-08-08): sky is NOT group 0xC (that is the
			// refraction group). BSSkyShaderProperty::GetRenderPasses (FUN_14288e400)
			// hard-codes the group by skyObjectType: dome/sun/stars/moons -> group
			// 0x11, clouds -> 0x12 (sun GLARE -> 0x17, skipped for now); renderMode
			// 0x19 passes its only gate. Vanilla draws 0x11/0x12/0x13 in stage
			// FUN_14284d680 right after the composite: slot0 RT 0x61 (scoped), slot1
			// aux RT 0x69, DS 0xC, depth-tested — sky fills only far pixels. The
			// world SSN accumulation DOES include these passes (baseline 11:2 12:3),
			// but our pre-resolve decal-group drop (9/0x11/0x12/0x13) deletes them —
			// correctly: they are the stale already-drawn-and-released passes that
			// faulted v0.2.11 (the resolve draws 0x11/9 internally) and v0.2.36. We
			// re-register FRESH ones post-resolve and draw them ourselves.
			RENDER_STEP(12);
			const auto skyGroup = accum + 0x18 + 0x11 * 0x678;
			g_diagSkyEmptyPre = static_cast<std::int32_t>(Fn<GroupEmpty_t>(0x281f2c0)(skyGroup));

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
			RENDER_STEP(13);
			g_diagSunPass = -1;
			// v0.2.41 STUTTER FIX (the 10:22 black-silhouette frames): the fog clear,
			// accum binds, camera state, and hook arming are INDEPENDENT of the
			// engine's sun pass-config — but they were gated on its dirty byte.
			// Frames where the engine had the config dirty (rebuild in flight;
			// correlates with heavy scenes) skipped the clear AND left the resolve's
			// accum binds in clear-on-apply mode with the CURRENT clear color = our
			// step-6 BLACK -> composite = black ambient * albedo = black world
			// silhouettes (depth fine, sky fine — drawn post-resolve). Now the accum
			// setup always runs when the hooks are available; only the sun EXEC is
			// skipped on dirty-config frames (one frame of ambient-only lighting).
			bool accumSetup = false;
			if (g_sunBindHooksInstalled && g_gfxState && *Settings::sunEnabled) {
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
						// v0.2.42 STUTTER SUSPECT: this clear IS the scope's entire
						// ambient light level (the 10:33 tone bisect proved brightness
						// tracks accumClearScale linearly and scale 0 = pitch black —
						// the sun exec contributes NOTHING yet). The old null-fallback
						// to 0 therefore painted whole frames black whenever the fog
						// singleton was transiently null (streaming/weather churn —
						// matches the black-silhouette bursts: sky/depth fine, world
						// black, content-correlated). Fall back to the LAST GOOD color
						// instead, and count nulls for the log.
						const auto cs = static_cast<float>(*Settings::accumClearScale);
						if (fog) {
							g_fogRGB[0] = *reinterpret_cast<const float*>(fog + 0x1d4);
							g_fogRGB[1] = *reinterpret_cast<const float*>(fog + 0x1d8);
							g_fogRGB[2] = *reinterpret_cast<const float*>(fog + 0x1dc);
						} else {
							++g_diagFogNulls;
						}
						Fn<SetClearColor_t>(0x1d8dc80)(renderer,
							g_fogRGB[0] * cs, g_fogRGB[1] * cs, g_fogRGB[2] * cs,
							static_cast<float>(*Settings::accumClearAlpha));
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
					Fn<StateSetViewport_t>(0x1da8bf0)(g_gfxState, cam, 1, 0.0f, 1.0f);
					WriteInverseProj(g_gfxState, cam);  // AFTER the commit (see note above)
					Fn<DepthMode_t>(0x1d8dd60)(renderer, 0);
					Fn<Flush_t>(0x1d8dc70)(renderer);
					Fn<DepthMode_t>(0x1d8de10)(renderer, 2);
					accumSetup = true;

					// Sun exec gate: only when the engine's cached config is usable at
					// this exact instant (dirty byte clear + built). Diags always read.
					const auto cfgClean = g_sunConfig && *reinterpret_cast<const std::uint8_t*>(g_sunConfig + 0x0) == 0;
					const auto cfgBuilt = g_sunConfig && *reinterpret_cast<const std::uintptr_t*>(g_sunConfig + 0x8) != 0;
					if (g_sunConfig) {
						g_diagSunCfgFlags = *reinterpret_cast<const std::uint32_t*>(g_sunConfig + 0x48);
						if (const auto lights = *reinterpret_cast<std::uintptr_t*>(g_sunConfig + 0x38)) {
							const auto cfgSun = *reinterpret_cast<const std::uintptr_t*>(lights);
							g_diagSunIsSSNSun = cfgSun == *reinterpret_cast<const std::uintptr_t*>(ssn0 + 0x248) ? 1 : 0;
						}
					}
					// v0.2.57: sunExecEnabled isolates the SUN DRAW ALONE. sunEnabled is
					// NOT a valid isolation for it — that flag gates this whole block,
					// which also owns the accum clear, the accum/DS binds, the camera
					// state and the pre-resolve G-buffer rebind. Turning it off faults
					// the delivery outright (step 17, C0000005 in the D3D layer) because
					// the ImageSpace copy then runs with no camera state. Everything
					// above stays; only the fullscreen additive BSDFLightDir exec below
					// is skipped, which is exactly the pass suspected of poisoning the
					// whole of 0x6a (100% NaN with a clean G-buffer and a clean clear).
					if (cfgClean && cfgBuilt && *Settings::sunExecEnabled) {

					// v0.2.78 — WHEN this runs is the whole defect (§3.1, proven v0.2.77).
					// A deferred directional light SHADES BY SAMPLING THE G-BUFFER. Measured
					// immediately before this exec: 0x63 albedo = 0xFF000000 (the black clear)
					// and 0x64 normals = 0x00000000 (not a valid normal), while the same two
					// buffers read as a real image after the resolve. N.L against a zero normal
					// is exactly zero everywhere -- which is 0x6a's immovable meanLum 155.0, its
					// indifference to a 10,000x sun, and (on frames where that memory holds
					// garbage rather than the clear) the ~20% NaN bursts. One mechanism, both
					// halves. Vanilla has no such problem: FUN_14284e9e0 fills the G-buffer in
					// the stages before its sun stage (call #22), then resolves (#24); OUR
					// G-buffer geometry is drawn INSIDE the resolve, which we call below.
					//
					// So the body is captured here and, when sunExecInResolve is set, invoked
					// from ResolveAccumBind0Hook -- the moment the resolve binds the light-accum
					// buffer, which is after the G-buffer geometry and before the light volumes.
					const auto runSunExec = [&]() {

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

						// v0.2.59: scan the constants this pass is about to read. A
						// fullscreen additive draw turns ALL of 0x6a to NaN only if one
						// of its uniform inputs is already non-finite — the staging
						// camera block (both eye slots, view/proj/viewproj + the
						// inverse-projection at +0x1d0) or the sun light's own floats.
						// Whatever this names is the actual root cause; the buffer-level
						// evidence cannot narrow it further.
						if (*Settings::diagLensReadback) {
							const auto ctxS = g_ctxPtrA ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrA) : 0;
							const auto ctxU = ctxS ? ctxS : (g_ctxPtrB ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrB) : 0);
							const auto staging = ctxU ? *reinterpret_cast<std::uintptr_t*>(ctxU + 0x25d0) : 0;
							const auto badStaging = staging ? FirstNonFiniteFloat(staging + 0x20, 0x420) : -1;
							const auto sunL = *reinterpret_cast<const std::uintptr_t*>(ssn0 + 0x248);
							const auto niL = sunL ? *reinterpret_cast<std::uintptr_t*>(sunL + 0xb8) : 0;
							const auto badLight = niL ? FirstNonFiniteFloat(niL + 0x16c, 12) : -1;
							const auto anyBad = badStaging >= 0 || badLight >= 0;
							if (anyBad) {
								++g_camDataBad;
							}
							if (const auto st = anyBad ? 1 : 0; st != g_camDataState) {
								g_camDataState = st;
								static std::uint32_t logs = 0;
								if (logs < 60) {
									++logs;
									if (anyBad) {
										logger::warn(
											FMT_STRING("CAMDATA non-finite BEFORE sun exec: staging+0x{:X} (rel 0x20), light+0x{:X}, count={}"),
											badStaging >= 0 ? 0x20 + badStaging : 0, badLight >= 0 ? 0x16c + badLight : 0,
											g_camDataBad);
									} else {
										logger::info(FMT_STRING("CAMDATA clean again (total bad frames={})"), g_camDataBad);
									}
								}
							}
						}

						alignas(16) std::uint8_t sunCtx[0x2d0];
						Fn<CtxCtor_t>(0x2812be0)(sunCtx, cam, accum);

						// v0.2.76: the accumulation target. TS_DrawWorld_PreWorldLightingStage
						// (0x142846d60) sets ctx+0x1c = renderer+4 ? 0x6a : 0x24 before every
						// pass it draws — six sites — and 0xffffffff where it wants none.
						// FUN_142812be0 zeroes the field, so every sun exec we have ever done
						// ran with accum target 0. Default -1 leaves it alone so the gate
						// measurement below reads the historical behaviour, not a mixed change.
						if (const auto tgt = *Settings::sunCtxAccumTarget; tgt >= 0) {
							*reinterpret_cast<std::uint32_t*>(sunCtx + 0x1c) = static_cast<std::uint32_t>(tgt);
						}
						// v0.2.60 causal bracket: 0x6a immediately before the sun draw
						// (must be the fog clear) and immediately after it.
						std::uint64_t accumPre = 0;
						const bool bracket = *Settings::diagLensReadback;
						if (bracket) {
							accumPre = SampleLogicalRT(rtm, renderer, 0x6a, &g_stage6a, g_rbFormat6a, g_rbW6a, g_rbH6a);
						}

						// ⚠️ THE GATE (Ghidra 2026-08-09, FUN_142891040). This does NOT
						// unconditionally draw. It is:
						//
						//   ok = (ctx+0x40 == cfg+0x48 && ctx+0x38 == cfg->shader)
						//        || FUN_142891280(cfg+0x48, shader, ctx);   // SetupTechnique
						//   if (ok) { ...SetupGeometry, draw, restore... }
						//   return ok;
						//
						// FUN_142891280 calls shader->vtable[0x20](shader, technique, ctx) and
						// caches (shader, technique) into ctx+0x38/+0x40 on success, clearing
						// them on failure. Our ctx is FRESH every render, so +0x38/+0x40 are 0,
						// the fast path can never match, and EVERY frame depends on
						// SetupTechnique(0x20201) succeeding. If it fails, this returns 0 and
						// nothing is drawn at all — which is exactly "the pass executes and
						// adds precisely zero" (§6.7, re-confirmed against the v0.2.75 control:
						// main accum 0x24 fully sun-lit with cast shadows, our 0x6a a perfectly
						// uniform grey, identical with the exec on and off).
						//
						// We discarded this byte for the entire sun arc, and reported
						// `sunPass=1` — meaning "we called it" — in its place. Gotcha #3: a
						// probe that cannot represent the failure will exonerate every suspect.
						// v0.2.77 THE ORDERING PROBE. Read the G-buffer this light is about to
						// shade from, right here, before the draw. Compare with the same two
						// buffers after the resolve (the end-of-render dump) -- if they are
						// empty now and populated then, our sun is shading nothing.
						if (*Settings::diagSunOrderProbe) {
							const auto g63 = SampleLogicalRT(rtm, renderer, 0x63, &g_stage63, g_rbFormat63, g_rbW63, g_rbH63);
							const auto g64 = SampleLogicalRT(rtm, renderer, 0x64, &g_stage64, g_rbFormat64, g_rbW64, g_rbH64);
							static std::uint32_t probeLogs = 0;
							if (probeLogs < 8) {
								++probeLogs;
								logger::info(
									FMT_STRING("SUN ORDER PROBE (pre-exec): 0x63 albedo={:016X} (fmt={} {}x{}) 0x64 normals={:016X} (fmt={} {}x{}) — zero/empty here means the sun is shading an unfilled G-buffer"),
									g63, g_rbFormat63, g_rbW63, g_rbH63, g64, g_rbFormat64, g_rbW64, g_rbH64);
							}
						}

						// v0.2.79 — the inverse projection, RE-APPLIED AT THE POINT OF USE.
						// staging+0x1d0 is the inverse-projection constant this pass consumes
						// for position reconstruction, and the engine's camera-state commit
						// never writes it (finding #15) — we do, in the pre-resolve block.
						// But with sunExecInResolve the exec now runs AFTER the resolve has
						// committed its own camera state, so the staging block in effect here
						// need not be the one we wrote. A stale or zeroed inverse projection
						// is FINITE, so camDataBad/invProjRejects both read clean (they did:
						// 0 and 0) while every reconstructed position divides by zero — which
						// is exactly the measurement: inputs healthy, 0x6a clean before the
						// draw and 100% NaN after it, every frame (sunPreNaN=0 sunPostNaN=347).
						// Cheap, validated, and skips itself on a singular source.
						if (*Settings::sunReapplyInvProj) {
							static std::uint32_t invLogs = 0;
							if (invLogs < 6) {
								++invLogs;
								const auto ctxI = g_ctxPtrA ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrA) : 0;
								const auto ctxJ = ctxI ? ctxI : (g_ctxPtrB ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrB) : 0);
								if (const auto stg = ctxJ ? *reinterpret_cast<std::uintptr_t*>(ctxJ + 0x25d0) : 0) {
									const auto* m = reinterpret_cast<const float*>(stg + 0x1d0);
									logger::info(
										FMT_STRING("INVPROJ at exec (before re-apply): staging={:016X} row0=[{} {} {} {}] row3=[{} {} {} {}]"),
										stg, m[0], m[1], m[2], m[3], m[12], m[13], m[14], m[15]);
								}
							}
							WriteInverseProj(g_gfxState, cam);
						}

						ProbeBoundResources(*Settings::sunExecInResolve ? "in-resolve (new)"sv : "pre-resolve (old, control)"sv);

						// v0.2.82 — THE FIX. Measured at the deferred call site:
						//   pre-resolve (old) : SRV [0,1,5,6,7], RTs bound = 1
						//   in-resolve  (new) : SRV [0,1,2,3,5,8], RTs bound = 4
						// FOUR render targets = the G-BUFFER MRT, still bound. The RT manager
						// STAGES binds and commits them later, and our hook sits on the slot-0
						// staging call -- before slot 1 is staged and before the commit. So the
						// sun was drawing into the G-buffer, corrupting albedo/normals; the
						// resolve's light volumes then read NaN normals and every downstream
						// buffer inherited it. That is exactly why the damage landed on
						// precisely the geometry pixels and left the sky clean.
						//
						// Bind the accumulation MRT and COMMIT it ourselves, the same way the
						// pre-resolve path does, so the draw lands where it is supposed to.
						if (*Settings::sunResolveRebindAccum && *Settings::sunExecInResolve) {
							Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x6a, 3);
							Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, *Settings::sunSpecEnabled ? 0x6b : -1, 3);
							Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
							ProbeBoundResources("in-resolve AFTER rebind"sv);
						}
						const auto sunDrew = Fn<ExecPassConfig_t>(0x2891040)(g_sunConfig, 0, sunCtx);
						Fn<FlushBatch_t>(0x2891300)(sunCtx);
						g_diagSunDrew = sunDrew ? 1 : 0;
						if (sunDrew) {
							++g_sunDrewCount;
						} else {
							++g_sunGatedCount;
						}
						// State transitions only — this runs ~90×/s.
						if (static std::int32_t lastDrew = -1; lastDrew != g_diagSunDrew) {
							lastDrew = g_diagSunDrew;
							logger::info(
								FMT_STRING("SUN EXEC {} — FUN_142891040 returned {} (technique 0x{:X}, ctxAccumTarget {}). drew={} gated={}"),
								sunDrew ? "DREW" : "GATED OFF (SetupTechnique refused; nothing was rasterised)",
								static_cast<int>(sunDrew), g_diagSunCfgFlags, *Settings::sunCtxAccumTarget,
								g_sunDrewCount, g_sunGatedCount);
						}

						if (bracket) {
							const auto accumPost =
								SampleLogicalRT(rtm, renderer, 0x6a, &g_stage6a, g_rbFormat6a, g_rbW6a, g_rbH6a);
							const auto preBad = PixelNonFinite(g_rbFormat6a, accumPre);
							const auto postBad = PixelNonFinite(g_rbFormat6a, accumPost);
							if (preBad) {
								++g_sunPreNaN;
							}
							if (postBad) {
								++g_sunPostNaN;
							}
							if (const auto st = (preBad ? 2 : 0) + (postBad ? 1 : 0); st != g_sunNaNState) {
								g_sunNaNState = st;
								static std::uint32_t logs = 0;
								if (logs < 60) {
									++logs;
									logger::warn(
										FMT_STRING("SUNBRACKET 0x6a pre={:016X}{} post={:016X}{} — {} (pre={} post={})"),
										accumPre, preBad ? " NaN"sv : ""sv, accumPost, postBad ? " NaN"sv : ""sv,
										!preBad && postBad ? "THE SUN DRAW CREATES IT"sv :
											preBad         ? "already NaN before the sun draw"sv :
															 "clean"sv,
										g_sunPreNaN, g_sunPostNaN);
								}
								// v0.2.61: FINITE IS NOT NON-DEGENERATE. camdata=0 only
								// proved no input was Inf/NaN — but normalize() of a ZERO
								// vector is 0/0 = NaN, and a zero matrix passes isfinite on
								// every element. BSDFLightShader::SetupGeometry builds the
								// Dir-light constants from the staging EYE-1 view matrix
								// (+0x260, the second 0x210-stride block), which nothing
								// populates for our mono camera except our own memcpy
								// mirror. A near-zero eye-1 view matrix would produce
								// exactly what we measure: finite inputs, a NaN constant,
								// and a 100%-NaN buffer from one fullscreen additive draw.
								static std::uint32_t matLogs = 0;
								if (!preBad && postBad && matLogs < 12) {
									++matLogs;
									const auto ctxS = g_ctxPtrA ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrA) : 0;
									const auto ctxU = ctxS ? ctxS : (g_ctxPtrB ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrB) : 0);
									if (const auto stg = ctxU ? *reinterpret_cast<std::uintptr_t*>(ctxU + 0x25d0) : 0) {
										const auto* e0 = reinterpret_cast<const float*>(stg + 0x50);
										const auto* e1 = reinterpret_cast<const float*>(stg + 0x260);
										const auto mag = [](const float* m, int row) {
											return std::sqrt(m[row * 4 + 0] * m[row * 4 + 0] +
												m[row * 4 + 1] * m[row * 4 + 1] + m[row * 4 + 2] * m[row * 4 + 2]);
										};
										logger::warn(
											FMT_STRING("  eye0 view rowmag=({:.4f},{:.4f},{:.4f}) eye1 view rowmag=({:.4f},{:.4f},{:.4f})"),
											mag(e0, 0), mag(e0, 1), mag(e0, 2), mag(e1, 0), mag(e1, 1), mag(e1, 2));
										logger::warn(
											FMT_STRING("  eye1 view = [{} {} {} {}][{} {} {} {}][{} {} {} {}][{} {} {} {}]"),
											e1[0], e1[1], e1[2], e1[3], e1[4], e1[5], e1[6], e1[7],
											e1[8], e1[9], e1[10], e1[11], e1[12], e1[13], e1[14], e1[15]);
										const auto* p1 = reinterpret_cast<const float*>(stg + 0x2a0);
										logger::warn(
											FMT_STRING("  eye1 proj diag=({},{},{},{}) eyePos0=({},{},{}) eyePos1=({},{},{})"),
											p1[0], p1[5], p1[10], p1[15],
											*reinterpret_cast<const float*>(ctxU + 0x2590),
											*reinterpret_cast<const float*>(ctxU + 0x2594),
											*reinterpret_cast<const float*>(ctxU + 0x2598),
											*reinterpret_cast<const float*>(ctxU + 0x25a0),
											*reinterpret_cast<const float*>(ctxU + 0x25a4),
											*reinterpret_cast<const float*>(ctxU + 0x25a8));
									}
								}
							}
						}

						if (scaling) {
							std::memcpy(reinterpret_cast<float*>(niLight + 0x16c), savedRGB, sizeof(savedRGB));
						}

						set(0xb0, 0, 4);  // job's own restore
						g_diagSunPass = 1;
					}
					};  // end runSunExec

					if (*Settings::sunExecInResolve) {
						g_pendingSunExec = runSunExec;  // fired from inside the resolve
					} else {
						runSunExec();                   // pre-v0.2.78 placement, kept for the A/B
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
			TimerMark(4);  // end "sun" (accum clears + binds + the BSDFLightDir exec)
			RENDER_STEP(14);
			// Ensure the staging inverse-projection is OURS for the resolve's own
			// lighting/composite too. The resolve re-commits the camera internally,
			// but the commit never touches staging+0x1d0, so this write survives it.
			if (g_gfxState) {
				Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 1);
				Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 0);
				WriteInverseProj(g_gfxState, cam);
			}

			// CRITICAL (v0.2.35): the resolve builds its render context at entry and
			// captures the CURRENT slot-0 RT into ctx+0x54 — the source of the
			// screen-size/UV constants for every pass it draws (lights, composite).
			// Vanilla calls the resolve with the G-BUFFER bound (the world render
			// precedes it) — and so did our v0.2.25 flow, whose composite worked.
			// The v0.2.26+ sun block left the accum MRT (0x6a) bound at resolve
			// entry, poisoning those constants: the composite sampled out of
			// footprint (flat output = the accum clear color) and screen-space
			// terms (spec) got garbage UV scaling. Rebind the G-buffer (no clear)
			// before the call to restore the vanilla invariant.
			if (accumSetup) {
				Fn<SelectDS_t>(0x1db9e40)(rtm, 0xc, 3, 0);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x63, 3);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, 0x64, 3);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 2, 0x66, 3);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 3, 0x67, 3);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 4, 0x68, 3);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 5, 0x69, 3);
				Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
			}
			// v0.2.73 LEVER 2 — LIGHT COUNT. Clamp the two loop counts the resolve
			// iterates, for the duration of the resolve only. The resolve draws a light
			// VOLUME per entry (cone/sphere geometry); through a 2.4 deg frustum any
			// light that intersects at all projects across the entire target, so 385
			// shadowed lights is 385 potentially full-screen shaded passes. Restored
			// immediately after, before the heartbeat re-reads the true counts — so a
			// clamped run reports its real light count AND its clamp, and cannot be
			// mistaken later for an ordinary reading.
			const auto   lightsMax = static_cast<std::int32_t>(*Settings::perfLightsMax);
			const bool   clampLights = lightsMax >= 0;
			std::int16_t savedLightsA = 0, savedLightsB = 0;
			auto* const  lightCountA = reinterpret_cast<std::int16_t*>(ssn0 + 0x1a8);
			auto* const  lightCountB = reinterpret_cast<std::int16_t*>(ssn0 + 0x1c0);
			if (clampLights) {
				savedLightsA = *lightCountA;
				savedLightsB = *lightCountB;
				const auto cap = static_cast<std::int16_t>(lightsMax);
				if (*lightCountA > cap) {
					*lightCountA = cap;
				}
				if (*lightCountB > cap) {
					*lightCountB = cap;
				}
			}
			g_diagLightsClamp = clampLights ? lightsMax : -1;

			g_inOwnResolve.store(accumSetup);
			Fn<DeferredResolve_t>(0x27ff8b0)(cam, g_accum, cullBuf, ssn0, 0x61, 0xc, 0, 1);
			g_inOwnResolve.store(false);
			g_pendingSunExec = nullptr;  // never let a stale closure outlive this frame

			if (clampLights) {
				*lightCountA = savedLightsA;
				*lightCountB = savedLightsB;
			}
			TimerMark(5);  // end "resolve" (G-buffer draw + light volumes + composite)

			// v0.2.43 black-burst forensics: sample the center pixel of the light
			// accum (0x6a) and the composite (0x61) right after the resolve. On a
			// burst frame (world black, sky fine) exactly one story is possible:
			// 0x6a dark = the lighting/clear side died; 0x6a lit but 0x61 dark = the
			// composite consumption died. Aim at a WALL while hunting (the center
			// pixel must be geometry, not sky).
			// v0.2.47 three-point readback: this (post-resolve) + post-sky + post-
			// delivery. Crescent-LED result (v0.2.46): bursts happen WITH fills
			// running (blue crescent, black world) while post-resolve 0x61 is never
			// dark (v0.2.44) — so the blackness enters in the sky draw or the
			// tonemap delivery. Whichever sample goes dark first names the stage.
			std::uint64_t rbPostResolve = ~0ull;
			if (*Settings::diagLensReadback) {
				rbPostResolve = SampleLogicalRT(rtm, renderer, 0x61, &g_stage61, g_rbFormat61, g_rbW61, g_rbH61);
				const auto p6a = SampleLogicalRT(rtm, renderer, 0x6a, &g_stage6a, g_rbFormat6a, g_rbW6a, g_rbH6a);
				++g_rbSamples;
				// v0.2.55: NaN/Inf in the composite — scored "lit" by PixelDark (its
				// exponent is 0x1f, far above the <12 dark cutoff) but displayed BLACK.
				// Edge-logged with the raw pixel and the camera numbers most likely to
				// have produced it, so one black event names its own cause.
				if (const auto nan61 = PixelNonFinite(g_rbFormat61, rbPostResolve) ? 1 : 0;
					nan61 != g_rbNaNState) {
					g_rbNaNState = nan61;
					static std::uint32_t nanLogs = 0;
					if (nanLogs < 60) {
						++nanLogs;
						const auto frustum = *reinterpret_cast<const float**>(cam + 0x1a0);
						logger::warn(
							FMT_STRING("NaN/Inf in 0x61 -> {} (px={:016X}) camRect=({},{},{},{},{},{}) frustum(l,r,t,b,n,f)=({},{},{},{},{},{})"),
							nan61 != 0 ? "PRESENT"sv : "gone"sv, rbPostResolve,
							g_diagRect[0], g_diagRect[1], g_diagRect[2], g_diagRect[3], g_diagRect[4], g_diagRect[5],
							frustum ? frustum[0] : 0.0f, frustum ? frustum[1] : 0.0f, frustum ? frustum[2] : 0.0f,
							frustum ? frustum[3] : 0.0f, frustum ? frustum[4] : 0.0f, frustum ? frustum[5] : 0.0f);
					}
				}
				if (PixelNonFinite(g_rbFormat61, rbPostResolve)) {
					++g_rbNaN61;
				}
				if (PixelDark(g_rbFormat61, rbPostResolve)) {
					++g_rbDark61;
				}
				if (PixelDark(g_rbFormat6a, p6a)) {
					++g_rbDark6a;
				}
				static std::uint32_t baselineLogs = 0;
				if (baselineLogs < 3) {
					++baselineLogs;
					logger::info(
						FMT_STRING("READBACK baseline post-resolve: 0x61={:016X} 0x6a={:016X} (fmt61={} fmt6a={} {}x{})"),
						rbPostResolve, p6a, g_rbFormat61, g_rbFormat6a, g_rbW61, g_rbH61);
				}
			}

			// --- SKY accumulate + draw (post-resolve; groups corrected in v0.2.40) ---
			// Vanilla order: composite into 0x61, THEN draw sky groups 0x11/0x12/0x13
			// into 0x61 (slot1 = 0x69, DS 0xC, no clears) — sky depth-tests against
			// the world and fills only far pixels (replacing our black pre-clear).
			// Accumulating the roots here (after the resolve) means the fresh passes
			// are drawn only by US and then released by FinishAccum; pre-resolve
			// registration was the v0.2.36 step-14 fault (the resolve draws 0x11/9
			// internally). Must run BEFORE FinishAccum (0x281e750 clears every
			// group's pass lists). Ctx is built AFTER the binds (v0.2.35 lesson: it
			// snapshots the current slot-0 RT for screen-size constants).
			// skyRootMask bisects a faulting root without a rebuild: 1 = sky dome only
			// (Sky+0x8), 2 = sun/cloud only, 3 = both.
			RENDER_STEP(15);
			g_diagSkyDrawn = 0;
			g_diagSkyRoots = 0;
			g_diagSkyEmptyPost = g_diagSkyEmptyPre;
			if (*Settings::skyEnabled && g_diagSkyEmptyPre != 0) {
				CapturePassCountsInto(accum, g_skyBaseCounts, g_skyBaseTotal);
				const auto base = REL::Module::get().base();
				const auto mask = static_cast<std::uint32_t>(*Settings::skyRootMask);
				constexpr struct { std::uintptr_t rva; std::uint32_t bit; } kSkyRoots[] = {
					{ 0x6885c00, 1 },  // Sky+0x8 (BSMultiBoundNode, the dome)
					{ 0x6885c08, 2 },  // sun/cloud geometry sibling
				};
				for (const auto& r : kSkyRoots) {
					if (!(mask & r.bit)) {
						continue;
					}
					const auto root = *reinterpret_cast<const std::uintptr_t*>(base + r.rva);
					if (!root || (root & 7) != 0) {
						continue;
					}
					const auto vtbl = *reinterpret_cast<const std::uintptr_t*>(root);
					if (vtbl < base || vtbl >= base + 0x0a000000) {
						continue;  // not an engine vtable -> mislabeled global, skip
					}
					// THE v0.2.37 zero-passes fix: the sky roots are DISABLED outside
					// the engine's own sky stage — vanilla brackets its sky accumulation
					// with vfunc+0x180(root, 1) ... (root, 0) on each root
					// (FUN_140c875f0's forward pre-pass does exactly this on the two
					// objects at ctx+0x38/+0x40 before AccumulateScene). Without the
					// toggle, culling rejects the whole subtree and nothing registers
					// (v0.2.37 log: sky=1/2/1/0). Same vfunc, same bracket.
					const auto toggleFn = *reinterpret_cast<const std::uintptr_t*>(vtbl + 0x180);
					if (toggleFn < base || toggleFn >= base + 0x0a000000) {
						continue;
					}
					// v0.2.39: ALSO bypass culling for the sky accumulation. The v0.2.38
					// toggle got the roots' screen-space glare quads registering
					// (skyNew=[11:+2 12:+3]) but the DOME geometry still never reached
					// group 0xC — its huge multibound fails the default frustum/portal
					// culling. The engine's own forward passes bracket accumulation
					// with cull+0x158 = 1 (accumulate-all mode, byte-confirmed in the
					// first-person pass FUN_14284e370); mirror that here.
					const auto savedCullMode = *reinterpret_cast<std::uint8_t*>(cullBuf + 0x158);
					*reinterpret_cast<std::uint8_t*>(cullBuf + 0x158) = 1;
					using Toggle_t = void (*)(std::uintptr_t, std::uint32_t);
					reinterpret_cast<Toggle_t>(toggleFn)(root, 1);
					Fn<AccumScene_t>(0x27ff370)(cam, root, cullBuf, 0);
					reinterpret_cast<Toggle_t>(toggleFn)(root, 0);
					*reinterpret_cast<std::uint8_t*>(cullBuf + 0x158) = savedCullMode;
					++g_diagSkyRoots;
				}
				CapturePassCountsInto(accum, g_skyAfterCounts, g_skyAfterTotal);
				g_diagSkyEmptyPost = static_cast<std::int32_t>(Fn<GroupEmpty_t>(0x281f2c0)(skyGroup));
			}
			if (*Settings::skyEnabled && g_diagSkyEmptyPost == 0) {
				// Vanilla sky-stage binds (FUN_14284d680, scoped values): slot 0 stays
				// 0x61 (the composited scene), slot 1 = aux RT 0x69, DS 0xC, no clears.
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x61, 3);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, 0x69, 3);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 2, -1, 3);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 3, -1, 3);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 4, -1, 3);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 5, -1, 3);
				Fn<SelectDS_t>(0x1db9e40)(rtm, 0xc, 3, 0);
				Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
				if (g_gfxState) {
					Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 1);
					Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 0);
					Fn<StateSetViewport_t>(0x1da8bf0)(g_gfxState, cam, 1, 0.0f, 1.0f);
					WriteInverseProj(g_gfxState, cam);
				}
				alignas(16) std::uint8_t skyCtx[0x2e0];
				Fn<CtxCtor_t>(0x2812be0)(skyCtx, cam, accum);
				// Draw order + exec flags = vanilla's: 0x11 (dome/sun/stars/moons,
				// flag 1), 0x12 (clouds, flag 0), 0x13 (flag 0). Vanilla's queued 0x11
				// uses a SORTED pass builder (FUN_14281df50); e400's default order is
				// the known gap if sky objects layer wrongly (sun behind dome etc.).
				Fn<DrawGroupNow_t>(0x281e400)(g_accum, 0x11, skyCtx, 1);
				Fn<DrawGroupNow_t>(0x281e400)(g_accum, 0x12, skyCtx, 0);
				Fn<DrawGroupNow_t>(0x281e400)(g_accum, 0x13, skyCtx, 0);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, -1, 3);
				Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
				g_diagSkyDrawn = 1;
			}

			std::uint64_t rbPostSky = ~0ull;
			if (*Settings::diagLensReadback) {
				rbPostSky = SampleLogicalRT(rtm, renderer, 0x61, &g_stage61, g_rbFormat61, g_rbW61, g_rbH61);
				if (PixelDark(g_rbFormat61, rbPostSky)) {
					++g_rbDark61Sky;
				}
			}

			// Unbind (FUN_140b03d60's post-resolve pattern), then finish our accumulator.
			TimerMark(6);  // end "sky" (sky accumulation + immediate group draw)
			RENDER_STEP(16);
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
			// v0.2.75 SUN INPUTS. §6.7 measured that the sun exec adds EXACTLY ZERO on
			// normal frames; §7.3 measured that on ~20% of frames it turns 100% of 0x6a
			// to NaN. Those two together say the pass DOES rasterize every pixel (so
			// §6.7's own "stencil rejects everything" suspect is dead) and computes a
			// contribution of zero. One input explains both readings: a light DIRECTION
			// that is zero most frames — dot(N, 0) = 0, no light — and garbage on the
			// rest — normalize(garbage) or a 0/0 → NaN. That is the signature of an
			// UNINITIALISED per-frame value, which is exactly what we would expect to
			// inherit by skipping the engine's pre-world sun stage and exec'ing its
			// cached pass config directly.
			//
			// So log what we actually hand the pass. The NiLight's world rotation rows
			// live at +0x70/+0x80/+0x90 (3 floats each, stride 0x10) — read off
			// TS_BSLight_UpdateVisibilityAndFade's own spot-direction math — and a
			// directional light's direction is a column of that basis. If these are
			// zeros or garbage, the hypothesis is confirmed without a debugger.
			if (const auto sunLight = *reinterpret_cast<const std::uintptr_t*>(ssn0 + 0x248)) {
				if (const auto niSun = *reinterpret_cast<const std::uintptr_t*>(sunLight + 0xb8)) {
					for (int row = 0; row < 3; ++row) {
						std::memcpy(&g_diagSunBasis[row * 3],
							reinterpret_cast<const void*>(niSun + 0x70 + static_cast<std::uintptr_t>(row) * 0x10), 12);
					}
					std::memcpy(g_diagSunPos, reinterpret_cast<const void*>(niSun + 0xa0), 12);
					std::memcpy(g_diagSunRGB, reinterpret_cast<const void*>(niSun + 0x16c), 12);
					g_diagSunScale = *reinterpret_cast<const float*>(niSun + 0x184);
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
					// v0.2.53: the last two floats of that block are the D3D11_VIEWPORT
					// MinDepth/MaxDepth. D3D11 REQUIRES 0 <= Min <= Max <= 1 and drops
					// the whole RSSetViewports call otherwise (the previous viewport
					// then silently stays in effect). The heartbeat has been printing
					// values like -0.35, 0.41 and 2.75 there, varying per frame with
					// view direction — flag it explicitly so we stop having to decode
					// float bits out of the diag ints by hand.
					{
						const auto minD = *reinterpret_cast<const float*>(&g_diagViewport[4]);
						const auto maxD = *reinterpret_cast<const float*>(&g_diagViewport[5]);
						if (!(minD >= 0.0f && minD <= maxD && maxD <= 1.0f)) {
							static std::uint32_t badLogs = 0;
							if (badLogs < 20) {
								++badLogs;
								logger::warn(
									FMT_STRING("VIEWPORT depth range INVALID: min={} max={} (D3D11 needs 0<=min<=max<=1; RSSetViewports is dropped otherwise)"),
									minD, maxD);
							}
						}
					}
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
			RENDER_STEP(17);
			// v0.2.49 DELIVERY CAMERA GUARD (black/blue-burst suspect #6, the best
			// yet): ImageSpace effects select their render camera via the manager's
			// +0x60 byte (FUN_1427b01a0: 0 -> mgr+0x28 [or +0x38 in VR], 1 -> the
			// TRANSIENT slot mgr+0x58 that engine stages own around their own
			// effect renders, e.g. FUN_140c875f0's tail). If a concurrent stage
			// holds the byte when our delivery runs, the tonemap quad renders with
			// a foreign/stale camera -> wrong viewport -> silently draws nothing
			// (lens keeps whatever was painted before = blue with the diag tint)
			// or garbage over the footprint (black). Force the normal selector for
			// the duration of our delivery; restore after. (If a racing stage's
			// own effect glitches for a frame in exchange, we'll see it in the
			// MAIN view at former-burst moments and refine.)
			const auto ismMgr = *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + kIsmInstanceRVA);
			std::uint8_t savedIsmBusy = 0;
			if (ismMgr) {
				savedIsmBusy = *reinterpret_cast<std::uint8_t*>(ismMgr + 0x60);
				*reinterpret_cast<std::uint8_t*>(ismMgr + 0x60) = 0;
			}
			// v0.2.67 CRESCENT (§3.2) — unbind the depth-stencil across the delivery.
			// Step 16 above restores DS logical 1, and the DS translation table at
			// rtm+0x15fc maps 1 -> physical 2: the MAIN VR EYE's depth-stencil. The
			// delivery quad has been drawing with it bound, stencil-masked to the
			// headset's hidden-area mesh — the ~20% four-corner cutout.
			//
			// Logical DS 0xA maps to physical -1 in that table = no depth-stencil.
			// Use 0xA, NOT -1: SelectDS (0x1db9e40) indexes rtm+0x15fc+idx*4
			// unconditionally, with none of the `param_3 == -1` guard that
			// SetCurrentRenderTarget (0x1db9dd0) has, so -1 would read rtm+0x15f8
			// and bind whatever physical index happens to live there.
			constexpr std::int32_t kDS_None = 0xA;
			const bool             unbindDS = *Settings::deliveryUnbindDS;
			{
				// Forensics for the first few renders: what the delivery would have
				// inherited. Cheap, self-limiting, and it makes the run informative
				// whichever way the A/B lands.
				static std::uint32_t dsLogs = 0;
				if (dsLogs < 5 && (g_ctxPtrA || g_ctxPtrB)) {
					auto dsCtx = g_ctxPtrA ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrA) : 0;
					if (!dsCtx && g_ctxPtrB) {
						dsCtx = *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrB);
					}
					if (dsCtx) {
						++dsLogs;
						logger::info(
							FMT_STRING("DELIVERY DS: logical={} physical={} mode={} slice={} depthStateOverride={} unbind={}"),
							*reinterpret_cast<const std::int32_t*>(dsCtx + 0x1f68),
							*reinterpret_cast<const std::int32_t*>(dsCtx + 0x1f2c),
							*reinterpret_cast<const std::int32_t*>(dsCtx + 0x1f5c),
							*reinterpret_cast<const std::int32_t*>(dsCtx + 0x1f30),
							*reinterpret_cast<const std::uint32_t*>(dsCtx + 0x1ee0 + 0xb0),
							unbindDS);
					}
				}
			}
			if (unbindDS) {
				Fn<SelectDS_t>(0x1db9e40)(rtm, kDS_None, 3, 0);
				Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
			}
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
			case 8:
				// v0.2.53 — RENDER WITHOUT DELIVERY. Everything above runs (the whole
				// second world render and every piece of engine state it touches), but
				// nothing is written into the lens RT, so the lens keeps whatever the
				// fill hook pre-painted (use with diagPauseTint = solid blue).
				//
				// Splits the last two variables apart. Field ladder so far: no render +
				// clear (mode 0) = never black; no render + raw copy (mode 1) = never
				// black; render + tonemap (mode 2) = black; render + raw copy (mode 7) =
				// black. Delivery path is therefore irrelevant and the render is the
				// trigger — while every readback (0x61 post-resolve, 0x61 post-sky, 0x62
				// post-delivery, 0x62 at next-frame entry) reads LIT through the blacks.
				//   blue goes black here => the render's SIDE EFFECTS on engine state
				//       blacken the lens surface; no texture we write is involved, and
				//       the hunt moves to what our bracket leaves behind (renderer
				//       flags, RT/DS binds, camera + viewport state, shader property
				//       cache) at the time the lens quad draws.
				//   blue survives here   => the black IS written into the lens RT by the
				//       delivery, and the readback is not reading what the lens samples
				//       (aux-vs-live texture, or a different logical target) —
				//       an x64dbg session on the slot-6 bind settles which.
				break;
			default:
				Fn<VanillaLensCopy_t>(0x27b08c0)(0x61, Addr::kRT_ScopeLens, 0);
				// v0.2.104 — OWN DELIVERY PASS. Composite the engine's reticle + the
				// glass look over the tonemapped picture (LensComposite.cpp). Runs
				// with the delivery's DS unbound and the ISM busy byte forced, same
				// as the tonemap it follows.
				if (*Settings::lensCompositeEnabled) {
					LensComposite::Inputs in{};
					in.renderer = renderer;
					in.rtm = rtm;
					in.ctx = g_ctxPtrA ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrA) : 0;
					if (!in.ctx && g_ctxPtrB) {
						in.ctx = *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrB);
					}
					in.scopeParent = *reinterpret_cast<std::uintptr_t*>(player + kScopeParentInPlayer);
					in.lensLogicalRT = static_cast<std::uint32_t>(Addr::kRT_ScopeLens);
					LensComposite::Run(in);
				} else {
					LensComposite::RestoreReticleQuad();
				}
				break;
			}
			if (unbindDS) {
				// Restore exactly what step 16 left bound, so nothing downstream sees
				// a DS we removed.
				Fn<SelectDS_t>(0x1db9e40)(rtm, 1, 3, 0);
				Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
			}
			if (ismMgr) {
				*reinterpret_cast<std::uint8_t*>(ismMgr + 0x60) = savedIsmBusy;
			}
			// Mark 7 — end "deliver" (step 16's unbind/FinishAccum + the tonemap copy
			// into the lens). Taken BEFORE the diagnostic dumps and readbacks below,
			// which are off in any perf run and would otherwise be attributed to it.
			TimerMark(7);
			TimersEnd();

			// v0.2.54: full-surface dump of the composite and the delivered lens,
			// taken at the same point the 1-pixel readback is taken (right after
			// delivery) so the two diagnostics describe the same instant.
			{
				static std::uint64_t dumpSeq = 0;
				++dumpSeq;
				// v0.2.65: an explicit request (DevBench /dump/now) fires on the very
				// next render regardless of cadence. The modulo path alone silently
				// produced nothing when the probe window was shorter than the period —
				// with the render rate ~9/s, `every=200` needs 20+ seconds to be sure.
				const bool onDemand = g_dumpRequest.exchange(false);
				const auto every = *Settings::diagDumpLensEveryNRenders;
				if (onDemand || (every > 0 && (dumpSeq % static_cast<std::uint64_t>(every)) == 0)) {
					// v0.2.55 established the composite is NaN on exactly the pixels
					// covered by world geometry, while the (separately drawn, forward)
					// sky stays clean — so the corruption is inside the deferred chain.
					// These four intermediates split it by stage on a single frame:
					//   0x63 albedo / 0x64 normals   = G-buffer (geometry pass output)
					//   0x6a diffuse / 0x6b specular = light accumulation
					// The first buffer showing magenta is where NaN is born; every
					// stage upstream of it is exonerated.
					if (*Settings::diagDumpBuffers) {
						DumpLogicalRT(rtm, renderer, 0x63, "63_gbuf_albedo"sv, dumpSeq);
						DumpLogicalRT(rtm, renderer, 0x64, "64_gbuf_normals"sv, dumpSeq);
						DumpLogicalRT(rtm, renderer, 0x6a, "6a_accum_diffuse"sv, dumpSeq);
						DumpLogicalRT(rtm, renderer, 0x6b, "6b_accum_specular"sv, dumpSeq);
						// v0.2.75 — THE CONTROL. The MAIN VIEW's light accumulation, the
						// same buffers ours are: renderer+4 is precisely the flag that
						// remaps 0x24/0x25 -> 0x6a/0x6b for a scoped pass, so these two
						// are the engine's own output of the very pass we are failing to
						// reproduce, in the same units, on the same frame.
						//
						// Gotcha #3 says to always print a known-good control beside the
						// unknown; this lighting hunt has never had one. It splits the
						// question in a single dump:
						//   0x24 shows real directional light, ours is flat  => our INPUTS
						//     to the sun pass are wrong; hunt them, the model is right.
						//   0x24 is ALSO flat                               => the sun does
						//     NOT reach the main view through the accum either, our whole
						//     model of where FO4VR's sunlight comes from is wrong, and the
						//     composite's own 0x40 sun term is where to look instead.
						// The second outcome would invalidate the entire v0.2.26-62 sun
						// arc, which is exactly why it is worth one dump to rule out.
						//
						// Caveat to keep with the data: our render runs at the fill hook,
						// so depending on where that sits in the frame these may hold the
						// PREVIOUS frame's main-view accumulation. Adjacent frame, same
						// scene - fine as a control, wrong as a per-frame correlation.
						DumpLogicalRT(rtm, renderer, 0x24, "24_MAIN_accum_diffuse"sv, dumpSeq);
						DumpLogicalRT(rtm, renderer, 0x25, "25_MAIN_accum_specular"sv, dumpSeq);
					}
					DumpLogicalRT(rtm, renderer, 0x61, "61_composite"sv, dumpSeq);
					DumpLogicalRT(rtm, renderer, static_cast<std::uint32_t>(Addr::kRT_ScopeLens), "62_lens"sv, dumpSeq);
					// Publish AFTER the files are written, so a client that polls
					// DumpEventCount() and then reads the directory cannot race a
					// half-written BMP.
					g_lastDumpIndex.store(dumpSeq);
					g_dumpEvents.fetch_add(1);
				}
			}

			if (*Settings::diagLensReadback) {
				const auto rbLens = SampleLogicalRT(rtm, renderer, static_cast<std::uint32_t>(Addr::kRT_ScopeLens),
					&g_stage62, g_rbFormat62, g_rbW62, g_rbH62);
				const bool darkLens = PixelDark(g_rbFormat62, rbLens);
				if (PixelNonFinite(g_rbFormat62, rbLens)) {
					++g_rbNaN62;
				}
				if (darkLens) {
					++g_rbDark62;
				}
				if (darkLens || PixelDark(g_rbFormat61, rbPostSky) || PixelDark(g_rbFormat61, rbPostResolve)) {
					static std::uint32_t darkLogs = 0;
					if (darkLogs < 30) {
						++darkLogs;
						logger::warn(
							FMT_STRING("READBACK DARK: postResolve61={:016X} postSky61={:016X} lens62={:016X} (fmt62={} {}x{})"),
							rbPostResolve, rbPostSky, rbLens, g_rbFormat62, g_rbW62, g_rbH62);
					}
				}
			}

			RENDER_STEP(18);
			Fn<CullDtor_t>(0x1d4d960)(cullBuf);
			RENDER_STEP(19);
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
		// v0.2.48: publish our thread id for the +4-reader hook — concurrent engine
		// threads that call the reader during our bracket must NOT see scoped mode
		// (phantom late-frame scoped actions on the lens RT = black-burst suspect).
		g_renderTid.store(::GetCurrentThreadId());
		*scopePassFlag = 1;
		*stereoMaster = 0;
		Fn<RendererFn_t>(0x1d94c10)(renderer);  // rebind CBs (stereo b8 included)
		const bool ok = RenderGuarded(static_cast<float>(*Settings::scopeFovDegrees));
		g_inOwnResolve.store(false);  // fault path may have skipped the in-function reset
		g_pendingSunExec = nullptr;
		*scopePassFlag = savedFlag;
		*stereoMaster = savedStereo;
		g_renderTid.store(0);
		Fn<RendererFn_t>(0x1d94c10)(renderer);  // rebind for the rest of the frame
		Fn<RendererFn_t>(0x1d95240)(renderer);  // clear prev-cam cache (vanilla does)

		if (!ok) {
			g_available = false;
			g_faulted = true;
			static constexpr std::string_view kSteps[] = {
				"entry"sv, "SetCameraFOV"sv, "accum prep"sv, "cull ctor"sv, "SetAccumulator"sv,
				"clear prev-cam cache"sv, "bind MRT+depth"sv, "ProcessQueuedLights"sv,
				"AccumulateScene"sv, "capture pass counts"sv, "clear decal groups"sv,
				"flush"sv, "sky accumulation"sv, "sun dir-light pass"sv, "deferred resolve"sv,
				"sky draw"sv, "unbind+finish accum"sv, "vanilla lens copy"sv, "cull dtor"sv, "done"sv
			};
			const auto step = g_lastStep;
			const auto base = REL::Module::get().base();
			const auto rva = g_lastExcAddr >= base ? g_lastExcAddr - base : 0;
			logger::critical(
				FMT_STRING("ScopeRender FAULTED at step {} ({}), code {:08X} at {:016X} (rva {:X}) — disabled for this session, falling back to copy fill"),
				step, step >= 0 && step < 20 ? kSteps[step] : "?"sv, g_lastExcCode, g_lastExcAddr, rva);
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
		++g_renders;
		const auto renders = g_renders;
		// v0.2.41 stutter-theory verifier: every sunPass value TRANSITION is logged
		// (rate-limited). If the black-silhouette bursts were dirty-config frames,
		// pre-fix logs would show 1->0 at burst start and 0->1 at burst end; post-fix
		// the transitions may still happen but the lens must stay lit (ambient frame).
		{
			static std::int32_t lastSunPass = -2;
			if (g_diagSunPass != lastSunPass) {
				static std::uint32_t sunEdgeLogs = 0;
				if (sunEdgeLogs < 40) {
					++sunEdgeLogs;
					logger::info(FMT_STRING("ScopeRender #{}: sunPass {} -> {} (0 = engine sun config dirty/unbuilt this frame)"), renders, lastSunPass, g_diagSunPass);
				}
				lastSunPass = g_diagSunPass;
			}
		}
		// Stutter probe: a frame whose world accumulation came back EMPTY renders a
		// black lens for that frame (composite shades nothing over the black
		// pre-clear). Log every occurrence, rate-limited.
		if (g_passTotal == 0) {
			static std::uint32_t zeroLogged = 0;
			if (zeroLogged < 20) {
				++zeroLogged;
				logger::warn(FMT_STRING("ScopeRender #{}: world accumulation EMPTY this frame (black-lens frame)"), renders);
			}
		}
		if (renders <= 5 || renders % 300 == 0) {
			std::string groups;
			for (std::uint32_t g = 0; g < kPassGroupCount; ++g) {
				if (g_passCounts[g] != 0) {
					fmt::format_to(std::back_inserter(groups), FMT_STRING("{}{:X}:{}"), groups.empty() ? "" : " ", g, g_passCounts[g]);
				}
			}
			std::string skyNew;
			for (std::uint32_t g = 0; g < kPassGroupCount; ++g) {
				if (g_skyAfterCounts[g] != g_skyBaseCounts[g]) {
					fmt::format_to(std::back_inserter(skyNew), FMT_STRING("{}{:X}:{:+}"), skyNew.empty() ? "" : " ", g,
						static_cast<std::int64_t>(g_skyAfterCounts[g]) - static_cast<std::int64_t>(g_skyBaseCounts[g]));
				}
			}
			logger::info(
				FMT_STRING("ScopeRender #{}: passes total={} [{}] lights={}+{} fogNulls={} fog=({:.3f},{:.3f},{:.3f}) rb={}/{}/{}/{}/{} pre62={}/{} nan={}/{} invproj={} camdata={} sun6a={}/{} sky={}/{}/{}/{} skyNew=[{}] sunPass={} sunDrew={} (drew={} gated={}) sunCfgFlags={:X} sunIsSSN={} sunSlot={}/{} sunFlags={:016X} eyes={} cull={}/{} port=({},{},{},{}) camRect=({},{},{},{},{},{}) viewport=({},{},{},{},{},{}) fov={:.3f} derived={:.3f} (M={:.2f} R={:.3f} d={:.2f})"),
				renders, g_passTotal, groups, g_diagLightsA, g_diagLightsB,
				g_diagFogNulls, g_fogRGB[0], g_fogRGB[1], g_fogRGB[2],
				g_rbSamples, g_rbDark61, g_rbDark6a, g_rbDark61Sky, g_rbDark62,
				g_rbPre62Samples, g_rbPre62Dark, g_rbNaN61, g_rbNaN62, g_invProjRejects, g_camDataBad, g_sunPreNaN, g_sunPostNaN,
				g_diagSkyEmptyPre, g_diagSkyRoots, g_diagSkyEmptyPost, g_diagSkyDrawn, skyNew,
				g_diagSunPass, g_diagSunDrew, g_sunDrewCount, g_sunGatedCount,
				g_diagSunCfgFlags, g_diagSunIsSSNSun,
				g_diagSunSlotPre, g_diagSunSlotPost, g_diagSunFlags, g_diagEyeCount,
				g_diagCullFix, g_diagFrustumAliased,
				g_diagPort[0], g_diagPort[1], g_diagPort[2], g_diagPort[3],
				g_diagRect[0], g_diagRect[1], g_diagRect[2], g_diagRect[3], g_diagRect[4], g_diagRect[5],
				g_diagViewport[0], g_diagViewport[1], g_diagViewport[2], g_diagViewport[3], g_diagViewport[4], g_diagViewport[5],
				g_lastFovDeg, g_derivedFovDeg, ScopeIdent::FovMult(), g_derivedDiscR, g_derivedEyeDist);

			// v0.2.75: the sun's own inputs. A basis of zeros (or garbage) here IS the
			// answer to "why does the sun pass add nothing" — see the note at the
			// capture site. Printed every heartbeat so it is in the log of any run.
			logger::info(
				FMT_STRING("ScopeRender #{}: SUN basis=[{:.4f},{:.4f},{:.4f} | {:.4f},{:.4f},{:.4f} | {:.4f},{:.4f},{:.4f}] pos=({:.1f},{:.1f},{:.1f}) rgb=({:.3f},{:.3f},{:.3f}) scale={:.3f} exec={}"),
				renders,
				g_diagSunBasis[0], g_diagSunBasis[1], g_diagSunBasis[2],
				g_diagSunBasis[3], g_diagSunBasis[4], g_diagSunBasis[5],
				g_diagSunBasis[6], g_diagSunBasis[7], g_diagSunBasis[8],
				g_diagSunPos[0], g_diagSunPos[1], g_diagSunPos[2],
				g_diagSunRGB[0], g_diagSunRGB[1], g_diagSunRGB[2],
				g_diagSunScale, *Settings::sunExecEnabled);

			// v0.2.73: the stage stopwatch, on its own line. Every condition that
			// changes the numbers (clamped lights, reduced render scale, sample count,
			// discarded frames) prints beside them, so a log line from a bench run
			// carries its own experimental setup and cannot be misfiled later.
			if (const auto t = GetStageTimes(); t.cpuSamples != 0) {
				std::string gpu, cpu;
				for (std::size_t i = 0; i < kStageCount; ++i) {
					fmt::format_to(std::back_inserter(gpu), FMT_STRING("{}{}={:.2f}"), i ? " " : "", kStageNames[i], t.gpuMs[i]);
					fmt::format_to(std::back_inserter(cpu), FMT_STRING("{}{}={:.2f}"), i ? " " : "", kStageNames[i], t.cpuMs[i]);
				}
				logger::info(
					FMT_STRING("ScopeRender #{}: PERF n={}gpu/{}cpu disjoint={} renderScale={:.3f} lightsClamp={} | GPU {:.2f} ms [{}] | CPU {:.2f} ms [{}]"),
					renders, t.gpuSamples, t.cpuSamples, t.disjoint,
					*Settings::perfRenderScale, g_diagLightsClamp,
					t.gpuTotalMs, gpu, t.cpuTotalMs, cpu);
			}
		}
		return true;
	}

	bool Available()
	{
		return g_available;
	}

	std::uint32_t OwnRenderThread()
	{
		return g_renderTid.load();
	}

	void TintLens(float a_r, float a_g, float a_b)
	{
		// Diagnostic: paint the lens RT 0x62 a solid color (v0.2.23 clear pattern),
		// then restore the same exit binds Render() leaves. Used by the fill hook to
		// color-code non-filled frames (burst forensics): if a black burst shows as
		// RED/GREEN instead, the burst was a non-filled frame, not a broken render.
		const auto base = REL::Module::get().base();
		const auto renderer = base + kRendererRVA;
		const auto rtm = base + kRTManager;
		Fn<SetClearColor_t>(0x1d8dc80)(renderer, a_r, a_g, a_b, 1.0f);
		Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, Addr::kRT_ScopeLens, 3);
		Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
		Fn<ClearColorNow_t>(0x1d8dd80)(renderer);
		Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x61, 3);
		Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
	}

	void CamSmoothReset() noexcept
	{
		g_camSmoothResetReq.store(true);
	}

	// v0.2.125 — LENS PRIMING. The 21:07 field run proved the placement chain
	// converges 314 ms after load with no aim — but the lens PICTURE only
	// exists after the first pose-gate activation (LensComposite init stamped
	// 33 s after load, at the first scope-in), so the correctly-placed disc
	// sat black the whole time ("didn't see the lens start"). This flag asks
	// the fill hook for one presence-time fill; set on every equip rebaseline
	// so a weapon swap re-primes with the new scope's view.
	std::atomic_bool g_lensPrimeNeeded{ true };

	bool LensPrimeNeeded() noexcept
	{
		return g_lensPrimeNeeded.load(std::memory_order_relaxed);
	}

	void LensPrimeDone() noexcept
	{
		g_lensPrimeNeeded.store(false, std::memory_order_relaxed);
	}

	bool WidgetPresentable()
	{
		// v0.2.119: may plugin-owned presence show the widget? True once the fit
		// has been applied for the current ScopeParent baseline (or always, when
		// the user runs with widgetFitEnabled=false and WANTS the vanilla look).
		// Gating presence on this is what keeps the raw oversized vanilla band
		// from ever being visible on a first-drawn weapon (field 2026-08-24).
		return !*Settings::widgetFitEnabled || g_fitAppliedAtomic.load(std::memory_order_relaxed);
	}

	void PresenceFit()
	{
		// v0.2.120 rework - the v0.2.119 version ran the fit continuously and
		// UNGATED: after an equip it applied with the PREVIOUS weapon's aperture
		// and a bound-heuristic placement computed at a hip pose (eye far away),
		// which then latched (field: disc by the hammer). Now: on an engine
		// rewrite of ScopeParent request the probe, WAIT for it, then apply the
		// fit exactly ONCE per baseline - census-only placement (see
		// ApplyWidgetFit) - and go quiet until the next equip. Live fills keep
		// continuous ownership while actually aiming, exactly as before .119.
		//
		// v0.2.121 - DON'T LATCH ON A DECLINE. The 2026-08-24 load-in defect,
		// log-proven: the first post-load probe walks fine but carries
		// one-update-stale WORLD transforms (P-Scope at (34.7,15.5,78.1) at
		// 15:44:59.048; the correct (229.1,-246.4,81.9) existed by .086), the
		// auto-place sanity gate correctly refuses ("offset implausibly large"),
		// the fit applies offset (0,0,0)... and s_done latched anyway, so the
		// good data 38 ms later was never consumed until the first live aim.
		// Now: latch only when a census placement actually LANDED, or when none
		// is expected (heuristic-only scope, fit/auto-place disabled, faulted
		// probe), or when a bounded retry budget is spent. Placement re-reads
		// world transforms live, so a retry needs a fresh PROBE only when the
		// face never resolved. Converges on the first retry in the logged case.
		const auto base = REL::Module::get().base();
		const auto player = *reinterpret_cast<std::uintptr_t*>(base + kPlayerGlobal);
		if (!player) {
			return;
		}
		constexpr std::uint32_t kRetryFrames = 30;  // ~1/3 s at 90 fps
		constexpr std::uint32_t kMaxTries = 8;      // ~2.7 s worst case, then latch
		static bool          s_done = false;
		static std::uint32_t s_tries = 0;
		static std::uint32_t s_cooldown = 0;
		bool                 warnExhausted = false;
		bool                 logRetry = false;
		int                  outcomeForLog = 0;
		__try {
			const auto sp = *reinterpret_cast<std::uintptr_t*>(player + kScopeParentInPlayer);
			if (!sp) {
				s_done = false;
				s_tries = 0;
				s_cooldown = 0;
				return;
			}
			const auto* t = reinterpret_cast<const float*>(sp + 0x60);
			const float sc = *reinterpret_cast<const float*>(sp + 0x6c);
			const bool  stillOurs = g_widget.applied &&
			                       t[0] == g_widget.wroteTx && t[1] == g_widget.wroteTy &&
			                       t[2] == g_widget.wroteTz && sc == g_widget.wroteScale;
			if (!stillOurs) {
				// the engine rewrote ScopeParent = an equip happened
				s_done = false;
				s_tries = 0;
				s_cooldown = 0;
				g_lensPrimeNeeded.store(true, std::memory_order_relaxed);
				ScopeIdent::Request();
			}
			ScopeIdent::RunIfRequested(player);
			if (s_done || ScopeIdent::ProbePending()) {
				return;
			}
			if (s_cooldown > 0) {
				--s_cooldown;
				return;
			}
			const auto outcome = ApplyWidgetFit(player, /*a_censusPlacementOnly=*/true);
			// Retry only while a census placement is genuinely expected AND the
			// machinery that could deliver one is enabled - otherwise this would
			// burn the budget on every equip of a heuristic-only scope, or spin
			// forever with widgetAutoPlace=false (a documented live knob).
			const bool wantCensus = ScopeIdent::CensusFaceExpected() &&
			                        *Settings::widgetFitEnabled &&
			                        *Settings::widgetAutoPlace;
			if (outcome == FitOutcome::kPlacedCensus || !wantCensus) {
				s_done = true;
				s_tries = 0;
				return;
			}
			if (++s_tries >= kMaxTries) {
				warnExhausted = true;
				s_done = true;
				s_tries = 0;
				return;
			}
			// The face resolved but placement declined (stale transforms): the
			// live re-read fixes itself, no probe needed. Face never resolved:
			// ask for a fresh walk before the next try.
			if (!ScopeIdent::CensusFaceResolved()) {
				ScopeIdent::Request();
			}
			s_cooldown = kRetryFrames;
			logRetry = true;
			outcomeForLog = static_cast<int>(outcome);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			logger::warn("PresenceFit faulted (ignored)"sv);
		}
		if (logRetry) {
			logger::info(FMT_STRING("PresenceFit: census placement not landed yet "
			                        "(outcome={}), retry {}/{} in {} frames"),
				outcomeForLog, s_tries, kMaxTries, kRetryFrames);
		}
		if (warnExhausted) {
			logger::warn(FMT_STRING("PresenceFit: census placement never landed after {} tries - "
			                        "keeping engine-baseline placement until the first live aim"),
				kMaxTries);
		}
	}

	void DimFrozenLens(float a_factor)
	{
		// v0.2.116 — POSE FREEZE dim. One-shot multiply of the frozen lens
		// picture, applied by the fill hook on the live->frozen edge (pose gate:
		// widget up, eye not at the tube; RT 0x62 persists so the last live
		// picture would otherwise read as live). Runs at the same fill-hook slot
		// TintLens / the phase-1 ImageSpaceCopy proved safe, and LensComposite
		// saves/restores all D3D state + invalidates the engine's cached-state
		// block itself.
		const auto base = REL::Module::get().base();
		LensComposite::Inputs in{};
		in.renderer = base + kRendererRVA;
		in.rtm = base + kRTManager;
		in.ctx = g_ctxPtrA ? *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrA) : 0;
		if (!in.ctx && g_ctxPtrB) {
			in.ctx = *reinterpret_cast<const std::uintptr_t*>(g_ctxPtrB);
		}
		const auto player = *reinterpret_cast<std::uintptr_t*>(base + kPlayerGlobal);
		in.scopeParent = player ? *reinterpret_cast<std::uintptr_t*>(player + kScopeParentInPlayer) : 0;
		in.lensLogicalRT = static_cast<std::uint32_t>(Addr::kRT_ScopeLens);
		__try {
			LensComposite::Dim(in, a_factor);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			logger::warn("DimFrozenLens faulted (ignored)"sv);
		}
	}

	void SamplePreFillLens()
	{
		// v0.2.52 — THE MISSING MEASUREMENT. Every readback so far ran INSIDE the
		// fill hook, right after our own delivery, and reported 0x62 lit (12300
		// samples, 0 dark, across sessions containing sustained blacks). But the
		// widget quad samples 0x62 much later in the frame, so that only ever
		// proved "our write is lit when we write it".
		//
		// The tint evidence says the black is inside the texture, not over it: the
		// crescent (a region of 0x62 outside the delivery footprint, drawn by the
		// SAME quad) keeps its blue while the picture area goes black. Nothing
		// opaque in front could take one and not the other. So the suspect is a
		// LATER writer that repaints only the delivery footprint — a viewport-shaped
		// draw (ImageSpace-style), not a clear (a ClearRenderTargetView would take
		// the crescent too).
		//
		// Sampling here — at fill-hook ENTRY, before our pre-paint and delivery —
		// reads what 0x62 held after everything else in the PREVIOUS frame finished.
		//   pre62 DARK while the lens looks black  => a later writer exists; find it
		//                                             (next: x64dbg conditional BP on
		//                                             the RT bind choke point
		//                                             FUN_141dbd380, phys index of
		//                                             0x62, during the sustained repro).
		//   pre62 lit while the lens looks black   => 0x62 is intact all frame and the
		//                                             black is in how the widget draws
		//                                             it (material / slot-6 bind / UV /
		//                                             alpha), not in our chain at all.
		const auto base = REL::Module::get().base();
		const auto renderer = base + kRendererRVA;
		const auto rtm = base + kRTManager;
		const auto px = SampleLogicalRT(rtm, renderer, Addr::kRT_ScopeLens,
			&g_stage62, g_rbFormat62, g_rbW62, g_rbH62);
		if (px == ~0ull) {
			return;
		}
		++g_rbPre62Samples;
		const auto dark = PixelDark(g_rbFormat62, px) ? 1 : 0;
		if (dark != 0) {
			++g_rbPre62Dark;
		}
		// Log only STATE CHANGES: a sustained black shows up as one DARK line and
		// one lit line seconds later, which is exactly what we need to correlate
		// with what the user sees (and it cannot spam the log at 90 Hz).
		if (dark != g_rbPre62State) {
			g_rbPre62State = dark;
			static std::uint32_t logs = 0;
			if (logs < 120) {
				++logs;
				logger::info(
					FMT_STRING("PREFILL 0x62 -> {} (px={:016X} fmt={} {}x{}) at sample #{}"),
					dark != 0 ? "DARK"sv : "lit"sv, px, g_rbFormat62, g_rbW62, g_rbH62,
					g_rbPre62Samples);
			}
		}
	}

	void RetryAfterFault()
	{
		if (g_faulted && !g_available) {
			g_faulted = false;
			g_available = true;
			logger::info("ScopeRender: fault latch cleared — retrying own render (retryAfterFault)"sv);
		}
	}

	bool InOwnResolve()
	{
		return g_inOwnResolve.load();
	}

	// v0.2.78: fire the deferred sun exec from inside the resolve. Called by
	// ResolveAccumBind0Hook right after the light-accum buffer is bound -- i.e. after
	// the G-buffer geometry has been drawn (which is the entire point; see the note at
	// the capture site) and before the resolve's own light volumes. One-shot: the slot
	// is cleared before invoking, so a re-entrant or repeated bind cannot draw twice.
	void RunPendingSunExec() noexcept
	{
		if (!g_pendingSunExec) {
			return;
		}
		auto fn = std::move(g_pendingSunExec);
		g_pendingSunExec = nullptr;
		fn();
	}

	void SetSunBindHooksInstalled(bool a_installed)
	{
		g_sunBindHooksInstalled = a_installed;
	}

	// --- DevBench surface (v0.2.65) -----------------------------------------

	void RequestDump()
	{
		g_dumpRequest.store(true);
	}

	void CancelDumpRequest()
	{
		g_dumpRequest.store(false);
	}

	std::uint64_t DumpEventCount()
	{
		return g_dumpEvents.load();
	}

	std::uint64_t LastDumpIndex()
	{
		return g_lastDumpIndex.load();
	}

	FovInfo GetFovInfo()
	{
		return { g_lastFovDeg, g_derivedFovDeg, g_derivedDiscR, g_derivedEyeDist };
	}

	PlacementReport GetPlacement()
	{
		PlacementReport r{};
		r.valid = g_place.valid;
		r.applied = r.valid && *Settings::widgetAutoPlace;
		for (std::size_t k = 0; k < 3; ++k) {
			r.offset[k] = g_place.offset[k];
			r.target[k] = g_place.target[k];
			r.baseWorld[k] = g_place.baseWorld[k];
			r.boundCenter[k] = g_place.boundCenter[k];
		}
		r.boundRadius = g_place.boundRadius;
		r.miss = g_place.miss;
		r.haveBoth = g_place.haveBoth;
		r.agreement = g_place.agreement;
		r.converged = g_place.converged;
		r.diverged = g_place.diverged;
		r.steps = g_place.steps;
		r.residual = g_place.residual;
		r.bestResidual = g_place.bestResidual;
		r.parentResidual = g_place.parentResidual;
		for (std::size_t k = 0; k < 3; ++k) {
			r.discWorld[k] = g_place.discWorld[k];
		}
		std::snprintf(r.reason, sizeof(r.reason), "%s", g_place.reason);
		std::snprintf(r.method, sizeof(r.method), "%s", g_place.method);
		return r;
	}

	void InvalidatePlacement()
	{
		g_placeDirty.store(true);
	}

	Diagnostics GetDiagnostics()
	{
		// Read off the render thread without locking. Every field is a scalar the
		// render thread writes and nobody else touches, so a torn read costs at
		// worst one stale value in a diagnostic — not worth a lock on the render
		// path. Treat a single sample as indicative, not atomic across fields.
		Diagnostics d{};
		d.ssnArray = g_ssnArray;
		d.accum = reinterpret_cast<std::uintptr_t>(g_accum);
		d.gfxState = g_gfxState;
		d.ctxPtrA = g_ctxPtrA;
		d.ctxPtrB = g_ctxPtrB;
		d.sunConfig = g_sunConfig;
		d.rtm = g_lastRtm;
		d.renderer = g_lastRenderer;
		d.camera = g_lastCam;

		d.renders = g_renders;
		d.lastStep = static_cast<std::int32_t>(g_lastStep);
		d.available = g_available && !g_faulted;
		d.faulted = g_faulted;
		d.sunBindHooks = g_sunBindHooksInstalled;

		d.nan61 = g_rbNaN61;
		d.nan62 = g_rbNaN62;
		d.sunPreNaN = g_sunPreNaN;
		d.sunPostNaN = g_sunPostNaN;
		d.camDataBad = g_camDataBad;
		d.invProjRejects = g_invProjRejects;
		d.fogNulls = g_diagFogNulls;
		d.dumpFiles = g_dumpFiles;

		d.passTotal = g_passTotal;
		d.lightsShadowed = g_diagLightsA;
		d.lightsQueued = g_diagLightsB;
		d.eyeCount = g_diagEyeCount;
		d.sunPass = g_diagSunPass;
		d.sunDrew = g_diagSunDrew;
		d.sunDrewCount = g_sunDrewCount;
		d.sunGatedCount = g_sunGatedCount;
		d.sunIsSSN = g_diagSunIsSSNSun;
		d.skyRoots = g_diagSkyRoots;
		d.skyDrawn = g_diagSkyDrawn;
		d.sunCfgFlags = g_diagSunCfgFlags;
		std::memcpy(d.camRect, g_diagRect, sizeof(d.camRect));
		std::memcpy(d.viewport, g_diagViewport, sizeof(d.viewport));
		d.lightsClamp = g_diagLightsClamp;
		return d;
	}

	void ResetStageTimers()
	{
		g_timerResetRequest.store(true);
	}

	StageTimes GetStageTimes()
	{
		static_assert(kSegCount == kStageCount, "stage name table and marker count disagree");
		StageTimes t{};
		t.enabled = *Settings::perfTimers;
		t.available = g_timersReady;
		t.gpuSamples = g_gpuSamples;
		t.cpuSamples = g_cpuSamples;
		t.disjoint = g_timerDisjoint;
		// Means, not totals: a run's length must not change the numbers you compare.
		const auto gN = g_gpuSamples ? static_cast<double>(g_gpuSamples) : 1.0;
		const auto cN = g_cpuSamples ? static_cast<double>(g_cpuSamples) : 1.0;
		for (std::size_t i = 0; i < kStageCount; ++i) {
			t.gpuMs[i] = g_gpuSamples ? g_gpuSum[i] / gN : 0.0;
			t.cpuMs[i] = g_cpuSamples ? g_cpuSum[i] / cN : 0.0;
		}
		t.gpuTotalMs = g_gpuSamples ? g_gpuTotalSum / gN : 0.0;
		t.cpuTotalMs = g_cpuSamples ? g_cpuTotalSum / cN : 0.0;
		return t;
	}
}
