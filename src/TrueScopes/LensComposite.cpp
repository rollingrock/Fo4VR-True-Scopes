#include "TrueScopes/LensComposite.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include "Settings/Settings.h"
#include "TrueScopes/ScopeIdent.h"

namespace TrueScopes::LensComposite
{
	namespace
	{
		// --- engine ground truth (all live-verified 2026-08-23, see the session doc) ---
		constexpr std::uintptr_t kD3DContextRVA = 0x6235ab0;      // ID3D11DeviceContext* (immediate)
		// Ghidra labels DAT_145a66b58/b68, but that high-.data label is SECTION-SHIFTED
		// (live read there returned garbage). True addresses RIP-decoded from the live
		// code bytes of GetByName (base+0xb00270) 2026-08-23: lock 0x5ac8c00,
		// array ptr 0x5ac8c08, count 0x5ac8c18 — and verified by enumerating 36
		// renderers with sane names ('ScopeMenu' at index 26, customRT 124/swap 125).
		constexpr std::uintptr_t kRendererArrayRVA = 0x5ac8c08;   // Interface3D::Renderer* []
		constexpr std::uintptr_t kRendererCountRVA = 0x5ac8c18;   // u32 count
		constexpr std::uintptr_t kRendererName = 0x220;           // BSFixedString (pool entry ptr) — GetByName compares it
		constexpr std::uintptr_t kRendererEnabled = 0x5c;
		constexpr std::uintptr_t kRendererCustomRT = 0x1d8;       // customRenderTarget (HUD-glassed) — int32 logical
		constexpr std::uintptr_t kRendererSwapRT = 0x1dc;         // customSwapTarget (raw Scaleform) — int32 logical
		constexpr std::uintptr_t kPoolEntryChars = 0x18;          // BSStringPool::Entry: chars start here
		constexpr std::uintptr_t kRTLogicalToPhys = 0x13bc;       // rtm + 0x13bc + logical*4 -> physical (int32)
		constexpr std::uintptr_t kPhysRTTable = 0xa70;            // renderer + 0xa70 + phys*0x30 = {tex, RTV, SRV, ...}
		constexpr std::uintptr_t kPhysRTStride = 0x30;
		constexpr std::uintptr_t kStateBlock = 0x1ee0;            // ctx + 0x1ee0: BSGraphics cached-state block
		constexpr std::uintptr_t kStateBlockInvalidate = 0x1da6170;  // TS_BSGraphics_StateBlock_Invalidate(block)
		constexpr std::uintptr_t kRendererRebindCBs = 0x1d94c10;     // FUN_141d94c10(renderer)
		// NiNode (VR layout): children ptr +0x168, count u16 +0x174; NiObjectNET name +0x10;
		// NiAVObject flags +0x108, bit 0 = hidden.
		constexpr std::uintptr_t kNodeChildren = 0x168;
		constexpr std::uintptr_t kNodeChildCount = 0x174;
		constexpr std::uintptr_t kObjName = 0x10;
		constexpr std::uintptr_t kObjFlags = 0x108;

		template <class F>
		F Fn(std::uintptr_t a_rva) noexcept
		{
			return reinterpret_cast<F>(REL::Module::get().base() + a_rva);
		}

		// ------------------------------------------------------------------ shaders
		// One triangle covers the clip-space square; uv derived from the vertex id.
		constexpr const char kHLSL[] = R"(
cbuffer Params : register(b0)
{
    float4 reticle;   // x scaleX, y scaleY, z offX, w offY  (lens-uv -> reticle-uv)
    float4 vignette;  // x inner radius, y outer radius, z strength, w edge-darken power
    float4 tint;      // rgb multiplier, w exposure
    float4 flags;     // x reticle alpha (0 = off), y screen mode (1 = electro-optical), z nv strength, w recon strength
    float4 nvp;       // x nv gain, y screen aspect (h/w, 1 = square), z nv scanlines, w unused
    float4 eyebox;    // xy eye lateral offset (disc units, already gain-scaled), z strength, w unused
    float4 fx;        // x edge blur strength, y edge blur start radius, z CA strength, w unused
};
Texture2D    picture  : register(t0);
Texture2D    reticleT : register(t1);
SamplerState smp      : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 cuv = i.uv - 0.5;
    float  r   = length(cuv) * 2.0;      // disc units: 1.0 at the picture edge

    float3 c;
    // --- chromatic fringe at the rim (optical tubes only; a screen is a display)
    // Real glass focuses red and blue at slightly different radii; sample the
    // channels at radially split UVs, amount growing with r^2 so the centre
    // stays untouched. fx.z is small (0.002-0.01 in UV terms at the rim).
    if (fx.z > 0.0 && flags.y == 0.0) {
        float2 k = cuv * (fx.z * r * r);
        c.r = picture.Sample(smp, i.uv - k).r;
        c.g = picture.Sample(smp, i.uv).g;
        c.b = picture.Sample(smp, i.uv + k).b;
    } else {
        c = picture.Sample(smp, i.uv).rgb;
    }

