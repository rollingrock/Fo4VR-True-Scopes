#include "TrueScopes/ScopeRender.h"

#include <DirectXMath.h>
#include <d3d11.h>

#include <fstream>

#include "Settings/Settings.h"
#include "TrueScopes/Addresses.h"
#include "TrueScopes/ScopeIdent.h"
#include "TrueScopes/LensComposite.h"
#include "TrueScopes/OneEuro.h"

// Mono world render from PrimaryWeaponScopeCamera into RT 0x61 -> 0x62,
// mirroring the engine's own deferred scene renderers (FUN_140c87320 /
// FUN_140b03d60):
//   * accumulator renderMode 0x19 = deferred; mode 0 routes every pass to the
//     forward buckets, which the resolve (FUN_1427ff8b0) never draws.
//   * renderer+4 (scope-pass flag, reader FUN_141d947d0) is checked ~10x inside
//     the resolve and reroutes light buffers 0x24/0x25 -> 0x6a/0x6b and DS
//     1 -> 0xC. The whole render runs inside a renderer+4 = 1 bracket, the
//     exact environment of the vanilla scoped world render.
//   * RT bind modes (FUN_141dbd380 + apply fn FUN_141d9b190): 0 = clear-on-
//     apply, 3 = no clear, 4 = CopyResource restore. Engine G-buffer binds:
//     slots 0-4 mode 0, slot 5 (0x23) mode 3.
//   * Accumulation = one BSShaderUtil::AccumulateScene(cam, ssn, cull, 1) —
//     for an SSN it adds every attached child; no portal lists or manual
//     subtrees.
//   * BSShaderUtil::SetCameraFOV params 3/4 are the frustum far/near planes,
//     in that order; every vanilla caller passes (far, near).
//   * The resolve draws the G-buffer groups itself, then composites into the
//     out target (param_5) via the shader-6 quad. Its internal 0x61->0x62 copy
//     is refraction-only; the per-frame lens delivery in vanilla is
//     FUN_1427b08c0(0x61, 0x62, 0) from FUN_14284e370, done explicitly here.
// Raw offsets decoded from code bytes — Ghidra high-.data labels are
// unreliable; code bytes are ground truth and match the live process.

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
		using SetCameraFOV_t = void (*)(std::uintptr_t, float, float, float);                          // 0x2804a90  BSShaderUtil::SetCameraFOV(cam, fovDeg, far, near) — param_3 = far, param_4 = near; near==far builds a NaN projection
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

		// --- sun pass ---
		// The sun is a BSDFLightShader "Dir" technique pass (flags 0x202|filter when
		// shadowed, 0x201 unshadowed — namer FUN_142922370), drawn once per frame into
		// the light accum MRT by the pre-world stage (FUN_142846d60 builds/refreshes
		// the persistent pass config, job FUN_142849990 executes it) — not by the
		// resolve, whose ssn+0x1a8/+0x1c0 loops are point/spot volumes only (cone
		// geometry, BSShaderUtil::GenerateCone in FUN_14286ffa0).
		using StateSetCamData_t = void (*)(std::uintptr_t, std::uintptr_t, std::uint8_t);              // 0x1da8c40  BSGraphics::State: update camera-data block (state, cam, slotSel)
		// 0x1da8bf0  BSGraphics::State::SetViewportFromCamera(state, cam, slotSel, minDepth, maxDepth).
		// Args 4/5 are the D3D11 viewport min/max depth, and arg 4 is a float in
		// XMM3 — declaring it as an integer type sends the value to R9, which the
		// callee never reads, leaving XMM3 holding leftover FP garbage. D3D11
		// requires 0 <= min <= max <= 1 and drops the whole RSSetViewports call
		// otherwise, silently leaving the previous viewport in effect.
		using StateSetViewport_t = void (*)(std::uintptr_t, std::uintptr_t, std::uint8_t, float, float);
		using DepthMode_t = void (*)(std::uintptr_t, std::uint32_t);                                  // 0x1d8dd60 / 0x1d8de10: depth/texture mode setters the resolve runs before lighting
		using CtxCtor_t = void* (*)(void*, std::uintptr_t, std::uintptr_t);                            // 0x2812be0  render-context ctor (ctx[0x2d0], camera, accumulator)
		using FindCamBlock_t = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uint8_t);       // 0x1daaf30  find CameraStateData block (state, camera, sel) in the state+0x140 array (stride 0x480); 0 if absent
		// Returns char: whether the draw actually happened — see the gate note at
		// the call site.
		using ExecPassConfig_t = char (*)(std::uintptr_t, std::uint8_t, void*);                        // 0x2891040  execute pass/pass-config (also takes the persistent sun config directly — FUN_142849990 does exactly that)
		using NiAVObjectUpdate_t = void (*)(std::uintptr_t, void*);                                    // 0x1c22fb0  NiAVObject::Update(obj, NiUpdateData) — recomputes world transforms down the subtree
		using FlushBatch_t = void (*)(void*);                                                          // 0x2891300  flush batched instances for the context

		template <class T>
		[[nodiscard]] T Fn(std::uintptr_t a_rva)
		{
			return reinterpret_cast<T>(REL::Module::get().base() + a_rva);
		}

		// --- data (fixed, byte/live-verified) ---
		constexpr std::uintptr_t kPlayerGlobal = Addr::kPlayerGlobal;
		constexpr std::uintptr_t kRTManager = 0x38ac010;         // RenderTargetManager (decoded from SetCurrentRenderTarget + slot-6 bind call sites)
		constexpr std::uintptr_t kRendererRVA = 0x6239340;       // BSGraphics::Renderer (live-verified)
		constexpr std::uintptr_t kIsmInstanceRVA = 0x68789e8;    // ImageSpaceManager::pInstance (RIP-decoded from FUN_1427b08c0 +0x63)
		constexpr std::uintptr_t kCamOffsetInPlayer = 0x720;     // PrimaryWeaponScopeCamera (VR camera type: eye count +0x208, port +0x214, frusta array +0x1a0)

		// --- data resolved at runtime from code anchors ---
		std::uintptr_t g_ssnArray = 0;    // BSShaderManager SSN slot array (slot 0 = world); anchor: lea in SetShadowSceneNode
		std::uintptr_t g_fovMode738 = 0;  // byte: force symmetric-FOV frusta in SetCameraFOV (vanilla scope pass forces 1)
		std::uintptr_t g_fovMode750 = 0;  // dword: which view gets symmetric FOV; 2 = all (vanilla scope pass forces 2)

		// Engine-global brackets armed while RenderImpl holds an engine global in a
		// modified state. Inline restores disarm; on the SEH fault path Render()
		// calls RestoreArmedEngineState() so a fault cannot leave previs disabled
		// engine-wide, the fov mode forced, the light counts clamped, or the ISM
		// selector held.
		struct ArmedRestore
		{
			std::uint8_t*          fov738 = nullptr;
			std::uint8_t           fov738Saved = 0;
			std::int32_t*          fov750 = nullptr;
			std::int32_t           fov750Saved = 0;
			volatile std::uint8_t* previs = nullptr;
			std::uint8_t           previsSaved = 0;
			std::int16_t*          lightsA = nullptr;
			std::int16_t           lightsASaved = 0;
			std::int16_t*          lightsB = nullptr;
			std::int16_t           lightsBSaved = 0;
			std::uint8_t*          ismBusy = nullptr;
			std::uint8_t           ismBusySaved = 0;
		};
		ArmedRestore g_armed;

		void RestoreArmedEngineState()
		{
			bool any = false;
			if (g_armed.fov738) {
				*g_armed.fov738 = g_armed.fov738Saved;
				g_armed.fov738 = nullptr;
				any = true;
			}
			if (g_armed.fov750) {
				*g_armed.fov750 = g_armed.fov750Saved;
				g_armed.fov750 = nullptr;
				any = true;
			}
			if (g_armed.previs) {
				*g_armed.previs = g_armed.previsSaved;
				g_armed.previs = nullptr;
				any = true;
			}
			if (g_armed.lightsA) {
				*g_armed.lightsA = g_armed.lightsASaved;
				g_armed.lightsA = nullptr;
				any = true;
			}
			if (g_armed.lightsB) {
				*g_armed.lightsB = g_armed.lightsBSaved;
				g_armed.lightsB = nullptr;
				any = true;
			}
			if (g_armed.ismBusy) {
				*g_armed.ismBusy = g_armed.ismBusySaved;
				g_armed.ismBusy = nullptr;
				any = true;
			}
			if (any) {
				logger::warn("fault path restored armed engine globals"sv);
			}
		}
		std::uintptr_t g_ctxPtrA = 0;     // &deferred-context ptr (DAT_146235ac8); anchor in FUN_141db9f80
		std::uintptr_t g_ctxPtrB = 0;     // &immediate-context ptr (DAT_146235ac0); anchor in FUN_141db9f80
		std::uintptr_t g_sunConfig = 0;   // persistent sun BSDFLightDir pass config (Ghidra DAT_146886758); anchor: lea in job FUN_142849990
		std::uintptr_t g_gfxState = 0;    // BSGraphics::State (real RVA 0x65A2AB0 — Ghidra's DAT_146541ef0 label is section-shifted); anchor: first lea in the resolve

		void* g_accum = nullptr;
		bool g_available = false;
		bool g_faulted = false;  // fault latch (vs never-initialized) — clearable via RetryAfterFault
		std::atomic<std::uint32_t> g_renderTid{ 0 };  // thread id while our renderer+4 bracket is live (see Hooks::ScopePassReadHook)
		// DevBench surface: render counter and the per-render engine pointers.
		std::uint64_t              g_renders = 0;
		std::uintptr_t             g_lastRtm = 0, g_lastRenderer = 0, g_lastCam = 0;
		bool g_sunBindHooksInstalled = false;      // set by Hooks::Install when the two resolve bind sites are hooked
		// The sun exec, deferred to inside the resolve. Holds a lambda that closes
		// over Render()'s locals; Render() is on the stack for the whole resolve
		// call, so those references stay valid. Cleared unconditionally after the
		// resolve (and on the fault path) so a stale closure can never be invoked
		// on a later frame.
		std::function<void()> g_pendingSunExec;
		std::atomic_bool g_inOwnResolve = false;   // true only while our resolve call is on the stack (the hooks key off this)
		// The deferred-decal stage (bullet holes). Armed per render, fired once
		// from ResolveAccumBind0Hook before the accum bind (G-buffer still bound —
		// the stage reads and writes it).
		std::atomic_bool g_pendingDecalStage{ false };
		std::uintptr_t   g_decalStageCam = 0;
		std::uint32_t    g_diagDecalN = 0;
		std::uint32_t    g_diagDecalBatches = 0;
		// Fault attribution — advanced through DecalStageImpl so the warn names
		// the dying step. 0=entry 1=plane 2=locked 3=visited 4=refs-held
		// 5=render-call 6=rendered 7=refs-released.
		std::atomic<std::uint32_t> g_decalStep{ 0 };

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
		std::int32_t g_diagLightsClamp = -1;  // perfLightsMax actually applied this render (-1 = none)
		// The sun NiLight's world basis / position / color as the exec sees them.
		float g_diagSunBasis[9] = {};  // rows at niLight+0x70/+0x80/+0x90
		float g_diagSunPos[3] = {};
		float g_diagSunRGB[3] = {};
		float g_diagSunScale = 0.0f;
		std::int32_t g_diagSunSlotPre = -2;   // sun (shadowed light 0) +0x18 shadow-map slot before resolve
		std::int32_t g_diagSunSlotPost = -2;  // ... and after (0xff = no slot -> the resolve skips the light)
		std::uint64_t g_diagSunFlags = 0;     // sun light +0x108 flags qword
		std::int32_t g_diagSunPass = -1;      // -1 not attempted, 0 config invalid, 1 executed
		// g_diagSunPass only says the call was made; these carry FUN_142891040's
		// return value — whether the draw actually happened.
		std::int32_t g_diagSunDrew = -1;      // -1 not attempted, 0 gated off, 1 drew
		std::uint64_t g_sunDrewCount = 0;
		std::uint64_t g_sunGatedCount = 0;
		std::uint32_t g_diagSunCfgFlags = 0;  // sun config technique flags (+0x48): 0x202|filter = shadowed Dir, 0x201 = unshadowed
		std::int32_t g_diagSunIsSSNSun = -1;  // config light[0] == *(ssn+0x248)?
		std::int32_t g_diagEyeCount = 0;
		// Culling forensics: the two frusta the engine keeps on a camera.
		// eye0 = [cam+0x1a0][0], the one SetCameraFOV writes and the projection uses;
		// comb = *(cam+0x200), the combined frustum BSCullingGroup::SetCamera culls
		// with. On a mono camera the engine never refreshes comb's lateral extents.
		// combPre is comb as found before the mirror below, so the log records the
		// stale value that was doing the culling.
		float g_diagFrustumEye0[7] = {};
		float g_diagFrustumCombPre[7] = {};
		std::int32_t g_diagFrustumAliased = -1;  // 1 = [cam+0x200] == [cam+0x1a0] (mirror is a no-op), 0 = distinct, -1 = unknown
		std::int32_t g_diagCullFix = -1;         // -1 not attempted, 0 skipped (setting off / null ptr), 1 applied
		float g_fogRGB[3] = { 0.05f, 0.05f, 0.05f };  // last-good fog color (ambient base); dim gray until first read
		std::uint64_t g_diagFogNulls = 0;             // frames where the fog singleton was null (stutter forensics)

		// 1-pixel GPU readback of the lens chain. D3D11 immediate context global
		// (RIP-derived from Renderer::ClearColor 0x141d8dd80: ID3D11DeviceContext*
		// at 0x146235ab0; RTV at renderer+0xa78+idx*0x30 => live ID3D11Texture2D*
		// at renderer+0xa70+idx*0x30, the fields the mode-4 CopyResource restore
		// uses). Each sampled render costs two Map() sync stalls — diagnostic
		// only, off by default.
		constexpr std::uintptr_t kD3DContextRVA = 0x6235ab0;

		// ---- per-stage GPU + CPU timing ------------------------------------------
		//
		// GPU: D3D11 timestamp queries. Eight marks bracket the seven stages of
		// Render(); a TIMESTAMP_DISJOINT query per render carries the tick frequency
		// and the validity flag (the GPU clock can change frequency mid-flight, and
		// the D3D contract is to discard such a frame — counted, never averaged in).
		// Results are collected 2-3 renders later with DONOTFLUSH, so nothing here
		// ever stalls the pipeline the way the 1-pixel readbacks do.
		//
		// CPU: QueryPerformanceCounter at the same marks. Both are needed —
		// AccumulateScene is CPU pass-list building with almost no GPU work, while
		// the resolve is the reverse; a single number could not tell those apart.
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

		// Set by ResetStageTimers() from the DevBench thread; consumed on the render
		// thread. A latch, not an edge detect: two config writes back to back can
		// land between renders, and an off->on edge seen by the render thread would
		// miss them — the stats would silently keep session-long running means.
		// Whenever the flag is set, the next render clears its accumulators.
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

		// Sky accumulation forensics: group counts immediately before and after the
		// sky-root accumulation — the delta says which groups the sky passes
		// actually landed in.
		std::uint32_t g_skyBaseCounts[kPassGroupCount] = {};
		std::uint32_t g_skyBaseTotal = 0;
		std::uint32_t g_skyAfterCounts[kPassGroupCount] = {};
		std::uint32_t g_skyAfterTotal = 0;

		// Specular (and world-pos reconstruction generally) needs the inverse
		// projection at staging+0x1d0, which only the engine's scene renderers
		// write manually (FUN_140c875f0: XMMatrixInverse of the matrix at
		// camData+0x90, stored transposed at +0x1d0..+0x20c). The state camera-data
		// update (0x1da8c40) does not touch it, and the camera commit FUN_141daa860
		// (inside the viewport setter 0x1da8bf0) copies the camera's
		// CameraStateData block into a fixed staging block at *(ctx+0x25d0)
		// spanning offsets +0x20..+0x1c8 only — +0x1d0 is never propagated, so
		// writing it into the camera block does nothing. The draws (BSDFLight spec
		// world-pos reconstruction, composite) consume staging+0x1d0. Correct
		// recipe: source = our block+0x90 (valid after SetCamData), destination =
		// *(ctx+0x25d0)+0x1d0, written any time before the draws (subsequent
		// commits leave +0x1d0 untouched).
		// g_invProjRejects counts a singular inverse-projection source (each one
		// would otherwise write a full NaN matrix into the staging block). Never
		// fires in practice; kept as hygiene.
		std::uint64_t g_invProjRejects = 0;
		std::int32_t g_invProjState = -1;




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
			// XMMatrixInverse returns a matrix of QNaN when the source is singular,
			// and the source is a cached camera-state block (FindCamBlock) that can
			// be stale, recycled or not yet populated for this mono camera. A NaN
			// here lands in staging+0x1d0 — the inverse-projection constant the
			// sun's fullscreen BSDFLightDir pass and the composite both consume —
			// and one poisoned draw turns the entire light accumulation buffer to
			// NaN. Validate, and on failure leave the previous value in place: a
			// stale inverse-projection is wrong-looking at worst, NaN is a black
			// lens.
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

			// BSDFLightShader::SetupGeometry builds the Dir-light constants from the
			// staging block's eye-1 slots (+0x260/+0x2a0 — second 0x210-stride view
			// block) and the ctx eye-1 position (+0x25a0). The camera commit only
			// fills as many eye slots as cam+0x208 — a mono camera fills eye 0 only,
			// leaving eye 1 with the main view's right-eye matrices: wrong
			// view-space sun direction, garbage spec. Mirror eye 0 -> eye 1 after
			// the commit; the resolve's internal re-commit also only writes eye 0,
			// so the mirror survives.
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

		// --- widget fit ----------------------------------------------------------
		// The vanilla VR scope widget (Data\Meshes\VR\Scope\world_scope.nif) hangs off
		// the engine's "ScopeParent" NiNode at player+0x7d0: TS_SetupScopeRig
		// (0x140ef21a0) does ScopeParent->AttachChild(WSScopeModel->model). Its render
		// surface `render_circle:0` is a flat disc of radius 7.852 centred exactly on
		// ScopeParent's origin, so
		//        scale = aperture_radius / 7.852
		// fits the widget to a real scope's lens. Shipped scopes measure 0.76–4.56, i.e.
		// scale 0.097–0.581 — the vanilla widget is 2–6x oversized, which is why the
		// real scope mesh shows through the middle of it.
		//
		// Two things this must get right:
		//
		// 1. ScopeParent's world transform is not recomputed per frame — the engine
		//    only refreshes it at equip/3D-change. Writing the local transform alone
		//    does nothing visible; NiAVObject::Update must follow, exactly as the
		//    engine's own path (FUN_140f0a9f0) does after writing it.
		//
		// 2. The engine rewrites that local transform at equip (not at scope-in).
		//    Offsets are applied to a captured baseline, never accumulated onto the
		//    current value — invalidating the baseline on any event re-captures our
		//    own previously written value and stacks the offset every cycle.
		//
		//    The correct invalidation signal is not an event at all: compare the
		//    node's current values with the exact ones we last wrote. Identical =>
		//    still ours, keep the baseline. Different => the engine wrote it at
		//    equip, so what is there now is pristine and becomes the new baseline.
		//    Exact float comparison is right here precisely because we wrote those
		//    bits ourselves.
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
		// Cross-thread mirror of g_widget.applied (read by the game-thread
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

		// --- derived scope FOV ---------------------------------------------------
		// What the player should perceive is the scope's real magnification M, and
		// that fixes the render FOV completely:
		//
		//   the lens disc has world radius  R = 7.852 * ScopeParent.worldScale
		//   the eye sits distance           d  from the disc centre
		//   so the disc subtends            tan(theta_disc / 2) = R / d
		//   and a point rendered at angle b lands where the eye sees it at
		//   tan-1( tan(b) * (R/d) / tan(theta_render/2) ), i.e.
		//                                   M = tan(theta_disc/2) / tan(theta_render/2)
		//   therefore                       theta_render = 2*atan( (R/d) / M )
		//
		// M comes from the weapon's zoomData fovMult, so this generalises to every
		// optic for free.
		//
		// Computing d per render is deliberate: a real scope's magnification does
		// not change as you move your head back, but its visible field narrows.
		// It also costs nothing — SetCameraFOV already runs every render.
		//
		// Layout, read out of the VR binary rather than assumed:
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

		// --- automatic widget placement ------------------------------------------
		//
		// One hand-tuned offset per scope does not scale, so derive the placement.
		// The target is the ocular face — the rear end of the scope, the end the
		// player looks into. Two facts make that computable without mesh data:
		//
		//   * the walk gives a world-space bounding sphere for the P-Scope subtree,
		//     centre C and radius r;
		//   * when the scope is raised the eye is very nearly on the tube axis, so
		//     the direction from C toward the eye E is the tube axis, to within the
		//     small angle the player's head is off-centre.
		//
		// so   target = C + normalize(E - C) * r.
		//
		// This is a heuristic and is written down as one. A bounding sphere over a
		// long tube has radius ~= half its length, so the target lands near the rear
		// face; the error is the sphere's overshoot past a flat face (about
		// tube_radius^2 / length, well under a unit for a real scope) plus whatever
		// mounts and rails inflate the sphere by. The second term is the one to
		// distrust, which is why this is computed but not applied by default:
		// `widgetAutoPlace` is off and the candidate is reported every probe.
		//
		// ScopeParent's translate is in its parent's space, so the world-space target
		// has to come back through that transform: v = R^T * (W - T) / s.
		//
		// Everything the placement reads is sampled inside one render. Reads taken
		// over separate DevBench round trips sample different frames, and a rifle
		// held in VR moves units between them — a cross-frame check measures hand
		// tremor as much as geometry.
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
			// on, so the gap between it and the exact census answer on a measured
			// scope is the only honest estimate of its error.
			char  method[16] = "none";
			// True when the target came from the census face, which is pure weapon
			// geometry and does not involve the eye. Such a target can be recomputed
			// every frame without jitter — and must be, see ApplyWidgetFit.
			bool  eyeIndependent = false;
			bool  haveBoth = false;
			float agreement = 0.0f;  // |exact - heuristic|, world units
			// --- loop state (kept for the report) ---
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
		// a_keepLoopState is set when the caller refreshes the target for a new
		// frame: the geometry must be re-read (the weapon moves) but the
		// accumulated offset, step count and best-so-far must not be reset.
		// One Euro damping of the scope camera's world orientation, once per live
		// fill, at the single coherent point after SetCameraFOV's Update has
		// derived a fresh weapon-parented pose and before anything downstream
		// consumes it (culling planes, the camera-state commits, AccumulateScene
		// all read the filtered basis). Feed-forward and self-healing: the next
		// fill regenerates the raw pose from the parent, so the engine never
		// accumulates the correction. Orientation only — positional tremor is
		// unamplified parallax, and filtering position risks the camera swimming
		// inside the tube. Never touches weapon or hand nodes, never ScopeParent:
		// the widget disc tracks the honest weapon, only the lens image is damped,
		// and the composited reticle marks the damped camera's boresight so image
		// and reticle stay mutually coherent (both lag true aim by <= maxLag).
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
			// Adaptive core (Casiez 2012). The magnitude-only derivative cannot
			// average oscillating tremor toward zero — at rest the effective
			// cutoff is minCutoff + beta*mean|tremor rate|. Tune minCutoff first
			// with beta=0 (the ladder in the TOML).
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
			// Transposed on read — the engine stores this column-major, so the true
			// mapping is world = T + s * (M_stored^T * v); see ScopeIdent::ReadGeom.
			// With R read this way both the forward use below and the R^T inverse
			// further down are the textbook forms.
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

			// Candidate 1, the heuristic: target = C + normalize(E - C) * r.
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

			// Candidate 2, the exact answer: the census-measured face centre pushed
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

			// Staleness guard. The post-load probe walks fine but its world
			// transforms still hold the pre-placement rig pose until the first
			// skeleton update lands, which can produce a large-but-under-cap
			// offset that latches a bad placement. The eye can never be more than
			// arm's length + weapon (~60 units) from an equipped scope, so
			// distance-from-eye is the principled test; the decline feeds
			// PresenceFit's bounded retry, which converges as soon as the
			// transforms are real.
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
				// Carry the loop forward: its offset is the accumulated correction,
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

		// Observe the achieved placement. Does not feed back.
		//
		// A closed loop here (measure where the disc landed, correct the offset by
		// the residual, repeat) is a mistake: it compares a target read this frame
		// against a disc position produced by last frame's write, so while the
		// weapon moves the loop integrates the motion into the offset and walks
		// the disc off the weapon — with the residual staying small the whole
		// time. With ScopeIdent::OcularFaceWorld re-reading the live shape
		// transform, every term — face, shape transform, parent transform,
		// baseline — is read in one frame and combined into a local-space offset,
		// so weapon motion cancels identically and there is nothing left for a
		// servo to fix. This measures and reports; the number is what would catch
		// the transform going wrong again, and it cannot make anything worse by
		// being large.
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


		// What a fit call actually did with placement. Only the callee knows
		// whether the census gate consumed a placement, so it says so — a caller
		// latching "done" on a call that declined would otherwise hide the next
		// good probe.
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

			// Re-baseline whenever the node holds something we did not write — that is the
			// engine having rewritten it at equip, and only then is the value pristine.
			// Doing this on an event (scope-in) re-captures our own output and compounds
			// the offset every cycle; see the note above.
			const bool rebaselined = !stillOurs;
			if (!stillOurs) {
				g_widget.baseTx = t[0];
				g_widget.baseTy = t[1];
				g_widget.baseTz = t[2];
				g_widget.baseScale = *s;
				g_widget.captured = true;
				g_widget.applied = false;
				g_fitAppliedAtomic.store(false, std::memory_order_relaxed);
				// An engine rewrite of ScopeParent is the equip signal — re-identify
				// the scope so a weapon swap can never keep the old weapon's
				// aperture/placement.
				ScopeIdent::Request();
				logger::info(FMT_STRING("WIDGET FIT baseline: translate=({:.3f},{:.3f},{:.3f}) scale={:.3f}"),
					g_widget.baseTx, g_widget.baseTy, g_widget.baseTz, g_widget.baseScale);
				// Sanity tripwire: the engine parks ScopeParent within a few units of the
				// weapon. A baseline far from that means corrupted state was captured, and
				// every offset from here is measured from the wrong origin. Say so rather
				// than fit to garbage.
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

			// Per-scope aperture, falling back to the widgetApertureRadius setting
			// for an unrecognised scope.
			const auto  aperture = ScopeIdent::ApertureRadius();
			// Under-aperture sizing: shrink the disc slightly inside the housing
			// hole so the seam is never a bright picture pixel. Applies after the
			// per-scope table, so every entry keeps its relative fit.
			const float apScale = std::clamp(static_cast<float>(*Settings::widgetApertureScale), 0.8f, 1.1f);
			const float scale = (aperture * apScale) / kVanillaRenderCircleRadius;
			// A zero/absurd scale makes the lens vanish or swallow the view, and the user
			// cannot tell that apart from a broken render — refuse instead of guessing.
			if (!(scale > 0.001f && scale < 8.0f)) {
				static bool warned = false;
				if (!warned) {
					warned = true;
					logger::warn(FMT_STRING("WIDGET FIT refused: scale {} out of range (aperture={})"),
						scale, aperture);
				}
				return FitOutcome::kNotWritten;
			}

			// Per-scope offsets, falling back per axis to the global settings.
			float ox = 0.0f, oy = 0.0f, oz = 0.0f;
			ScopeIdent::WidgetOffsets(ox, oy, oz);

			// Recompute policy. ScopeParent hangs off PrimaryUIAttachNode, not the
			// weapon, so the local offset that puts the disc on the lens depends on
			// the relative pose of the weapon and that UI node — which changes
			// through the scope-raise animation and as the hands move relative to
			// the head. Computing the offset once at equip would bake in whatever
			// transitional pose existed at that instant.
			//
			// A census target is pure weapon geometry with no eye term, so
			// recomputing it per frame is stable, not jittery. The bound heuristic
			// does use the eye, so that one stays latched — recomputing it per
			// frame would creep the disc with head motion.
			//
			// A completed probe is new ident data: anything latched from before it
			// (e.g. a bound-heuristic placement computed in the probe gap) must
			// re-latch. The generation counter makes that structural instead of
			// hoping the paths happen to order correctly.
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
				// above zero means ScopeParent.world is not parent.world composed
				// with its local translate — i.e. the layout is not what this code
				// believes. The number is here so that is diagnosed rather than
				// inferred from a misplaced disc.
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
				// censusPlacementOnly (the presence path): the bound heuristic uses
				// the eye position and is garbage at hip poses. Presence applies
				// placement only from the eye-independent census target; heuristic
				// scopes keep the engine baseline until the first live aim places
				// them. Open loop, computed from same-frame data — see
				// ObserveAutoPlacement for why a closed loop here is a regression.
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
			// The census path recomputes per frame, so the write stays per-frame
			// (that is the live tracking) but the log only speaks when something
			// meaningful changed: a rebaseline, a scale change, or the offset
			// moving more than a quarter unit since the last logged value.
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

			// Publish the per-render engine pointers for DevBench, so an ad-hoc
			// read can target the camera or the RT manager directly.
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

			// Identify the equipped scope. Runs only when something asked for it —
			// scope-in, or a DevBench request — because it walks the weapon's 3D
			// and calls into the engine's inventory code, neither of which belongs
			// in a per-frame path. Must precede ApplyWidgetFit, which consumes the
			// aperture it resolves.
			ScopeIdent::RunIfRequested(player);

			// Derive the FOV from the scope's real magnification and the lens
			// geometry. Always computed so it can be compared against the
			// hand-tuned value in the log, but only used when scopeFovDegrees is 0 —
			// a derivation that quietly replaces a confirmed calibration is how a
			// known-good state gets lost.
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

			// Mark 0 — everything from here to the light fit is "setup".
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
			g_armed.fov738 = mode738;
			g_armed.fov738Saved = saved738;
			g_armed.fov750 = mode750;
			g_armed.fov750Saved = saved750;
			// Params 3/4 are (far, near) — not (near, far). SetCameraFOV stores
			// param_4 into the frustum near slot and param_3 into the far slot, and
			// every vanilla caller passes (far, near). The engine is standard-Z
			// everywhere (proj z-row f/(f-n) in FUN_141da8e60, DS clear = 1.0,
			// geometry depth mode 3 = LESS_EQUAL + write), so a swapped pair builds
			// a reversed projection where the farthest fragment always wins.
			Fn<SetCameraFOV_t>(0x2804a90)(
				cam, a_fovDeg,
				static_cast<float>(*Settings::scopeFarClip),
				static_cast<float>(*Settings::scopeNearClip));
			*mode738 = saved738;
			*mode750 = saved750;
			g_armed.fov738 = nullptr;
			g_armed.fov750 = nullptr;

			// Damp the scope camera's orientation here — the Update above just
			// derived a fresh world pose; everything downstream (culling planes,
			// camera-state commits, AccumulateScene) consumes the filtered basis
			// coherently. See ApplyCamSmooth.
			ApplyCamSmooth(cam);

			// This VR camera type is not flatrim NiCamera. From SetCameraFOV's own
			// code: eye/frustum count @ +0x208, aspect @ +0x210, port @ +0x214..0x220
			// (which it forces to full-frame {0,1,1,0} itself — do not touch +0x184,
			// that is per-eye frustum data on this type).


			// Publish the frustum we just built to the slot the culler actually reads.
			//
			// AccumulateScene (0x27ff370) ignores the BSCullingProcess's own frustum
			// for visibility: it builds a stack BSCullingGroup and calls
			// BSCullingGroup::SetCamera (0x638270), which derives its six clip planes
			// from *(NiFrustum**)(cam + 0x200) — the combined (all-eye union) frustum
			// — together with the camera's world transform at cam+0x70/+0xa0.
			//
			// SetCameraFOV writes the per-eye frusta at [cam+0x1a0] + eye*0x1c and
			// rebuilds the combined one only in its `if (1 < eyeCount)` tail
			// (FUN_141c2bf80, fed by the HMD eye projections). Our scope camera is
			// mono (eyes=1), so that tail never runs and FUN_141c2bee0 refreshes just
			// the combined frustum's near field — the lateral extents would stay at
			// whatever the engine last left there and the render would cull against
			// a frustum that has nothing to do with the scope.
			//
			// Mirroring eye 0 into the combined slot is the whole fix. Safe to mutate:
			// this camera is PrimaryWeaponScopeCamera, which only the (disarmed)
			// vanilla scope redirect and this render consume, and the engine
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

			// Accumulator: deferred renderMode 0x19 (0 = forward buckets, which the
			// resolve never draws), the deferred enable bytes both engine templates
			// set, world SSN, eye positions.
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

			// Stack culling process bound to our accumulator, with our camera at
			// +0x18 — UpdateLightList dereferences it (the engine's own world path
			// sets cull+0x18 = camera, FUN_14284e370).
			RENDER_STEP(3);
			alignas(16) std::uint8_t cullBuf[0x1a0];
			Fn<CullCtor_t>(0x1d4d8e0)(cullBuf, 0);
			*reinterpret_cast<std::uintptr_t*>(cullBuf + 0x18) = cam;
			// Also give the culling process its own frustum, exactly as the engine's
			// cull helper FUN_141d4dc50 does (set +0x18, then SetFrustum). The ctor
			// zeroes the NiFrustum at +0x20 and the six planes at +0x3c, so without
			// this BSCullingProcess::TestBaseVisibility hands degenerate planes to
			// the object's vtable+0x170 visibility test. Separate from the camera
			// mirror above: AccumulateScene's own culling uses the camera's combined
			// frustum, not this. Both are needed and both hang off the one setting
			// so an A/B stays single-flag.
			if (*Settings::cullToScopeFrustum) {
				if (const auto* const eye0 = *reinterpret_cast<const float**>(cam + 0x1a0)) {
					Fn<CullSetFrustum_t>(0x1c452b0)(cullBuf, eye0);
				}
			}
			RENDER_STEP(4);
			Fn<CullSetAccum_t>(0x1d4d9c0)(cullBuf, g_accum);

			RENDER_STEP(5);
			Fn<ClearPrevCam_t>(0x1d95240)(renderer);

			// Scope G-buffer setup, decoded from the world path's own +4 remap site
			// (FUN_142844180): when renderer+4 is set the engine binds the dedicated
			// scope-sized mono G-buffer 0x63/0x64/0x66/0x67/0x68/0x69 with DS 0xC,
			// all mode 0 (clear). Binding the stereo double-wide 0x1c..0x23 with the
			// mono DS 0xC is an RTV/DSV size mismatch D3D11 rejects silently —
			// nothing draws.
			RENDER_STEP(6);
			Fn<ClearPrevCam_t>(0x1d94990)(renderer);  // Renderer::ResetState

			// Pre-clear the composite target. The resolve binds 0x61 mode 3 (never
			// clears) and the composite only shades G-buffer-covered pixels — empty
			// regions would otherwise keep stale frames (ghosting). Engine pattern
			// (FUN_1401f8bb0): bind + ClearColor.
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

			// Re-fit the lights for our camera. The main frame's light update fitted
			// every light's screen proxy volume to the main camera; drawn through
			// the zoomed scope projection those volumes cover only part of the
			// screen. Safe with cull+0x18 = our camera, and the next main frame
			// re-fits for its own camera, so the mutation self-heals.
			TimerMark(1);  // end "setup" (camera, binds, clears)
			RENDER_STEP(7);
			using ProcessLights_t = void (*)(std::uintptr_t, void*);
			Fn<ProcessLights_t>(0x27eab40)(ssn0, cullBuf);
			TimerMark(2);  // end "lights" (ProcessQueuedLights)

			// Previs bypass. Inside AccumulateScene the per-object frustum test is
			// gated:
			//   BSCuller::ProcessVectorFrustum(culler, 0)   -- 0x1d4b9d0, the SIMD
			//   6-plane test -- runs only if
			//       culler.count != 0 &&
			//       (BSPreCulledObjects::QEnabled() == false || culler[+0x3a6f] != 0)
			// and culler[+0x3a6f] is copied from BSCullingGroup+0x17a, which that
			// group's constructor (0x6373e0) sets to 0 and nothing in this path ever
			// sets otherwise (the group is a stack local inside AccumulateScene,
			// unreachable from here). QEnabled() is normally true:
			//   [0x146878ad0]=1 (enabled) && [0x14391d830]=1 (want) && [0x146878ad1]=0 (temp-disable)
			// so the engine skips frustum culling entirely here and leans on previs —
			// precomputed per-cell visibility, computed once per frame for the main
			// camera in the frame prep (FUN_1427dff70) — and the scope render would
			// inherit that whole visible set.
			//
			// Flip the temp-disable byte across our accumulation only. Deliberately
			// the raw byte and not BSPreCulledObjects::SetTempDisabled (0x1427e0de0):
			// that setter also walks every registered visibility callback and
			// un-hides the objects previs had hidden. Writing the byte keeps those
			// objects hidden (their app-cull flags are untouched), so previs
			// occlusion is kept and real frustum culling is added.
			const auto previsTempDisable = base + 0x6878ad1;
			std::uint8_t savedPrevis = 0;
			const bool bypassPrevis = *Settings::cullToScopeFrustum;
			if (bypassPrevis) {
				savedPrevis = *reinterpret_cast<volatile std::uint8_t*>(previsTempDisable);
				*reinterpret_cast<volatile std::uint8_t*>(previsTempDisable) = 1;
				g_armed.previs = reinterpret_cast<volatile std::uint8_t*>(previsTempDisable);
				g_armed.previsSaved = savedPrevis;
			}

			RENDER_STEP(8);
			Fn<AccumScene_t>(0x27ff370)(cam, ssn0, cullBuf, 1);

			// Restore immediately: this is a process-global the main view reads too.
			// Worst case a concurrent engine cull sees it disabled for a few hundred
			// microseconds and does a real frustum test instead of skipping one —
			// more work, never wrong output.
			if (bypassPrevis) {
				*reinterpret_cast<volatile std::uint8_t*>(previsTempDisable) = savedPrevis;
				g_armed.previs = nullptr;
			}
			TimerMark(3);  // end "accum" (AccumulateScene — CPU pass-list building)

			RENDER_STEP(9);
			CapturePassCounts(accum);

			// Drop stale pass-list groups from our accumulator: their passes can be
			// freed back to the pool by their owning property's ClearRenderPasses at
			// any time, so drawing them later dereferences freed passes. Group 0x11
			// is sky, not decals; the real decal groups are 5/6, which the resolve
			// draws itself. 0x17 = sun glare, drawn by the resolve tail with the
			// same stale-pass exposure (gated: dropSunGlareGroup).
			RENDER_STEP(10);
			using ClearGroup_t = void (*)(std::uintptr_t);
			for (const std::uint32_t g : { 9u, 0x11u, 0x12u, 0x13u }) {
				Fn<ClearGroup_t>(0x281ecb0)(accum + 0x18 + static_cast<std::uintptr_t>(g) * 0x678);
			}
			if (*Settings::dropSunGlareGroup) {
				Fn<ClearGroup_t>(0x281ecb0)(accum + 0x18 + 0x17u * 0x678);
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

			// --- sky group check ---
			// Sky is not group 0xC (that is the refraction group).
			// BSSkyShaderProperty::GetRenderPasses (FUN_14288e400) hard-codes the
			// group by skyObjectType: dome/sun/stars/moons -> group 0x11, clouds ->
			// 0x12 (sun glare -> 0x17, skipped); renderMode 0x19 passes its only
			// gate. Vanilla draws 0x11/0x12/0x13 in stage FUN_14284d680 right after
			// the composite: slot0 RT 0x61 (scoped), slot1 aux RT 0x69, DS 0xC,
			// depth-tested — sky fills only far pixels. The world SSN accumulation
			// does include these passes, but the pre-resolve group drop above
			// deletes them — correctly: they are stale already-drawn-and-released
			// passes (the resolve draws 0x11/9 internally). Fresh ones are
			// re-registered post-resolve and drawn here.
			RENDER_STEP(12);
			const auto skyGroup = accum + 0x18 + 0x11 * 0x678;
			g_diagSkyEmptyPre = static_cast<std::int32_t>(Fn<GroupEmpty_t>(0x281f2c0)(skyGroup));

			// --- sun ---
			// Pre-draw the sun's BSDFLightDir pass into the scope light-accum MRT.
			// The engine draws the sun once per frame (pre-world stage) into the
			// main view's 0x24/0x25; the resolve only draws point/spot volumes, so
			// 0x6a would stay sunless. Recipe = queued job FUN_142849990 verbatim:
			// bind accum MRT, camera state, render states (base ctx+0x1ee0: +0xb0=5,
			// +0xbc=1, +0xc8=5(additive), +0xd0=1), execute the persistent config,
			// flush, restore +0xb0=0. The resolve's own accum clear (bind mode 0) is
			// forced to mode 3 by the Hooks::Install call-site hooks while
			// g_inOwnResolve — without them the sun would be wiped, so the draw is
			// skipped entirely if they are not installed.
			RENDER_STEP(13);
			g_diagSunPass = -1;
			// The fog clear, accum binds, camera state, and hook arming are
			// independent of the engine's sun pass-config, so they must not gate on
			// its dirty byte: a frame that skips them leaves the resolve's accum
			// binds in clear-on-apply mode with the current clear color (the step-6
			// black) -> black world silhouettes. The accum setup always runs when
			// the hooks are available; only the sun exec is skipped on dirty-config
			// frames (one frame of ambient-only lighting).
			bool accumSetup = false;
			if (g_sunBindHooksInstalled && g_gfxState && *Settings::sunEnabled) {
					// Clear the accum RTs deterministically (bind as slot 0 + commit +
					// immediate ClearColor) — but to the fog/ambient color, not black.
					// Vanilla's mode-0 clear-on-apply executes lazily at the first
					// draw, by which point the resolve has re-set the clear color to
					// the fog RGB (FUN_1427aeeb0()+0x1d4..0x1dc, alpha 1) — the accum
					// starts at the ambient base light level. A black clear deletes
					// that base term: the whole scene loses ambient and reads
					// near-black except sun-facing surfaces.
					{
						using GetFogSingleton_t = std::uintptr_t (*)();
						const auto fog = Fn<GetFogSingleton_t>(0x27aeeb0)();
						// This clear is the scope's entire ambient light level, so a
						// null fog singleton (transient during streaming/weather
						// churn) must not fall back to zero — that paints whole
						// frames black. Fall back to the last good color instead,
						// and count nulls for the log.
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
					// 2..5 unbound like the resolve. Specular is left unbound: the
					// sun pass's spec output is unreliable, and with spec unbound the
					// sun contributes diffuse only while 0x6b stays cleared, which
					// the composite reads as "no sun specular" (correct-looking minus
					// highlights).
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x6a, 3);
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, -1, 3);
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 2, -1, 3);
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 3, -1, 3);
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 4, -1, 3);
					Fn<SetCurRT_t>(0x1db9dd0)(rtm, 5, -1, 3);
					Fn<SelectDS_t>(0x1db9e40)(rtm, 0xc, 3, 0);
					Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);

					// Camera state exactly as the resolve sets it before its light loops,
					// plus the manual inverse-projection write the engine's scene
					// renderers do (specular/world-pos reconstruction input; see
					// WriteInverseProj).
					Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 1);
					Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 0);
					Fn<StateSetViewport_t>(0x1da8bf0)(g_gfxState, cam, 1, 0.0f, 1.0f);
					WriteInverseProj(g_gfxState, cam);  // after the commit (see note above)
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
					// sunExecEnabled isolates the sun draw alone. sunEnabled is not a
					// valid isolation for it — that flag gates this whole block, which
					// also owns the accum clear, the accum/DS binds, the camera state
					// and the pre-resolve G-buffer rebind, and turning it off faults
					// the delivery outright (the ImageSpace copy then runs with no
					// camera state). Everything above stays; only the fullscreen
					// additive BSDFLightDir exec below is skipped.
					if (cfgClean && cfgBuilt && *Settings::sunExecEnabled) {

					// When this runs matters: a deferred directional light shades by
					// sampling the G-buffer, and our G-buffer geometry is drawn inside
					// the resolve, which is called below — exec'ing before it samples
					// the black clear (N.L against a zero normal is zero everywhere).
					// Vanilla fills the G-buffer in the stages before its sun stage
					// (FUN_14284e9e0), then resolves. So the body is captured here and
					// invoked from ResolveAccumBind0Hook — the moment the resolve
					// binds the light-accum buffer, after the G-buffer geometry and
					// before the light volumes.
					const auto runSunExec = [&]() {

					// Render states (dirty-mask at ctx+0x1ee0: |4 = depth-stencil group,
					// |8 = 0xbc group, |0x10 = blend group).
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
						// around the exec, restore after. If a small scale does not dim
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


						// The gate: FUN_142891040 does not unconditionally draw. It is:
						//
						//   ok = (ctx+0x40 == cfg+0x48 && ctx+0x38 == cfg->shader)
						//        || FUN_142891280(cfg+0x48, shader, ctx);   // SetupTechnique
						//   if (ok) { ...SetupGeometry, draw, restore... }
						//   return ok;
						//
						// FUN_142891280 calls shader->vtable[0x20](shader, technique, ctx)
						// and caches (shader, technique) into ctx+0x38/+0x40 on success,
						// clearing them on failure. Our ctx is fresh every render, so
						// +0x38/+0x40 are 0, the fast path can never match, and every
						// frame depends on SetupTechnique succeeding. If it fails, this
						// returns 0 and nothing is drawn at all — hence the return value
						// is recorded, not discarded.



						// The RT manager stages binds and commits them later, and the
						// hook that invokes this sits on the slot-0 staging call —
						// before slot 1 is staged and before the commit — so at this
						// point the G-buffer MRT is still bound. Without a commit of
						// our own the sun would draw into the G-buffer, corrupting
						// albedo/normals for everything downstream. Bind the
						// accumulation MRT and commit it here, the same way the
						// pre-resolve path does.
						{  // accum rebind — always on
							Fn<SetCurRT_t>(0x1db9dd0)(rtm, 0, 0x6a, 3);
							Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, -1, 3);
							Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
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
								FMT_STRING("SUN EXEC {} — FUN_142891040 returned {} (technique 0x{:X}). drew={} gated={}"),
								sunDrew ? "DREW" : "GATED OFF (SetupTechnique refused; nothing was rasterised)",
								static_cast<int>(sunDrew), g_diagSunCfgFlags,
								g_sunDrewCount, g_sunGatedCount);
						}


						if (scaling) {
							std::memcpy(reinterpret_cast<float*>(niLight + 0x16c), savedRGB, sizeof(savedRGB));
						}

						set(0xb0, 0, 4);  // job's own restore
						g_diagSunPass = 1;
					}
					};  // end runSunExec

					g_pendingSunExec = runSunExec;  // fired from inside the resolve
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
			// Ensure the staging inverse-projection is ours for the resolve's own
			// lighting/composite too. The resolve re-commits the camera internally,
			// but the commit never touches staging+0x1d0, so this write survives it.
			if (g_gfxState) {
				Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 1);
				Fn<StateSetCamData_t>(0x1da8c40)(g_gfxState, cam, 0);
				WriteInverseProj(g_gfxState, cam);
			}

			// The resolve builds its render context at entry and captures the
			// current slot-0 RT into ctx+0x54 — the source of the screen-size/UV
			// constants for every pass it draws (lights, composite). Vanilla calls
			// the resolve with the G-buffer bound (the world render precedes it);
			// entering with the accum MRT bound poisons those constants (composite
			// samples out of footprint, garbage UV scaling on screen-space terms).
			// Rebind the G-buffer (no clear) before the call.
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
			// Clamp the two light loop counts the resolve iterates, for the duration
			// of the resolve only. The resolve draws a light volume per entry
			// (cone/sphere geometry); through a narrow frustum any light that
			// intersects at all projects across the entire target, so hundreds of
			// shadowed lights can mean hundreds of full-screen shaded passes.
			// Restored immediately after, before the heartbeat re-reads the true
			// counts — so a clamped run reports its real light count and its clamp.
			const auto   lightsMax = static_cast<std::int32_t>(*Settings::perfLightsMax);
			const bool   clampLights = lightsMax >= 0;
			std::int16_t savedLightsA = 0, savedLightsB = 0;
			auto* const  lightCountA = reinterpret_cast<std::int16_t*>(ssn0 + 0x1a8);
			auto* const  lightCountB = reinterpret_cast<std::int16_t*>(ssn0 + 0x1c0);
			if (clampLights) {
				savedLightsA = *lightCountA;
				savedLightsB = *lightCountB;
				g_armed.lightsA = lightCountA;
				g_armed.lightsASaved = savedLightsA;
				g_armed.lightsB = lightCountB;
				g_armed.lightsBSaved = savedLightsB;
				const auto cap = static_cast<std::int16_t>(lightsMax);
				if (*lightCountA > cap) {
					*lightCountA = cap;
				}
				if (*lightCountB > cap) {
					*lightCountB = cap;
				}
			}
			g_diagLightsClamp = clampLights ? lightsMax : -1;

			// Arm the decal stage for this resolve (fired by the bind hook, before
			// the accum bind, while the G-buffer is still current).
			g_decalStageCam = cam;
			g_pendingDecalStage.store(accumSetup && *Settings::decalStageEnabled);
			g_inOwnResolve.store(accumSetup);
			Fn<DeferredResolve_t>(0x27ff8b0)(cam, g_accum, cullBuf, ssn0, 0x61, 0xc, 0, 1);
			g_inOwnResolve.store(false);
			g_pendingSunExec = nullptr;  // never let a stale closure outlive this frame
			g_pendingDecalStage.store(false);

			if (clampLights) {
				*lightCountA = savedLightsA;
				*lightCountB = savedLightsB;
				g_armed.lightsA = nullptr;
				g_armed.lightsB = nullptr;
			}
			TimerMark(5);  // end "resolve" (G-buffer draw + light volumes + composite)


			// --- sky accumulate + draw (post-resolve) ---
			// Vanilla order: composite into 0x61, then draw sky groups 0x11/0x12/0x13
			// into 0x61 (slot1 = 0x69, DS 0xC, no clears) — sky depth-tests against
			// the world and fills only far pixels (replacing the black pre-clear).
			// Accumulating the roots here (after the resolve) means the fresh passes
			// are drawn only by us and then released by FinishAccum; pre-resolve
			// registration faults (the resolve draws 0x11/9 internally). Must run
			// before FinishAccum (0x281e750 clears every group's pass lists). Ctx is
			// built after the binds: it snapshots the current slot-0 RT for
			// screen-size constants. skyRootMask bisects a faulting root without a
			// rebuild: 1 = sky dome only (Sky+0x8), 2 = sun/cloud only, 3 = both.
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
					// The sky roots are disabled outside the engine's own sky stage —
					// vanilla brackets its sky accumulation with vfunc+0x180(root, 1)
					// ... (root, 0) on each root (FUN_140c875f0's forward pre-pass
					// does exactly this before AccumulateScene). Without the toggle,
					// culling rejects the whole subtree and nothing registers.
					const auto toggleFn = *reinterpret_cast<const std::uintptr_t*>(vtbl + 0x180);
					if (toggleFn < base || toggleFn >= base + 0x0a000000) {
						continue;
					}
					// Also bypass culling for the sky accumulation — the dome's huge
					// multibound fails the default frustum/portal culling. The
					// engine's own forward passes bracket accumulation with
					// cull+0x158 = 1 (accumulate-all mode, per the first-person pass
					// FUN_14284e370); mirror that here.
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
				// uses a sorted pass builder (FUN_14281df50); e400's default order is
				// the known gap if sky objects layer wrongly (sun behind dome etc.).
				Fn<DrawGroupNow_t>(0x281e400)(g_accum, 0x11, skyCtx, 1);
				Fn<DrawGroupNow_t>(0x281e400)(g_accum, 0x12, skyCtx, 0);
				Fn<DrawGroupNow_t>(0x281e400)(g_accum, 0x13, skyCtx, 0);
				Fn<SetCurRT_t>(0x1db9dd0)(rtm, 1, -1, 3);
				Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
				g_diagSkyDrawn = 1;
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
			// Log what the sun pass is actually handed. The NiLight's world rotation
			// rows live at +0x70/+0x80/+0x90 (3 floats each, stride 0x10 — read off
			// TS_BSLight_UpdateVisibilityAndFade's own spot-direction math), and a
			// directional light's direction is a column of that basis. Zeros or
			// garbage here explain a pass that rasterizes but contributes nothing
			// (dot(N, 0) = 0) or NaN (normalize(garbage), 0/0) without a debugger.
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
					// The last two floats of that block are the D3D11_VIEWPORT
					// MinDepth/MaxDepth. D3D11 requires 0 <= min <= max <= 1 and drops
					// the whole RSSetViewports call otherwise (the previous viewport
					// then silently stays in effect) — flag it explicitly rather than
					// decoding float bits out of the diag ints by hand.
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

			// Lens delivery 0x61 -> 0x62 via the vanilla copy (FUN_1427b08c0, effect
			// 0xf) — it is the HDR->display tonemap, not a plain copy. The composite
			// writes linear HDR into 0x61; delivering with the raw
			// ImageSpaceManager::Copy shows un-tonemapped values (a faint/dark lens).
			RENDER_STEP(17);
			// Delivery camera guard: ImageSpace effects select their render camera
			// via the manager's +0x60 byte (FUN_1427b01a0: 0 -> mgr+0x28 [or +0x38
			// in VR], 1 -> the transient slot mgr+0x58 that engine stages own around
			// their own effect renders, e.g. FUN_140c875f0's tail). If a concurrent
			// stage holds the byte when our delivery runs, the tonemap quad renders
			// with a foreign/stale camera -> wrong viewport -> silently draws
			// nothing or garbage over the footprint. Force the normal selector for
			// the duration of our delivery; restore after.
			const auto ismMgr = *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + kIsmInstanceRVA);
			std::uint8_t savedIsmBusy = 0;
			if (ismMgr) {
				savedIsmBusy = *reinterpret_cast<std::uint8_t*>(ismMgr + 0x60);
				*reinterpret_cast<std::uint8_t*>(ismMgr + 0x60) = 0;
				g_armed.ismBusy = reinterpret_cast<std::uint8_t*>(ismMgr + 0x60);
				g_armed.ismBusySaved = savedIsmBusy;
			}
			// Unbind the depth-stencil across the delivery. Step 16 above restores
			// DS logical 1, and the DS translation table at rtm+0x15fc maps 1 ->
			// physical 2: the main VR eye's depth-stencil, stencil-masked to the
			// headset's hidden-area mesh — the delivery quad must not draw with it
			// bound (four-corner cutout).
			//
			// Logical DS 0xA maps to physical -1 in that table = no depth-stencil.
			// Use 0xA, not -1: SelectDS (0x1db9e40) indexes rtm+0x15fc+idx*4
			// unconditionally, with none of the `param_3 == -1` guard that
			// SetCurrentRenderTarget (0x1db9dd0) has, so -1 would read rtm+0x15f8
			// and bind whatever physical index happens to live there.
			constexpr std::int32_t kDS_None = 0xA;
			const bool             unbindDS = *Settings::deliveryUnbindDS;
			{
				// Forensics for the first few renders: what the delivery would have
				// inherited. Cheap and self-limiting.
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
			{
				// Mode 2 (the shipping path) is the only lensMode that reaches
				// delivery — modes 0/1 divert upstream.
				Fn<VanillaLensCopy_t>(0x27b08c0)(0x61, Addr::kRT_ScopeLens, 0);
				// Own delivery pass: composite the engine's reticle + the glass look
				// over the tonemapped picture (LensComposite.cpp). Runs with the
				// delivery's DS unbound and the ISM busy byte forced, same as the
				// tonemap it follows.
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
			}
			if (unbindDS) {
				// Restore exactly what step 16 left bound, so nothing downstream sees
				// a DS we removed.
				Fn<SelectDS_t>(0x1db9e40)(rtm, 1, 3, 0);
				Fn<CommitTargetsAlt_t>(0x1db9f80)(rtm);
			}
			if (ismMgr) {
				*reinterpret_cast<std::uint8_t*>(ismMgr + 0x60) = savedIsmBusy;
				g_armed.ismBusy = nullptr;
			}
			// Mark 7 — end "deliver" (step 16's unbind/FinishAccum + the tonemap
			// copy into the lens).
			TimerMark(7);
			TimersEnd();



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
		// Resolves to +0x65A2AB0 in the live process — Ghidra's DAT_146541ef0 label
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
		//   +1 = 0   stereo master off — every draw becomes mono DrawIndexed and the
		//            VS stereo constant (b8 float = +1 && +2, uploaded by the state
		//            flush from ctx+0x1ec0) goes 0, so no per-instance NDC half-shift.
		//   FUN_141d94c10 rebinds the constant buffers (incl. b8) around both edges.
		// Do not bracket +2 instead: the deferred technique setup (FUN_142918fc0 and
		// ~30 siblings) unconditionally re-writes +2=1 mid-resolve, which composites
		// stereo-instanced with stale view-1 data (a split lens). +1 is never
		// touched by pass setup.
		const auto renderer = REL::Module::get().base() + kRendererRVA;
		using RendererFn_t = void (*)(std::uintptr_t);
		auto* scopePassFlag = reinterpret_cast<std::uint8_t*>(renderer + 4);
		auto* stereoMaster = reinterpret_cast<std::uint8_t*>(renderer + 1);
		const auto savedFlag = *scopePassFlag;
		const auto savedStereo = *stereoMaster;
		// Publish our thread id for the +4-reader hook — concurrent engine threads
		// that call the reader during our bracket must not see scoped mode.
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
			RestoreArmedEngineState();  // undo any bracket the fault skipped
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
		// Every sunPass value transition is logged (rate-limited) — a run of 0s
		// means the engine's sun config was dirty/unbuilt for those frames.
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
			logger::info(
				FMT_STRING("ScopeRender #{}: passes={} sky={} sun={} lensMode={} fillEveryN={} presence={} faulted={}"),
				renders, g_passTotal, g_diagSkyDrawn, g_diagSunPass,
				*Settings::lensMode, *Settings::fillEveryNFrames,
				WidgetPresentable(), g_faulted);

			// The stage stopwatch, on its own line. Every condition that changes the
			// numbers (clamped lights, sample count, discarded frames) prints beside
			// them, so a log line from a bench run carries its own setup.
			if (const auto t = GetStageTimes(); t.cpuSamples != 0) {
				std::string gpu, cpu;
				for (std::size_t i = 0; i < kStageCount; ++i) {
					fmt::format_to(std::back_inserter(gpu), FMT_STRING("{}{}={:.2f}"), i ? " " : "", kStageNames[i], t.gpuMs[i]);
					fmt::format_to(std::back_inserter(cpu), FMT_STRING("{}{}={:.2f}"), i ? " " : "", kStageNames[i], t.cpuMs[i]);
				}
				logger::info(
					FMT_STRING("ScopeRender #{}: PERF n={}gpu/{}cpu disjoint={} lightsClamp={} | GPU {:.2f} ms [{}] | CPU {:.2f} ms [{}]"),
					renders, t.gpuSamples, t.cpuSamples, t.disjoint,
					g_diagLightsClamp,
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


	void CamSmoothReset() noexcept
	{
		g_camSmoothResetReq.store(true);
	}

	// Lens priming. The placement chain converges shortly after load with no
	// aim, but the lens picture only exists after the first pose-gate
	// activation, so the correctly-placed disc would sit black until then.
	// This flag asks the fill hook for one presence-time fill; set on every
	// equip rebaseline so a weapon swap re-primes with the new scope's view.
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
		// May plugin-owned presence show the widget? True once the fit has been
		// applied for the current ScopeParent baseline (or always, when the user
		// runs with widgetFitEnabled=false and wants the vanilla look). Gating
		// presence on this keeps the raw oversized vanilla band from ever being
		// visible on a first-drawn weapon.
		return !*Settings::widgetFitEnabled || g_fitAppliedAtomic.load(std::memory_order_relaxed);
	}

	void PresenceFit()
	{
		// On an engine rewrite of ScopeParent request the probe, wait for it,
		// then apply the fit once per baseline — census-only placement (see
		// ApplyWidgetFit) — and go quiet until the next equip. Live fills keep
		// continuous ownership while actually aiming. Running the fit
		// continuously and ungated would apply the previous weapon's aperture
		// after an equip, with a bound-heuristic placement computed at a hip
		// pose.
		//
		// Don't latch on a decline: the first post-load probe walks fine but can
		// carry one-update-stale world transforms, which the auto-place sanity
		// gate correctly refuses — latching then would hide the good data a few
		// frames later until the first live aim. Latch only when a census
		// placement actually landed, when none is expected (heuristic-only
		// scope, fit/auto-place disabled, faulted probe), or when a bounded
		// retry budget is spent. Placement re-reads world transforms live, so a
		// retry needs a fresh probe only when the face never resolved.
		const auto base = REL::Module::get().base();
		const auto player = *reinterpret_cast<std::uintptr_t*>(base + kPlayerGlobal);
		if (!player) {
			return;
		}
		constexpr std::uint32_t kRetryFrames = 30;  // ~1/3 s at 90 fps
		constexpr std::uint32_t kMaxTries = 8;      // ~2.7 s worst case, then latch
		static bool          s_done = false;
		static bool          s_track = false;  // census landed -> track per frame
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
				s_track = false;
				s_tries = 0;
				s_cooldown = 0;
				g_lensPrimeNeeded.store(true, std::memory_order_relaxed);
				ScopeIdent::Request();
			}
			ScopeIdent::RunIfRequested(player);
			if (s_done || ScopeIdent::ProbePending()) {
				// The disc hangs off PrimaryUIAttachNode, not the weapon, so its
				// correct local offset changes every frame the gun moves — a
				// placement applied once would leave the frozen disc trailing
				// the gun at hip. Once a census placement has landed, keep
				// tracking it per frame: the census target is pure weapon
				// geometry (eye-independent), so it is hip-safe by construction,
				// and the write early-out keeps unchanged frames free.
				if (s_done && s_track) {
					ApplyWidgetFit(player, /*a_censusPlacementOnly=*/true);
				}
				return;
			}
			if (s_cooldown > 0) {
				--s_cooldown;
				return;
			}
			const auto outcome = ApplyWidgetFit(player, /*a_censusPlacementOnly=*/true);
			// Retry only while a census placement is genuinely expected and the
			// machinery that could deliver one is enabled — otherwise this would
			// burn the budget on every equip of a heuristic-only scope, or spin
			// forever with widgetAutoPlace=false (a documented live knob).
			const bool wantCensus = ScopeIdent::CensusFaceExpected() &&
			                        *Settings::widgetFitEnabled &&
			                        *Settings::widgetAutoPlace;
			if (outcome == FitOutcome::kPlacedCensus || !wantCensus) {
				s_done = true;
				s_track = (outcome == FitOutcome::kPlacedCensus);
				s_tries = 0;
				return;
			}
			if (++s_tries >= kMaxTries) {
				warnExhausted = true;
				s_done = true;
				s_track = false;
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
		// Pose-freeze dim: one-shot multiply of the frozen lens picture, applied
		// by the fill hook on the live->frozen edge (pose gate: widget up, eye
		// not at the tube; RT 0x62 persists so the last live picture would
		// otherwise read as live). LensComposite saves/restores all D3D state
		// and invalidates the engine's cached-state block itself.
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


	bool RetryAfterFault()
	{
		if (g_faulted && !g_available) {
			g_faulted = false;
			g_available = true;
			logger::info("ScopeRender: fault latch cleared — retrying own render (retryAfterFault)"sv);
			return true;  // report the clear so the caller's retry cap counts real retries only
		}
		return false;
	}

	bool InOwnResolve()
	{
		return g_inOwnResolve.load();
	}

	// The bullet-hole (screen-space decal) stage. Decals are BSDFDecal objects
	// at SSN+0x218, drawn in vanilla by the dedicated DrawWorld stage
	// FUN_142845cc0, which this render never runs; no BSRenderPass is involved,
	// so there is no pass-lifetime exposure. Points that must hold:
	//  - rebuild the visible list ourselves with the engine's own visitor under
	//    the SSN spin lock (the engine's global list is consumed and freed
	//    before our fill site runs), into a plugin-owned BSScrapArray-shaped
	//    list with capacity pre-reserved >= the SSN count so the visitor's
	//    growth path never fires (growth on a foreign buffer would scrap-free
	//    plugin memory).
	//  - param 2 of the renderer is the texture-batch count from the visitor's
	//    counter, snapshotted before unlock — not the decal count (an overcount
	//    reads past the instance-count array).
	//  - the DrawWorld camera global is bracketed to our scope camera (the
	//    stage's draw-ctx builder and camera-rect write read it).
	//  - pipe-state sandwich: the resolve pokes pending state right before the
	//    hooked bind; the stage's teardown clobbers sctx+0xd0 (2 -> 1). Save
	//    and re-poke {0xa8,0xac,0xb4,0xbc,0xc8,0xd0} with the sun-exec dirty
	//    bits (|4 depth-stencil, |8 0xbc group, |0x10 blend).
	//  - decal refcounts (+8) held across the call (AddDecal parity), engine
	//    release on exit (dec; 0 -> vfunc vtable+8).
	//  - no MRT restore: the stage's exit binding is bitwise the G-buffer set
	//    the resolve left bound, and the resolve's accum binds only touch
	//    slots 0/1 (+unbind 2) afterward — verified from disassembly.
	// The near plane uses our known scopeNearClip rather than walking the VR
	// camera's +0x1a0 frustum pointer (layout differs from flatrim).
	void DecalStageImpl(std::uint32_t& a_n, std::uint32_t& a_batches) noexcept
	{
		const auto base = REL::Module::get().base();
		const auto cam = g_decalStageCam;
		if (!cam) {
			return;
		}
		const auto ssn = *reinterpret_cast<std::uintptr_t*>(base + Addr::kSSNDecalOwnerPtr);
		if (!ssn) {
			return;
		}
		static std::uintptr_t* s_buf = nullptr;
		static std::uint32_t   s_cap = 0;

		float        nrm[3] = { *reinterpret_cast<const float*>(cam + 0x70),
			                    *reinterpret_cast<const float*>(cam + 0x74),
			                    *reinterpret_cast<const float*>(cam + 0x78) };
		const float  nearD = (std::max)(0.1f, static_cast<float>(*Settings::scopeNearClip));
		const float* cp = reinterpret_cast<const float*>(cam + 0xa0);
		float        pt[3] = { cp[0] + nrm[0] * nearD, cp[1] + nrm[1] * nearD, cp[2] + nrm[2] * nearD };
		alignas(16) float plane[8] = {};
		Fn<void (*)(float*, const float*, const float*)>(Addr::kNiPlaneCtor)(plane, nrm, pt);

		// BSScrapArray's capacity field is padded to 8 bytes, so size lives at
		// +0x18 — pack it at +0x14 and the visitor reads its element count from
		// stack garbage past the struct.
		struct ScrapArray
		{
			void*           heap;   // +0x00 ScrapHeap* (null ok)
			std::uintptr_t* data;   // +0x08
			std::uint32_t   cap;    // +0x10
			std::uint32_t   pad14;  // +0x14 (capacity padding)
			std::uint32_t   size;   // +0x18 - the field the visitor/renderer read
			std::uint32_t   pad1c;  // +0x1c
		};
		static_assert(offsetof(ScrapArray, size) == 0x18);
		struct VisitCtx
		{
			void* plane;
			void* list;
			int*  counter;
		};
		ScrapArray list{ nullptr, s_buf, s_cap, 0, 0, 0 };
		int        batches = 0;
		VisitCtx   vc{ plane, &list, &batches };
		g_decalStep.store(1);  // plane built

		// The lock must never be orphaned: a fault while holding it leaves the
		// next shot's AddDecal spinning forever on the game thread. __finally
		// releases on every exit path, fault included; the wrapper's __except
		// still latches the stage off.
		auto* lock = reinterpret_cast<volatile long*>(ssn + 0x230);
		int   spins = 0;
		while (_InterlockedExchange(lock, 1) != 0) {
			if (++spins > 10000) {
				::Sleep(1);
			} else {
				::Sleep(0);
			}
		}
		int batchCount = 0;
		__try {
			g_decalStep.store(2);  // locked
			const auto arrData = *reinterpret_cast<std::uintptr_t*>(ssn + 0x218);
			const auto arrCnt = *reinterpret_cast<std::uint32_t*>(ssn + 0x228);
			if (arrData && arrCnt) {
				if (arrCnt > s_cap) {
					std::free(s_buf);
					s_cap = arrCnt + 32;
					s_buf = static_cast<std::uintptr_t*>(std::malloc(sizeof(std::uintptr_t) * s_cap));
					if (!s_buf) {
						s_cap = 0;
					}
					list.data = s_buf;
					list.cap = s_cap;
				}
				if (list.data) {
					for (std::uint32_t i = 0; i < arrCnt; ++i) {
						if (const auto d = *reinterpret_cast<std::uintptr_t*>(arrData + 8ull * i)) {
							Fn<int (*)(void*, std::uintptr_t)>(Addr::kDecalCullVisitor)(&vc, d);
						}
					}
					g_decalStep.store(3);  // visited
					for (std::uint32_t i = 0; i < list.size; ++i) {
						_InterlockedIncrement(reinterpret_cast<volatile long*>(list.data[i] + 8));
					}
					g_decalStep.store(4);  // refs held
					batchCount = batches;
				}
			}
		} __finally {
			_InterlockedExchange(lock, 0);
		}

		a_n = list.size;
		a_batches = batchCount > 0 ? static_cast<std::uint32_t>(batchCount) : 0u;
		if (list.size != 0 && batchCount > 0) {
			const auto    ctxA = g_ctxPtrA ? *reinterpret_cast<std::uintptr_t*>(g_ctxPtrA) : 0;
			const auto    ctxB = g_ctxPtrB ? *reinterpret_cast<std::uintptr_t*>(g_ctxPtrB) : 0;
			const auto    sctx = (ctxA ? ctxA : ctxB) + 0x1ee0;
			const bool    haveSctx = sctx != 0x1ee0;
			std::uint32_t saved[6] = {};
			static constexpr std::uintptr_t kOffs[6] = { 0xa8, 0xac, 0xb4, 0xbc, 0xc8, 0xd0 };
			static constexpr std::uint32_t  kBits[6] = { 4, 4, 4, 8, 0x10, 0x10 };
			if (haveSctx) {
				for (int i = 0; i < 6; ++i) {
					saved[i] = *reinterpret_cast<const std::uint32_t*>(sctx + kOffs[i]);
				}
			}
			auto*      camGlobal = reinterpret_cast<std::uintptr_t*>(base + Addr::kDrawWorldCameraPtr);
			const auto savedCam = *camGlobal;
			*camGlobal = cam;
			// A fault in the render call must not skip the camera-global restore
			// (leaving the engine's DrawWorld camera pointed at our scope camera)
			// or the decal refcount release (leaking every visited decal).
			// __finally runs them on every exit; the wrapper's __except still
			// latches the stage off.
			__try {
				g_decalStep.store(5);  // render call
				Fn<void (*)(void*, std::uint32_t, std::uint32_t)>(Addr::kDeferredDecalRender)(
					&list, static_cast<std::uint32_t>(batchCount), 0);
				g_decalStep.store(6);  // rendered
			} __finally {
				*camGlobal = savedCam;
				if (haveSctx) {
					auto* dirty = reinterpret_cast<std::uint32_t*>(sctx);
					for (int i = 0; i < 6; ++i) {
						auto* f = reinterpret_cast<std::uint32_t*>(sctx + kOffs[i]);
						if (*f != saved[i]) {
							*f = saved[i];
							*dirty |= kBits[i];
						}
					}
				}
				for (std::uint32_t i = 0; i < list.size; ++i) {
					const auto d = list.data[i];
					if (_InterlockedDecrement(reinterpret_cast<volatile long*>(d + 8)) == 0) {
						const auto vt = *reinterpret_cast<std::uintptr_t*>(d);
						reinterpret_cast<void (*)(std::uintptr_t)>(
							*reinterpret_cast<std::uintptr_t*>(vt + 8))(d);
					}
				}
				g_decalStep.store(7);  // refs released
			}
		}
	}

	void RunPendingDecalStage() noexcept
	{
		if (!g_pendingDecalStage.exchange(false)) {
			return;
		}
		static bool s_dead = false;
		if (s_dead) {
			return;
		}
		std::uint32_t n = 0, b = 0;
		bool          faulted = false;
		g_decalStep.store(0);
		__try {
			DecalStageImpl(n, b);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			faulted = true;
		}
		g_diagDecalN = n;
		g_diagDecalBatches = b;
		if (faulted) {
			s_dead = true;
			logger::warn(FMT_STRING("DECAL STAGE faulted at step {} (0=entry 1=plane 2=locked "
			                        "3=visited 4=refs 5=render 6=rendered 7=released) - "
			                        "disabled for this session"),
				g_decalStep.load());
			return;
		}
		// Log whenever the drawn/total counts change — diagnosis needs
		// drew-vs-SSN-total per fill, not just the first run.
		static std::uint32_t s_lastN = 0xffffffffu, s_lastTotal = 0xffffffffu;
		std::uint32_t        total = 0;
		const auto           ssnG = *reinterpret_cast<std::uintptr_t*>(
            REL::Module::get().base() + Addr::kSSNDecalOwnerPtr);
		if (ssnG) {
			total = *reinterpret_cast<const std::uint32_t*>(ssnG + 0x228);
		}
		if (n != s_lastN || total != s_lastTotal) {
			s_lastN = n;
			s_lastTotal = total;
			logger::info(FMT_STRING("DECAL STAGE: drew {}/{} decal(s) in {} batch(es)"), n, total, b);
		}
	}

	// Fire the deferred sun exec from inside the resolve. Called by
	// ResolveAccumBind0Hook right after the light-accum buffer is bound — after
	// the G-buffer geometry has been drawn and before the resolve's own light
	// volumes. One-shot: the slot is cleared before invoking, so a re-entrant
	// or repeated bind cannot draw twice.
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

	// --- DevBench surface ---


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

		d.invProjRejects = g_invProjRejects;
		d.fogNulls = g_diagFogNulls;

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