    // --- edge blur (optical tubes only): field curvature softens the rim.
    // Four taps on a small ring, blended in from fx.y (start radius) to the edge.
    if (fx.x > 0.0 && flags.y == 0.0 && r > fx.y) {
        float  w  = saturate((r - fx.y) / max(1.0 - fx.y, 0.05)) * saturate(fx.x);
        float  br = 0.006 * w;                    // tap ring radius in UV
        float3 b = picture.Sample(smp, i.uv + float2( br,  br)).rgb
                 + picture.Sample(smp, i.uv + float2(-br,  br)).rgb
                 + picture.Sample(smp, i.uv + float2( br, -br)).rgb
                 + picture.Sample(smp, i.uv + float2(-br, -br)).rgb;
        c = lerp(c, b * 0.25, w);
    }

    c *= tint.rgb * tint.w;

    // --- in-lens recreations of the suppressed fullscreen zoom imods ---------
    // numbers from the IMAD records themselves (see Settings.h)
    if (flags.z > 0.0) {           // zd_ScopeNightVision
        c *= nvp.x;                                       // adaptation gain
        c = c * 1.1 + 0.4 * float3(0.05, 0.08, 0.05);     // brightness/contrast lift
        float l = dot(c, float3(0.299, 0.587, 0.114));
        float3 nv = l * float3(0.31, 0.816, 0.216) * 2.2; // green phosphor
        c = lerp(c, nv, 0.686 * saturate(flags.z));
        // phosphor scanlines: subtle horizontal banding, image-intensifier flavor
        if (nvp.z > 0.0) {
            c *= 1.0 - nvp.z * 0.5 * (0.5 + 0.5 * sin(i.uv.y * 620.0));
        }
    }
    if (flags.w > 0.0) {           // zd_ScopeTargetingRecon
        float l = dot(c, float3(0.299, 0.587, 0.114));
        float3 flat3 = lerp(l.xxx, c, 0.3);               // desaturate to 0.3
        flat3 = flat3 * 0.6 + 0.25;                       // flatten contrast
        flat3 *= float3(1.0, 0.878, 0.835);               // warm screen tint
        c = lerp(c, flat3, saturate(flags.w));
    }

    float v;
    if (flags.y > 0.0) {
        // screen-type optic: square soft edge instead of the radial vignette.
        // A rectangular display (aspect = h/w < 1) is delivered with the
        // aperture sized to its half-WIDTH, so the vertical edge arrives at
        // "aspect" instead of 1 -- dividing y by it reuses the same ramp.
        float2 d = abs(i.uv - 0.5) * 2.0;
        d.y /= max(nvp.y, 0.05);
        float2 e = 1.0 - smoothstep(0.86, 1.02, d);
        v = e.x * e.y;
        c *= lerp(1.0, v, 0.85);
        // The 0.85 edge leaves a 15% floor -- right where a SQUARE screen's
        // edge meets its bezel (VR-verified), wrong past a RECTANGLE's crop
        // line, which lies INSIDE the quad on housing pixels. Full crop there,
        // and only there: squares (aspect 1) never take this branch.
        if (nvp.y < 0.999) {
            c *= 1.0 - smoothstep(0.98, 1.06, d.y);
        }
    } else {
        // radial term in "disc units": 1.0 at the picture disc's edge
        v = 1.0 - smoothstep(vignette.x, vignette.y, r);
        v = pow(saturate(v), max(vignette.w, 0.001));
        c *= lerp(1.0, v, saturate(vignette.z));
    }

    if (flags.x > 0.0) {
        float2 ruv = (i.uv - 0.5) * reticle.xy + 0.5 + reticle.zw;
        if (all(ruv >= 0.0) && all(ruv <= 1.0)) {
            float4 rc = reticleT.Sample(smp, ruv);
            c = lerp(c, rc.rgb, saturate(rc.a * flags.x));
        }
    }

    // --- eye-box / exit pupil (optical tubes only), AFTER the reticle: the
    // whole sight picture lives behind the eyepiece, so moving the head off the
    // tube axis clips picture and reticle together. eyebox.xy is the eye's real
    // lateral offset from the tube axis in disc units (computed per frame on the
    // CPU from ScopeParent's world transform and the camera root, gain-scaled).
    // The exit pupil is a disc that SHIFTS OPPOSITE the eye and SHRINKS as the
    // eye moves off axis; on axis (offset 0) the pupil is larger than the
    // picture and this is a no-op.
    if (eyebox.z > 0.0 && flags.y == 0.0) {
        float2 p   = cuv * 2.0;
        float  m   = length(eyebox.xy);
        float  rad = 1.15 - 0.9 * saturate(m);
        float  vis = 1.0 - smoothstep(rad - 0.25, rad + 0.15, length(p + eyebox.xy));
        c *= lerp(1.0, vis, saturate(eyebox.z));
    }
    return float4(c, 1.0);
}
)";

		struct alignas(16) Params
		{
			float reticle[4];
			float vignette[4];
			float tint[4];
			float flags[4];
			float nvp[4];
			float eyebox[4];
			float fx[4];
		};

		// --- eye-box pose (v0.2.113) ------------------------------------------
		// The eye's lateral offset from the scope tube's axis, in disc-radius
		// units, measured at the ScopeParent disc. Ground truth shared with
		// ScopeRender.cpp's DeriveScopeFovDegrees (which documents where each
		// offset was read out of the VR binary):
		//   PlayerCamera singleton  [base+0x5930608], camera root at +0x20
		//   NiAVObject world rotate +0x70 (NiMatrix3 = 3 rows of NiPoint4),
		//   world translate +0xa0, world scale +0xac
		// ScopeParent's local axes: X = right across the lens, Y = the tube axis
		// (down-range), Z = up. local = R^T * (world - T): local_c = sum_r
		// rot[r][c] * v[r], rows 0x10 apart.
		// Returns false (offsets zeroed) when any pointer is missing or the
		// numbers are not sane — the shader then behaves as eye-on-axis.
		bool EyeLateral(std::uintptr_t a_scopeParent, float& a_x, float& a_y) noexcept
		{
			a_x = a_y = 0.0f;
			if (!a_scopeParent) {
				return false;
			}
			const auto playerCam = *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + 0x5930608);
			if (!playerCam) {
				return false;
			}
			const auto camRoot = *reinterpret_cast<std::uintptr_t*>(playerCam + 0x20);
			if (!camRoot) {
				return false;
			}
			const auto* rot = reinterpret_cast<const float*>(a_scopeParent + 0x70);
			const auto* disc = reinterpret_cast<const float*>(a_scopeParent + 0xa0);
			const float scale = *reinterpret_cast<const float*>(a_scopeParent + 0xac);
			const auto* camPos = reinterpret_cast<const float*>(camRoot + 0xa0);
			// The camera root is the HMD CENTRE, between the eyes — measured from
			// it, the aiming eye reads ~half an IPD off axis (~2.2 units against a
			// ~1.3-unit disc radius) and the shadow can never centre (first
			// in-headset report, 2026-08-24). So compute BOTH eye positions —
			// camera ± half-IPD along the head's own X axis — and use whichever
			// sits closer to the tube axis: that auto-selects the eye the player
			// is actually aiming with, with no dominance setting to configure.
			//
			// The head's world X axis: with the verified convention R[r][c] =
			// m[c*4 + r], world_dir(local X)[r] = R[r][0] = m[r] — i.e. the first
			// three floats of the raw matrix, directly.
			const auto* camRot = reinterpret_cast<const float*>(camRoot + 0x70);
			const float halfIpd = 0.5f * static_cast<float>(*Settings::eyeBoxIpdUnits);
			const float discR = 7.852f * scale;  // vanilla render_circle radius at scale 1
			if (!(discR > 0.001f)) {
				return false;
			}
			float best = -1.0f;
			for (const float side : { -1.0f, 1.0f }) {
				const float e[3] = { camPos[0] + side * halfIpd * camRot[0],
					                 camPos[1] + side * halfIpd * camRot[1],
					                 camPos[2] + side * halfIpd * camRot[2] };
				const float v[3] = { e[0] - disc[0], e[1] - disc[1], e[2] - disc[2] };
				// local = R^T * v: local_c = sum_r R[r][c]*v[r] = raw row c dot v
				// (same transposed-on-read convention ScopeIdent::OcularFaceWorld
				// live-verified 2026-08-11).
				const float lx = rot[0] * v[0] + rot[1] * v[1] + rot[2] * v[2];
				const float lz = rot[8] * v[0] + rot[9] * v[1] + rot[10] * v[2];
				if (!std::isfinite(lx) || !std::isfinite(lz)) {
					continue;
				}
				const float m2 = lx * lx + lz * lz;
				if (best < 0.0f || m2 < best) {
					best = m2;
					// mesh X = right = screen +u; mesh Z = up = screen -v
					a_x = lx / discR;
					a_y = -lz / discR;
				}
			}
			return best >= 0.0f && std::fabs(a_x) < 50.0f && std::fabs(a_y) < 50.0f;
		}

		using D3DCompile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

		// ------------------------------------------------------------------ state
		ID3D11Device*             g_device = nullptr;
		ID3D11VertexShader*       g_vs = nullptr;
		ID3D11PixelShader*        g_ps = nullptr;
		ID3D11Buffer*             g_cb = nullptr;
		ID3D11SamplerState*       g_sampler = nullptr;
		ID3D11BlendState*         g_blend = nullptr;
		ID3D11RasterizerState*    g_raster = nullptr;
		ID3D11DepthStencilState*  g_dss = nullptr;
		ID3D11Texture2D*          g_scratch = nullptr;
		ID3D11ShaderResourceView* g_scratchSRV = nullptr;
		std::uint32_t             g_scratchW = 0, g_scratchH = 0;
		DXGI_FORMAT               g_scratchFmt = DXGI_FORMAT_UNKNOWN;
		bool                      g_initTried = false;
		bool                      g_ready = false;
		Diag                      g_diag{};
		std::uintptr_t            g_hiddenQuad = 0;  // render_UI:0 we set hidden

		template <class T>
		void SafeRelease(T*& a_p) noexcept
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}

		bool Compile(const char* a_entry, const char* a_target, ID3DBlob** a_out) noexcept
		{
			static D3DCompile_t compile = nullptr;
			if (!compile) {
				if (auto mod = ::LoadLibraryW(L"d3dcompiler_47.dll")) {
					compile = reinterpret_cast<D3DCompile_t>(::GetProcAddress(mod, "D3DCompile"));
				}
				if (!compile) {
					logger::error("LensComposite: d3dcompiler_47.dll / D3DCompile unavailable"sv);
					return false;
				}
			}
			ID3DBlob* err = nullptr;
			const auto hr = compile(kHLSL, sizeof(kHLSL) - 1, "TrueScopesLens", nullptr, nullptr, a_entry, a_target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out, &err);
			if (FAILED(hr)) {
				logger::error(FMT_STRING("LensComposite: {} compile failed hr=0x{:08X}: {}"), a_entry, static_cast<std::uint32_t>(hr),
					err ? static_cast<const char*>(err->GetBufferPointer()) : "(no message)");
			}
			SafeRelease(err);
			return SUCCEEDED(hr);
		}

		bool EnsureStatic(ID3D11DeviceContext* a_ctx) noexcept
		{
			if (g_initTried) {
				return g_ready;
			}
			g_initTried = true;
			a_ctx->GetDevice(&g_device);
			if (!g_device) {
				logger::error("LensComposite: no ID3D11Device"sv);
				return false;
			}
			ID3DBlob* vs = nullptr;
			ID3DBlob* ps = nullptr;
			if (!Compile("VSMain", "vs_5_0", &vs) || !Compile("PSMain", "ps_5_0", &ps)) {
				SafeRelease(vs);
				SafeRelease(ps);
				return false;
			}
			HRESULT hr = g_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
			if (SUCCEEDED(hr)) {
				hr = g_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps);
			}
			SafeRelease(vs);
			SafeRelease(ps);
			if (FAILED(hr)) {
				logger::error(FMT_STRING("LensComposite: shader object creation failed hr=0x{:08X}"), static_cast<std::uint32_t>(hr));
				return false;
			}

			D3D11_BUFFER_DESC cbd{};
			cbd.ByteWidth = sizeof(Params);
			cbd.Usage = D3D11_USAGE_DYNAMIC;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			hr = g_device->CreateBuffer(&cbd, nullptr, &g_cb);

			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			if (SUCCEEDED(hr)) {
				hr = g_device->CreateSamplerState(&sd, &g_sampler);
			}

			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].BlendEnable = FALSE;
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (SUCCEEDED(hr)) {
				hr = g_device->CreateBlendState(&bd, &g_blend);
			}

			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = FALSE;
			rd.ScissorEnable = FALSE;
			if (SUCCEEDED(hr)) {
				hr = g_device->CreateRasterizerState(&rd, &g_raster);
			}

			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = FALSE;
			dd.StencilEnable = FALSE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			if (SUCCEEDED(hr)) {
				hr = g_device->CreateDepthStencilState(&dd, &g_dss);
			}
			if (FAILED(hr)) {
				logger::error(FMT_STRING("LensComposite: state object creation failed hr=0x{:08X}"), static_cast<std::uint32_t>(hr));
				return false;
			}
			g_ready = true;
			g_diag.ready = true;
			logger::info("LensComposite: shaders + states ready"sv);
			return true;
		}

		bool EnsureScratch(ID3D11Texture2D* a_like) noexcept
		{
			D3D11_TEXTURE2D_DESC d{};
			a_like->GetDesc(&d);
			if (g_scratch && g_scratchW == d.Width && g_scratchH == d.Height && g_scratchFmt == d.Format) {
				return true;
			}
			SafeRelease(g_scratchSRV);
			SafeRelease(g_scratch);
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			d.MiscFlags = 0;
			d.CPUAccessFlags = 0;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.MipLevels = 1;
			if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_scratch))) {
				logger::error(FMT_STRING("LensComposite: scratch CreateTexture2D failed ({}x{} fmt {})"), d.Width, d.Height, static_cast<int>(d.Format));
				return false;
			}
			D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
			sv.Format = d.Format;
			sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sv.Texture2D.MipLevels = 1;
			if (FAILED(g_device->CreateShaderResourceView(g_scratch, &sv, &g_scratchSRV))) {
				SafeRelease(g_scratch);
				logger::error("LensComposite: scratch SRV failed"sv);
				return false;
			}
			g_scratchW = d.Width;
			g_scratchH = d.Height;
			g_scratchFmt = d.Format;
			logger::info(FMT_STRING("LensComposite: scratch {}x{} fmt {}"), d.Width, d.Height, static_cast<int>(d.Format));
			return true;
		}

		struct PhysRT
		{
			ID3D11Texture2D*          tex;
			ID3D11RenderTargetView*   rtv;
			ID3D11ShaderResourceView* srv;
			std::int32_t              phys;
		};

		PhysRT LookupRT(std::uintptr_t a_renderer, std::uintptr_t a_rtm, std::int32_t a_logical) noexcept
		{
			PhysRT r{};
			r.phys = -1;
			if (a_logical < 0 || a_logical > 0x100) {
				return r;
			}
			const auto phys = *reinterpret_cast<const std::int32_t*>(a_rtm + kRTLogicalToPhys + static_cast<std::uintptr_t>(a_logical) * 4);
			if (phys < 0 || phys > 0x200) {
				return r;
			}
			const auto slot = a_renderer + kPhysRTTable + static_cast<std::uintptr_t>(phys) * kPhysRTStride;
			// slot+0 is NOT a texture pointer (0 live; the v0.2.104.0 bug — every
			// render skipped). Take the RTV (+8) / SRV (+0x10) and derive the texture
			// from the RTV like DumpLogicalRT does. The ref from GetResource is
			// released by the caller (Run) after use.
			r.rtv = *reinterpret_cast<ID3D11RenderTargetView**>(slot + 8);
			r.srv = *reinterpret_cast<ID3D11ShaderResourceView**>(slot + 0x10);
			if (r.rtv) {
				ID3D11Resource* res = nullptr;
				r.rtv->GetResource(&res);
				r.tex = static_cast<ID3D11Texture2D*>(res);
			}
			r.phys = phys;
			return r;
		}

		// Find the ScopeMenu Interface3D renderer by name and return the logical RT
		// that holds the reticle. Logs every renderer name on the first miss so a
		// renamed menu is diagnosable from the log alone.
		std::int32_t FindReticleRT() noexcept
		{
			const auto base = REL::Module::get().base();
			const auto arr = *reinterpret_cast<const std::uintptr_t*>(base + kRendererArrayRVA);
			const auto count = *reinterpret_cast<const std::uint32_t*>(base + kRendererCountRVA);
			if (!arr || count == 0 || count > 64) {
				return -1;
			}
			const auto& want = *Settings::reticleRendererName;
			static bool loggedList = false;
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto rdr = *reinterpret_cast<const std::uintptr_t*>(arr + static_cast<std::uintptr_t>(i) * 8);
				if (!rdr) {
					continue;
				}
				const auto entry = *reinterpret_cast<const std::uintptr_t*>(rdr + kRendererName);
				const char* name = entry ? reinterpret_cast<const char*>(entry + kPoolEntryChars) : "";
				if (!loggedList) {
					logger::info(FMT_STRING("LensComposite: Interface3D renderer[{}] '{}' enabled={} customRT={} swapRT={}"), i, name,
						*reinterpret_cast<const std::uint8_t*>(rdr + kRendererEnabled),
						*reinterpret_cast<const std::int32_t*>(rdr + kRendererCustomRT),
						*reinterpret_cast<const std::int32_t*>(rdr + kRendererSwapRT));
				}
				if (want == name) {
					std::snprintf(g_diag.rendererName, sizeof(g_diag.rendererName), "%s", name);
					const auto glassed = *reinterpret_cast<const std::int32_t*>(rdr + kRendererCustomRT);
					const auto raw = *reinterpret_cast<const std::int32_t*>(rdr + kRendererSwapRT);
					loggedList = true;
					return *Settings::reticleUseGlassed ? (glassed >= 0 ? glassed : raw) : (raw >= 0 ? raw : glassed);
				}
			}
			loggedList = true;
			return -1;
		}

		// Walk ScopeParent's subtree for `render_UI:0`; returns the NiAVObject.
		std::uintptr_t FindQuad(std::uintptr_t a_node, int a_depth) noexcept
		{
			if (!a_node || a_depth > 6) {
				return 0;
			}
			const auto children = *reinterpret_cast<const std::uintptr_t*>(a_node + kNodeChildren);
			const auto count = *reinterpret_cast<const std::uint16_t*>(a_node + kNodeChildCount);
			if (!children || count > 64) {
				return 0;
			}
			for (std::uint16_t i = 0; i < count; ++i) {
				const auto c = *reinterpret_cast<const std::uintptr_t*>(children + static_cast<std::uintptr_t>(i) * 8);
				if (!c) {
					continue;
				}
				const auto entry = *reinterpret_cast<const std::uintptr_t*>(c + kObjName);
				if (entry && std::strcmp(reinterpret_cast<const char*>(entry + kPoolEntryChars), "render_UI:0") == 0) {
					return c;
				}
				// only NiNodes have a children array; leaf shapes' +0x168 is past their
				// size (BSTriShape is 0x160-ish) — guard by the parent's own walk depth
				// and by the fact that world_scope.nif's shapes sit directly under one
				// node. We only descend into the first level's nodes.
				if (a_depth < 1) {
					if (const auto r = FindQuad(c, a_depth + 1)) {
						return r;
					}
				}
			}
			return 0;
		}

		void HideReticleQuad(std::uintptr_t a_scopeParent) noexcept
		{
			if (!a_scopeParent) {
				return;
			}
			const auto q = FindQuad(a_scopeParent, 0);
			if (!q) {
				return;
			}
			auto& flags = *reinterpret_cast<std::uint8_t*>(q + kObjFlags);
			if (!(flags & 1)) {
				flags |= 1;
			}
			g_hiddenQuad = q;
			g_diag.quadHidden = true;
		}
	}

	void RestoreReticleQuad() noexcept
	{
		if (g_hiddenQuad) {
			// The engine may have destroyed the rig since; only touch it if it still
			// looks like our quad (name check) — the rig is recreated on every equip.
			__try {
				const auto entry = *reinterpret_cast<const std::uintptr_t*>(g_hiddenQuad + kObjName);
				if (entry && std::strcmp(reinterpret_cast<const char*>(entry + kPoolEntryChars), "render_UI:0") == 0) {
					*reinterpret_cast<std::uint8_t*>(g_hiddenQuad + kObjFlags) &= static_cast<std::uint8_t>(~1u);
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			g_hiddenQuad = 0;
		}
		g_diag.quadHidden = false;
	}

	Diag GetDiag() noexcept
	{
		return g_diag;
	}

	bool Run(const Inputs& a_in) noexcept
	{
		const auto base = REL::Module::get().base();
		auto* const d3d = *reinterpret_cast<ID3D11DeviceContext**>(base + kD3DContextRVA);
		if (!d3d || !EnsureStatic(d3d)) {
			++g_diag.skips;
			return false;
		}
		const auto lens = LookupRT(a_in.renderer, a_in.rtm, static_cast<std::int32_t>(a_in.lensLogicalRT));
		if (!lens.tex || !lens.rtv) {
			++g_diag.skips;
			return false;
		}
		if (!EnsureScratch(lens.tex)) {
			lens.tex->Release();
			++g_diag.skips;
			return false;
		}

		// Reticle source (previous frame's Scaleform render — same latency vanilla has).
		// The reticle lookup only needs the SRV; release the GetResource ref at once.
		ID3D11ShaderResourceView* reticleSRV = nullptr;
		const bool                wantReticle = *Settings::reticleEnabled;
		if (wantReticle) {
			const auto logical = FindReticleRT();
			g_diag.reticleRT = logical;
			auto rt = LookupRT(a_in.renderer, a_in.rtm, logical);
			if (rt.tex) {
				rt.tex->Release();
				rt.tex = nullptr;
			}
			g_diag.reticlePhys = rt.phys;
			reticleSRV = rt.srv;
			if (reticleSRV) {
				HideReticleQuad(a_in.scopeParent);
			}
		} else if (g_hiddenQuad) {
			RestoreReticleQuad();
		}

		// 1. picture -> scratch
		d3d->CopyResource(g_scratch, lens.tex);

		// 2. constants
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(d3d->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				Params p{};
				p.reticle[0] = static_cast<float>(*Settings::reticleScaleX);
				p.reticle[1] = static_cast<float>(*Settings::reticleScaleY);
				p.reticle[2] = static_cast<float>(*Settings::reticleOffsetX);
				p.reticle[3] = static_cast<float>(*Settings::reticleOffsetY);
				p.vignette[0] = static_cast<float>(*Settings::vignetteInner);
				p.vignette[1] = static_cast<float>(*Settings::vignetteOuter);
				p.vignette[2] = static_cast<float>(*Settings::vignetteStrength);
				p.vignette[3] = static_cast<float>(*Settings::vignettePower);
				p.tint[0] = static_cast<float>(*Settings::lensTintR);
				p.tint[1] = static_cast<float>(*Settings::lensTintG);
				p.tint[2] = static_cast<float>(*Settings::lensTintB);
				p.tint[3] = static_cast<float>(*Settings::lensExposure);
				p.flags[0] = (wantReticle && reticleSRV) ? static_cast<float>(*Settings::reticleAlpha) : 0.0f;
				// v0.2.111 — modes from the probed zoom identity (ScopeIdent):
				// screen look for the recon widget branch (overlay 16) or a
				// Screen* measured shape; NV / recon color from the zoom's imod.
				{
					const auto ident = ScopeIdent::Get();
					const bool screen = ident.zoomOverlay == 16 ||
					                    std::strncmp(ident.faceShape, "Screen", 6) == 0;
					p.flags[1] = screen ? 1.0f : 0.0f;
					p.flags[2] = (ident.zoomImodID & 0xFFFFFF) == 0x94636
					                 ? static_cast<float>(*Settings::nvEffectStrength)
					                 : 0.0f;
					p.flags[3] = (ident.zoomImodID & 0xFFFFFF) == 0x2041b6
					                 ? static_cast<float>(*Settings::reconEffectStrength)
					                 : 0.0f;
					p.nvp[0] = static_cast<float>(*Settings::nvGain);
					// v0.2.112 — rectangular screens (MGScopeThermal is 2.55 x
					// 1.37): the aperture is the screen's half-WIDTH, and the
					// shader crops the vertical overshoot to aspect = h/w.
					// 1.0 (every circular optic and square screen) is a no-op.
					const float aspect = ident.screenAspect;
					p.nvp[1] = (aspect > 0.05f && aspect <= 1.0f) ? aspect : 1.0f;
					p.nvp[2] = static_cast<float>(*Settings::nvScanlines);
				}
				// v0.2.113 — the glass effects (SESSION_2026-08-23_RETICLE_AND_
				// GLASS.md §4 step 3, the deferred half): eye-box from the real
				// head-to-scope pose, edge blur, chromatic fringe. The shader
				// applies all three only to optical tubes (screen mode skips
				// them — an LCD has no exit pupil and no field curvature).
				{
					float ex = 0.0f, ey = 0.0f;
					const auto strength = static_cast<float>(*Settings::eyeBoxStrength);
					if (strength > 0.0f && EyeLateral(a_in.scopeParent, ex, ey)) {
						const auto gain = static_cast<float>(*Settings::eyeBoxGain);
						p.eyebox[0] = ex * gain;
						p.eyebox[1] = ey * gain;
						p.eyebox[2] = strength;
					}
					p.fx[0] = static_cast<float>(*Settings::edgeBlurStrength);
					p.fx[1] = static_cast<float>(*Settings::edgeBlurStart);
					p.fx[2] = static_cast<float>(*Settings::caStrength) * 0.02f;
				}
				std::memcpy(m.pData, &p, sizeof(p));
				d3d->Unmap(g_cb, 0);
			}
		}

		// 3. save every piece of state we touch
		ID3D11RenderTargetView*   oldRTV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView*   oldDSV = nullptr;
		D3D11_VIEWPORT            oldVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		UINT                      oldVPCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		ID3D11VertexShader*       oldVS = nullptr;
		ID3D11PixelShader*        oldPS = nullptr;
		ID3D11GeometryShader*     oldGS = nullptr;
		ID3D11HullShader*         oldHS = nullptr;
		ID3D11DomainShader*       oldDS = nullptr;
		ID3D11InputLayout*        oldIL = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY  oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11ShaderResourceView* oldSRV[2] = {};
		ID3D11SamplerState*       oldSamp = nullptr;
		ID3D11Buffer*             oldCB = nullptr;
		ID3D11BlendState*         oldBlend = nullptr;
		float                     oldBlendFactor[4] = {};
		UINT                      oldSampleMask = 0xffffffff;
		ID3D11RasterizerState*    oldRaster = nullptr;
		ID3D11DepthStencilState*  oldDSS = nullptr;
		UINT                      oldStencilRef = 0;

		d3d->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRTV, &oldDSV);
		d3d->RSGetViewports(&oldVPCount, oldVP);
		d3d->VSGetShader(&oldVS, nullptr, nullptr);
		d3d->PSGetShader(&oldPS, nullptr, nullptr);
		d3d->GSGetShader(&oldGS, nullptr, nullptr);
		d3d->HSGetShader(&oldHS, nullptr, nullptr);
		d3d->DSGetShader(&oldDS, nullptr, nullptr);
		d3d->IAGetInputLayout(&oldIL);
		d3d->IAGetPrimitiveTopology(&oldTopo);
		d3d->PSGetShaderResources(0, 2, oldSRV);
		d3d->PSGetSamplers(0, 1, &oldSamp);
		d3d->PSGetConstantBuffers(0, 1, &oldCB);
		d3d->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
		d3d->RSGetState(&oldRaster);
		d3d->OMGetDepthStencilState(&oldDSS, &oldStencilRef);

		// 4. our draw
		{
			// the picture must not be bound as an SRV while we render into it
			ID3D11ShaderResourceView* nulls[2] = {};
			d3d->PSSetShaderResources(0, 2, nulls);
			ID3D11RenderTargetView* rtvs[1] = { lens.rtv };
			d3d->OMSetRenderTargets(1, rtvs, nullptr);
			D3D11_VIEWPORT vp{};
			vp.Width = static_cast<float>(g_scratchW);
			vp.Height = static_cast<float>(g_scratchH);
			vp.MaxDepth = 1.0f;
			d3d->RSSetViewports(1, &vp);
			d3d->IASetInputLayout(nullptr);
			d3d->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			d3d->VSSetShader(g_vs, nullptr, 0);
			d3d->GSSetShader(nullptr, nullptr, 0);
			d3d->HSSetShader(nullptr, nullptr, 0);
			d3d->DSSetShader(nullptr, nullptr, 0);
			d3d->PSSetShader(g_ps, nullptr, 0);
			ID3D11ShaderResourceView* srvs[2] = { g_scratchSRV, reticleSRV };
			d3d->PSSetShaderResources(0, 2, srvs);
			d3d->PSSetSamplers(0, 1, &g_sampler);
			d3d->PSSetConstantBuffers(0, 1, &g_cb);
			const float bf[4] = { 0, 0, 0, 0 };
			d3d->OMSetBlendState(g_blend, bf, 0xffffffff);
			d3d->RSSetState(g_raster);
			d3d->OMSetDepthStencilState(g_dss, 0);
			d3d->Draw(3, 0);
		}

		// 5. restore + release the refs Get* handed us
		{
			ID3D11ShaderResourceView* nulls[2] = {};
			d3d->PSSetShaderResources(0, 2, nulls);
		}
		d3d->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRTV, oldDSV);
		d3d->RSSetViewports(oldVPCount, oldVP);
		d3d->VSSetShader(oldVS, nullptr, 0);
		d3d->PSSetShader(oldPS, nullptr, 0);
		d3d->GSSetShader(oldGS, nullptr, 0);
		d3d->HSSetShader(oldHS, nullptr, 0);
		d3d->DSSetShader(oldDS, nullptr, 0);
		d3d->IASetInputLayout(oldIL);
		d3d->IASetPrimitiveTopology(oldTopo);
		d3d->PSSetShaderResources(0, 2, oldSRV);
		d3d->PSSetSamplers(0, 1, &oldSamp);
		d3d->PSSetConstantBuffers(0, 1, &oldCB);
		d3d->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
		d3d->RSSetState(oldRaster);
		d3d->OMSetDepthStencilState(oldDSS, oldStencilRef);
		for (auto& v : oldRTV) {
			SafeRelease(v);
		}
		SafeRelease(oldDSV);
		SafeRelease(oldVS);
		SafeRelease(oldPS);
		SafeRelease(oldGS);
		SafeRelease(oldHS);
		SafeRelease(oldDS);
		SafeRelease(oldIL);
		for (auto& v : oldSRV) {
			SafeRelease(v);
		}
		SafeRelease(oldSamp);
		SafeRelease(oldCB);
		SafeRelease(oldBlend);
		SafeRelease(oldRaster);
		SafeRelease(oldDSS);

		// 6. the engine caches what it believes is bound; tell it to forget.
		if (a_in.ctx) {
			Fn<void (*)(std::uintptr_t)>(kStateBlockInvalidate)(a_in.ctx + kStateBlock);
		}
		Fn<void (*)(std::uintptr_t)>(kRendererRebindCBs)(a_in.renderer);

		lens.tex->Release();
		++g_diag.runs;
		return true;
	}
}
