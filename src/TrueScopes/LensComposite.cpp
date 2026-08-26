#include "TrueScopes/LensComposite.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include "Settings/Settings.h"
#include "TrueScopes/ScopeIdent.h"

namespace TrueScopes::LensComposite
{
	namespace
	{
		// engine ground truth
		constexpr std::uintptr_t kD3DContextRVA = 0x6235ab0;      // ID3D11DeviceContext* (immediate)
		// Ghidra labels these DAT_145a66b58/b68, but that high-.data label is
		// section-shifted (a live read there returns garbage). True addresses
		// RIP-decoded from the code bytes of GetByName (base+0xb00270):
		// lock 0x5ac8c00, array ptr 0x5ac8c08, count 0x5ac8c18.
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
		// NiNode (VR layout): children ptr +0x168, loop bound u16 +0x172 (the
		// slot high-water mark NiNode::GetObjectByName itself iterates; null
		// holes are legal and skipped). +0x174 is the non-null element count and
		// undercounts across holes - don't use it as a loop bound.
		// NiObjectNET name +0x10; NiAVObject flags +0x108, bit 0 = hidden.
		constexpr std::uintptr_t kNodeChildren = 0x168;
		constexpr std::uintptr_t kNodeChildCount = 0x172;
		constexpr std::uintptr_t kObjName = 0x10;
		constexpr std::uintptr_t kObjFlags = 0x108;

		template <class F>
		F Fn(std::uintptr_t a_rva) noexcept
		{
			return reinterpret_cast<F>(REL::Module::get().base() + a_rva);
		}

		// shaders
		// One triangle covers the clip-space square; uv derived from the vertex id.
		constexpr const char kHLSL[] = R"(
cbuffer Params : register(b0)
{
    float4 reticle;   // x scaleX, y scaleY, z offX, w offY  (lens-uv -> reticle-uv)
    float4 vignette;  // x inner radius, y outer radius, z strength, w edge-darken power
    float4 tint;      // rgb multiplier, w exposure
    float4 flags;     // x reticle alpha (0 = off), y screen mode (1 = electro-optical), z nv strength, w recon strength
    float4 nvp;       // x nv gain, y screen aspect (h/w, 1 = square), z nv scanlines, w reticle eye-box follow
    float4 eyebox;    // xy eye lateral offset (disc units, already gain-scaled), z strength, w residual scatter floor
    float4 fx;        // x edge blur strength, y edge blur start radius, z CA strength, w sheen dark boost
    // v0.2.123 glass suite - unified layout (hand-mirrored in C++ Params; zero = all off):
    float4 pose;      // xy RAW eye lateral offset (disc units, UN-gained); zw smoothed parallax UV shift
    float4 rim;       // x band start (disc units), y band end, z strength, w top bias (center drop)
    float4 sheen;     // x glint strength, y glint width (disc units), z fresnel strength, w smudge amount
    float4 sheen2;    // x glint travel, y smudge scale, z reticle parallax fraction, w rim parallax
    float4 glass2;    // x residual brightness-adapt scale, yzw reserved
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
    // v0.2.123 parallax depth: picture samples shift by pose.zw (CPU-smoothed,
    // clamped) while every rim-anchored term (cuv/r, vignette, eye-box, rim
    // shadow, reticle at fraction 0) stays on geometric UV - the image plane
    // reads DEEPER than the lens plane. (0,0) = bit-exact identity.
    float2 puv = i.uv + pose.zw;

    float3 c;
    // --- chromatic fringe at the rim (optical tubes only; a screen is a display)
    // Real glass focuses red and blue at slightly different radii; sample the
    // channels at radially split UVs, amount growing with r^2 so the centre
    // stays untouched. fx.z is small (0.002-0.01 in UV terms at the rim).
    if (fx.z > 0.0 && flags.y == 0.0) {
        float2 k = cuv * (fx.z * r * r);
        c.r = picture.Sample(smp, puv - k).r;
        c.g = picture.Sample(smp, puv).g;
        c.b = picture.Sample(smp, puv + k).b;
    } else {
        c = picture.Sample(smp, puv).rgb;
    }

    // --- edge blur (optical tubes only): field curvature softens the rim.
    // Four taps on a small ring, blended in from fx.y (start radius) to the edge.
    if (fx.x > 0.0 && flags.y == 0.0 && r > fx.y) {
        float  w  = saturate((r - fx.y) / max(1.0 - fx.y, 0.05)) * saturate(fx.x);
        float  br = 0.006 * w;                    // tap ring radius in UV
        float3 b = picture.Sample(smp, puv + float2( br,  br)).rgb
                 + picture.Sample(smp, puv + float2(-br,  br)).rgb
                 + picture.Sample(smp, puv + float2( br, -br)).rgb
                 + picture.Sample(smp, puv + float2(-br, -br)).rgb;
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

    // --- eye-box / exit pupil (optical tubes only), BEFORE the reticle as of
    // v0.2.129: the PICTURE clips fully as approved, but the reticle gets its
    // own, weaker response (real-scope observation 2026-08-25: the reticle
    // stays faintly visible inside the blacked-out eye box, silhouetted
    // against residual scatter - which the dark-adaptive sheen now provides).
    // nvp.w = reticleEyeBoxFollow: 1 = reticle clips with the picture (the
    // old behavior), 0 = reticle immune. eyebox.xy is the eye's real lateral
    // offset from the tube axis in disc units (computed per frame on the CPU,
    // gain-scaled). The exit pupil SHIFTS OPPOSITE the eye and SHRINKS as the
    // eye moves off axis; on axis it is larger than the picture = no-op.
    float ebMul = 1.0;
    if (eyebox.z > 0.0 && flags.y == 0.0) {
        float2 p   = cuv * 2.0;
        float  m   = length(eyebox.xy);
        float  rad = 1.15 - 0.9 * saturate(m);
        float  vis = 1.0 - smoothstep(rad - 0.25, rad + 0.15, length(p + eyebox.xy));
        ebMul = lerp(1.0, vis, saturate(eyebox.z));
    }
    // v0.2.130: the clip bottoms out at a faint FLAT scatter (eyebox.w), not
    // pure black - what a dark reticle silhouettes against in a real tube.
    // v0.2.131: the scatter ADAPTS to scene brightness (field concern: a
    // constant glow at night reads as a screen again). Residual scatter IS
    // scene light diffused in the tube, so scale it by the picture's average
    // luminance - five FIXED taps, identical for every pixel, so no scene
    // structure can leak into the glow; it only breathes with overall
    // brightness. glass2.x = 0 restores the constant v0.2.130 behavior.
    float resid = eyebox.w;
    if (resid > 0.0 && glass2.x > 0.0 && ebMul < 0.999) {
        float3 lw = float3(0.299, 0.587, 0.114);
        float  al = (dot(picture.Sample(smp, float2(0.50, 0.50)).rgb, lw)
                   + dot(picture.Sample(smp, float2(0.32, 0.32)).rgb, lw)
                   + dot(picture.Sample(smp, float2(0.68, 0.32)).rgb, lw)
                   + dot(picture.Sample(smp, float2(0.32, 0.68)).rgb, lw)
                   + dot(picture.Sample(smp, float2(0.68, 0.68)).rgb, lw)) * 0.2;
        resid *= saturate(al * glass2.x);
    }
    c = c * ebMul + float3(0.92, 1.0, 0.97) * (resid * (1.0 - ebMul));

    if (flags.x > 0.0) {
        float2 ruv = (i.uv + pose.zw * sheen2.z - 0.5) * reticle.xy + 0.5 + reticle.zw;
        if (all(ruv >= 0.0) && all(ruv <= 1.0)) {
            float4 rc = reticleT.Sample(smp, ruv);
            c = lerp(c, rc.rgb, saturate(rc.a * flags.x) * lerp(1.0, ebMul, saturate(nvp.w)));
        }
    }

    // --- v0.2.123 HOUSING RIM SHADOW (optical tubes only): a hard near-black
    // annulus that reads as the ocular tube WALL - distinct from the soft
    // photographic vignette above. Last in the multiply chain: the wall
    // occludes picture, phosphor, and reticle alike. Two asymmetries make it
    // 3D: a top bias (real tube interiors are lit from above; note it rides
    // the disc, so it cants with the weapon) and a parallax shift opposite
    // the eye (the rim is NEAR geometry against the far image). The combined
    // center shift is clamped so the band can never slide off one side.
    if (rim.z > 0.0 && flags.y == 0.0) {
        float2 ctr = float2(0.0, rim.w) - pose.xy * sheen2.w;
        float  cl  = 1.0 - rim.x;
        float  cm  = length(ctr);
        if (cm > cl && cm > 0.0) { ctr *= cl / cm; }
        float rr = length(cuv * 2.0 - ctr);
        c *= 1.0 - saturate(rim.z) * smoothstep(rim.x, max(rim.y, rim.x + 0.001), rr);
    }

    // --- v0.2.123 GLASS SHEEN: light on the eyepiece surface, in FRONT of
    // everything - strictly additive, so it never occludes. A broad Gaussian
    // glint at the mirror point of the eye offset (head right -> glint left),
    // modulated by a fixed procedural smudge pattern (smudge only CARVES the
    // glint, never brightens; invisible except where the glint rakes it - the
    // strongest "physical surface" cue), plus a tiny fresnel rim lift that
    // grows off-axis. Applies to screen optics too: their front window is
    // still glass. Faint AR-coating green tint.
    if (sheen.x > 0.0 || sheen.z > 0.0) {
        float2 eo  = pose.xy;
        float2 p2  = cuv * 2.0;
        float2 hc  = clamp(-eo * sheen2.x, -0.85, 0.85);
        float  gd  = length(p2 - hc) / max(sheen.y, 0.05);
        float  gl  = exp(-gd * gd);
        float  smu = 1.0;
        if (sheen.w > 0.0) {
            float2 q = cuv * sheen2.y;
            float  n = sin(q.x * 5.1 + sin(q.y * 3.7)) * sin(q.y * 4.3 + sin(q.x * 6.1))
                     + 0.5 * sin(q.x * 11.3 + q.y * 7.7) * sin(q.y * 9.1 - q.x * 3.3);
            smu = max(0.0, 1.0 - sheen.w * (0.5 - 0.33 * n));
        }
        float fr = sheen.z * pow(saturate(r), 3.0) * (0.35 + 0.65 * saturate(length(eo)));
        // v0.2.126 DARK-ADAPTIVE (field 2026-08-25: "the overall screen when
        // showing black just looks like a screen and not glass"): real glass
        // shows its surface reflections against a DARK scene and loses them
        // against a bright one. Boost the whole glass term as the composed
        // picture darkens - the eye-box clip and frozen dim stop reading as a
        // dead LCD. fx.w = 0 keeps this off (Dim constraint).
        float lum = dot(c, float3(0.299, 0.587, 0.114));
        float dk  = 1.0 + fx.w * saturate(1.0 - lum * 2.0);
        c += float3(0.92, 1.0, 0.97) * (sheen.x * gl * smu + fr) * dk;
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
			// glass suite - mirrors the HLSL cbuffer above exactly.
			// Zero-init = every effect off (the Dim() constraint).
			float pose[4];
			float rim[4];
			float sheen[4];
			float sheen2[4];
			float glass2[4];
		};

		// eye-box pose
		// The eye's lateral offset from the scope tube's axis, in disc-radius
		// units, measured at the ScopeParent disc. Offsets shared with
		// ScopeRender.cpp's DeriveScopeFovDegrees (which documents where each
		// was read out of the VR binary):
		//   PlayerCamera singleton  [base+0x5930608], camera root at +0x20
		//   NiAVObject world rotate +0x70 (NiMatrix3 = 3 rows of NiPoint4),
		//   world translate +0xa0, world scale +0xac
		// ScopeParent's local axes: X = right across the lens, Y = the tube axis
		// (down-range), Z = up. local = R^T * (world - T): local_c = sum_r
		// rot[r][c] * v[r], rows 0x10 apart.
		// Returns false (offsets zeroed) when any pointer is missing or the
		// numbers are not sane - the shader then behaves as eye-on-axis.
		// parallax EMA state (render thread only).
		float         g_pllxPrev[2] = {};
		std::uint64_t g_pllxLastMs = 0;

		bool EyeLateral(std::uintptr_t a_scopeParent, float& a_x, float& a_y, float& a_axialY) noexcept
		{
			a_x = a_y = 0.0f;
			a_axialY = 0.0f;
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
			// The camera root is the HMD centre, between the eyes - measured from
			// it the aiming eye reads ~half an IPD off axis (~2.2 units against a
			// ~1.3-unit disc radius) and the shadow can never centre. So compute
			// both eye positions - camera ± half-IPD along the head's own X axis -
			// and use whichever sits closer to the tube axis: that picks the eye
			// the player is actually aiming with, no dominance setting needed.
			//
			// The head's world X axis: with the convention R[r][c] = m[c*4 + r],
			// world_dir(local X)[r] = R[r][0] = m[r] - the first three floats of
			// the raw matrix, directly.
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
				// (same transposed-on-read convention as ScopeIdent::OcularFaceWorld).
				const float lx = rot[0] * v[0] + rot[1] * v[1] + rot[2] * v[2];
				const float lz = rot[8] * v[0] + rot[9] * v[1] + rot[10] * v[2];
				// tube-axial component (local +Y = down-range) of the eye
				// position - the eye sits behind the ocular, so ly < 0 and
				// eye relief L = -ly. Parallax scales by D/(L+D).
				const float ly = rot[4] * v[0] + rot[5] * v[1] + rot[6] * v[2];
				if (!std::isfinite(lx) || !std::isfinite(lz) || !std::isfinite(ly)) {
					continue;
				}
				const float m2 = lx * lx + lz * lz;
				if (best < 0.0f || m2 < best) {
					best = m2;
					// mesh X = right = screen +u; mesh Z = up = screen -v
					a_x = lx / discR;
					a_y = -lz / discR;
					a_axialY = ly;
				}
			}
			return best >= 0.0f && std::fabs(a_x) < 50.0f && std::fabs(a_y) < 50.0f;
		}

		using D3DCompile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

		// state
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
			// slot+0 is not a texture pointer (it reads 0 live). Take the RTV (+8)
			// and SRV (+0x10) and derive the texture from the RTV. The ref from
			// GetResource is released by the caller after use.
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
			// snapshot once: load() reassigns the std::string on every scope-in
			// while this runs on the render thread - a narrow but real UAF.
			// Changing reticleRendererName requires a restart.
			static const std::string want = *Settings::reticleRendererName;
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
				// only NiNodes have a children array; a leaf shape's +0x168 is past
				// its size (BSTriShape is ~0x160), so gate the recursion on the
				// vtable test (ScopeIdent::IsNiNode) - reading the children fields
				// off a leaf is garbage.
				if (a_depth < 1 && ScopeIdent::IsNiNode(c)) {
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

	namespace
	{
		// The shared draw core: copy the lens picture to the scratch, upload the
		// given constants, draw the fullscreen triangle back into the lens RT with
		// exact state save/restore, then invalidate the engine's cached-state
		// block. Shared by Run() and Dim(). Does not release a_lens.tex and does
		// not touch the run counters; the caller owns both.
		bool Execute(
			ID3D11DeviceContext*      a_d3d,
			const PhysRT&             a_lens,
			ID3D11ShaderResourceView* a_reticleSRV,
			const Params&             a_p,
			const Inputs&             a_in) noexcept
		{
			// 1. picture -> scratch
			a_d3d->CopyResource(g_scratch, a_lens.tex);

			// 2. constants
			{
				D3D11_MAPPED_SUBRESOURCE m{};
				if (SUCCEEDED(a_d3d->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
					std::memcpy(m.pData, &a_p, sizeof(a_p));
					a_d3d->Unmap(g_cb, 0);
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

			a_d3d->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRTV, &oldDSV);
			a_d3d->RSGetViewports(&oldVPCount, oldVP);
			a_d3d->VSGetShader(&oldVS, nullptr, nullptr);
			a_d3d->PSGetShader(&oldPS, nullptr, nullptr);
			a_d3d->GSGetShader(&oldGS, nullptr, nullptr);
			a_d3d->HSGetShader(&oldHS, nullptr, nullptr);
			a_d3d->DSGetShader(&oldDS, nullptr, nullptr);
			a_d3d->IAGetInputLayout(&oldIL);
			a_d3d->IAGetPrimitiveTopology(&oldTopo);
			a_d3d->PSGetShaderResources(0, 2, oldSRV);
			a_d3d->PSGetSamplers(0, 1, &oldSamp);
			a_d3d->PSGetConstantBuffers(0, 1, &oldCB);
			a_d3d->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
			a_d3d->RSGetState(&oldRaster);
			a_d3d->OMGetDepthStencilState(&oldDSS, &oldStencilRef);

			// 4. our draw
			{
				// the picture must not be bound as an SRV while we render into it
				ID3D11ShaderResourceView* nulls[2] = {};
				a_d3d->PSSetShaderResources(0, 2, nulls);
				ID3D11RenderTargetView* rtvs[1] = { a_lens.rtv };
				a_d3d->OMSetRenderTargets(1, rtvs, nullptr);
				D3D11_VIEWPORT vp{};
				vp.Width = static_cast<float>(g_scratchW);
				vp.Height = static_cast<float>(g_scratchH);
				vp.MaxDepth = 1.0f;
				a_d3d->RSSetViewports(1, &vp);
				a_d3d->IASetInputLayout(nullptr);
				a_d3d->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				a_d3d->VSSetShader(g_vs, nullptr, 0);
				a_d3d->GSSetShader(nullptr, nullptr, 0);
				a_d3d->HSSetShader(nullptr, nullptr, 0);
				a_d3d->DSSetShader(nullptr, nullptr, 0);
				a_d3d->PSSetShader(g_ps, nullptr, 0);
				ID3D11ShaderResourceView* srvs[2] = { g_scratchSRV, a_reticleSRV };
				a_d3d->PSSetShaderResources(0, 2, srvs);
				a_d3d->PSSetSamplers(0, 1, &g_sampler);
				a_d3d->PSSetConstantBuffers(0, 1, &g_cb);
				const float bf[4] = { 0, 0, 0, 0 };
				a_d3d->OMSetBlendState(g_blend, bf, 0xffffffff);
				a_d3d->RSSetState(g_raster);
				a_d3d->OMSetDepthStencilState(g_dss, 0);
				a_d3d->Draw(3, 0);
			}

			// 5. restore + release the refs Get* handed us
			{
				ID3D11ShaderResourceView* nulls[2] = {};
				a_d3d->PSSetShaderResources(0, 2, nulls);
			}
			a_d3d->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRTV, oldDSV);
			a_d3d->RSSetViewports(oldVPCount, oldVP);
			a_d3d->VSSetShader(oldVS, nullptr, 0);
			a_d3d->PSSetShader(oldPS, nullptr, 0);
			a_d3d->GSSetShader(oldGS, nullptr, 0);
			a_d3d->HSSetShader(oldHS, nullptr, 0);
			a_d3d->DSSetShader(oldDS, nullptr, 0);
			a_d3d->IASetInputLayout(oldIL);
			a_d3d->IASetPrimitiveTopology(oldTopo);
			a_d3d->PSSetShaderResources(0, 2, oldSRV);
			a_d3d->PSSetSamplers(0, 1, &oldSamp);
			a_d3d->PSSetConstantBuffers(0, 1, &oldCB);
			a_d3d->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
			a_d3d->RSSetState(oldRaster);
			a_d3d->OMSetDepthStencilState(oldDSS, oldStencilRef);
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
			return true;
		}
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
			} else if (g_hiddenQuad) {
				// if the ScopeMenu RT stops resolving after the vanilla quad was
				// hidden, no reticle would composite and the vanilla quad would
				// stay invisible - restore it so there is a reticle either way.
				RestoreReticleQuad();
			}
		} else if (g_hiddenQuad) {
			RestoreReticleQuad();
		}

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
		// modes from the probed zoom identity (ScopeIdent): screen look for
		// the recon widget branch (overlay 16) or a Screen* measured shape;
		// NV / recon color from the zoom's imod.
		bool screenMode = false;
		{
			const auto ident = ScopeIdent::Get();
			const bool screen = ident.zoomOverlay == 16 ||
			                    std::strncmp(ident.faceShape, "Screen", 6) == 0;
			screenMode = screen;
			p.flags[1] = screen ? 1.0f : 0.0f;
			p.flags[2] = (ident.zoomImodID & 0xFFFFFF) == 0x94636
			                 ? static_cast<float>(*Settings::nvEffectStrength)
			                 : 0.0f;
			p.flags[3] = (ident.zoomImodID & 0xFFFFFF) == 0x2041b6
			                 ? static_cast<float>(*Settings::reconEffectStrength)
			                 : 0.0f;
			p.nvp[0] = static_cast<float>(*Settings::nvGain);
			// rectangular screens (MGScopeThermal is 2.55 x 1.37): the
			// aperture is the screen's half-width, and the shader crops
			// the vertical overshoot to aspect = h/w. 1.0 (every circular
			// optic and square screen) is a no-op.
			const float aspect = ident.screenAspect;
			p.nvp[1] = (aspect > 0.05f && aspect <= 1.0f) ? aspect : 1.0f;
			p.nvp[2] = static_cast<float>(*Settings::nvScanlines);
			p.nvp[3] = std::clamp(static_cast<float>(*Settings::reticleEyeBoxFollow), 0.0f, 1.0f);
		}
		// glass effects: eye-box from the real head-to-scope pose, edge
		// blur, chromatic fringe. The shader applies all three only to
		// optical tubes (screen mode skips them - an LCD has no exit
		// pupil and no field curvature).
		{
			float ex = 0.0f, ey = 0.0f, ly = 0.0f;
			const auto strength = static_cast<float>(*Settings::eyeBoxStrength);
			// the pose read feeds four consumers - eye-box (gain-scaled), rim
			// parallax, sheen, and parallax depth (all raw, so the glass effects
			// never retune when eyeBoxGain is calibrated). Run it when any of
			// them wants it; each consumer gates itself below.
			const float rimStr = (std::max)(0.0f, static_cast<float>(*Settings::rimShadowStrength));
			const float rimPll = static_cast<float>(*Settings::rimShadowParallax);
			const float shStr = (std::max)(0.0f, static_cast<float>(*Settings::sheenStrength));
			const float shFre = (std::max)(0.0f, static_cast<float>(*Settings::sheenFresnel));
			const float pllxD = static_cast<float>(*Settings::parallaxDepthUnits);
			const bool wantPose = strength > 0.0f || shStr > 0.0f || shFre > 0.0f ||
			                      pllxD > 0.0f || (rimStr > 0.0f && rimPll != 0.0f);
			const bool havePose = wantPose && EyeLateral(a_in.scopeParent, ex, ey, ly);
			if (havePose && strength > 0.0f) {
				const auto gain = static_cast<float>(*Settings::eyeBoxGain);
				p.eyebox[0] = ex * gain;
				p.eyebox[1] = ey * gain;
				p.eyebox[2] = strength;
				p.eyebox[3] = (std::max)(0.0f, static_cast<float>(*Settings::eyeBoxResidual));
				p.glass2[0] = (std::max)(0.0f, static_cast<float>(*Settings::eyeBoxResidualAdapt));
			}
			if (havePose) {
				p.pose[0] = ex;
				p.pose[1] = ey;
			}
			// parallax depth (optical tubes only; an LCD sits at the housing).
			// Image plane D units behind the lens; shift = -0.5*D/(L+D)*offset,
			// L = live eye relief (a pistol at arm's length shows less than a
			// shouldered rifle). EMA across fills absorbs the auto-eye flip; a
			// >500 ms gap snaps so nothing stale slides in on scope-in; a pose
			// failure decays toward zero through the same EMA instead of
			// popping. The clamp keeps clamp-sampler smear under the rim band.
			{
				float tx = 0.0f, ty = 0.0f;
				if (havePose && pllxD > 0.0f && !screenMode && std::isfinite(ly) && ly < 0.0f) {
					const float L = (std::max)(static_cast<float>(*Settings::parallaxMinEyeRelief), -ly);
					const float d = pllxD / (L + pllxD);
					tx = -0.5f * d * ex;
					ty = -0.5f * d * ey;
					const float m = std::sqrt(tx * tx + ty * ty);
					const float cap = static_cast<float>(*Settings::parallaxMaxShift);
					if (m > cap && m > 0.0f) {
						tx *= cap / m;
						ty *= cap / m;
					}
				}
				const auto now = ::GetTickCount64();
				const float sm = (std::clamp)(static_cast<float>(*Settings::parallaxSmoothing), 0.0f, 0.95f);
				if (now - g_pllxLastMs > 500) {
					g_pllxPrev[0] = tx;
					g_pllxPrev[1] = ty;
				} else {
					g_pllxPrev[0] += (tx - g_pllxPrev[0]) * (1.0f - sm);
					g_pllxPrev[1] += (ty - g_pllxPrev[1]) * (1.0f - sm);
				}
				g_pllxLastMs = now;
				p.pose[2] = g_pllxPrev[0];
				p.pose[3] = g_pllxPrev[1];
				p.sheen2[2] = static_cast<float>(*Settings::reticleParallaxFraction);
			}
			// rim shadow: fill whenever on - without pose it degrades to the
			// static top-biased ring (the shader clamps the center).
			if (rimStr > 0.0f) {
				p.rim[0] = static_cast<float>(*Settings::rimShadowStart);
				p.rim[1] = static_cast<float>(*Settings::rimShadowEnd);
				p.rim[2] = rimStr;
				p.rim[3] = (std::clamp)(static_cast<float>(*Settings::rimShadowTopBias), -0.25f, 0.25f);
				p.sheen2[3] = rimPll;
			}
			// glass sheen: pose-gated - a static centred glint would read as a
			// bug, so the whole block turns off when the pose read fails.
			if (havePose && (shStr > 0.0f || shFre > 0.0f)) {
				p.sheen[0] = shStr;
				p.sheen[1] = static_cast<float>(*Settings::sheenWidth);
				p.sheen[2] = shFre;
				p.sheen[3] = (std::max)(0.0f, static_cast<float>(*Settings::sheenSmudge));
				p.sheen2[0] = static_cast<float>(*Settings::sheenTravel);
				p.sheen2[1] = static_cast<float>(*Settings::sheenSmudgeScale);
			}
			p.fx[0] = static_cast<float>(*Settings::edgeBlurStrength);
			p.fx[1] = static_cast<float>(*Settings::edgeBlurStart);
			p.fx[2] = static_cast<float>(*Settings::caStrength) * 0.02f;
			p.fx[3] = (std::max)(0.0f, static_cast<float>(*Settings::sheenDarkBoost));
		}

		Execute(d3d, lens, reticleSRV, p, a_in);
		lens.tex->Release();
		++g_diag.runs;
		return true;
	}

	bool Dim(const Inputs& a_in, float a_factor) noexcept
	{
		// One-shot multiply on the frozen lens picture (pose freeze: the fill
		// stops, RT 0x62 persists, and this keeps the stale picture from reading
		// as live). A neutral parameter set turns the composite shader into
		// `picture * factor`: no reticle, optical path with every effect at zero,
		// vignette strength 0 - only the unconditional tint multiply acts.
		// Applied once per freeze edge by the caller, so nothing compounds.
		const auto base = REL::Module::get().base();
		auto* const d3d = *reinterpret_cast<ID3D11DeviceContext**>(base + kD3DContextRVA);
		if (!d3d || !EnsureStatic(d3d)) {
			return false;
		}
		const auto lens = LookupRT(a_in.renderer, a_in.rtm, static_cast<std::int32_t>(a_in.lensLogicalRT));
		if (!lens.tex || !lens.rtv) {
			return false;
		}
		if (!EnsureScratch(lens.tex)) {
			lens.tex->Release();
			return false;
		}
		Params p{};
		p.tint[0] = p.tint[1] = p.tint[2] = a_factor;
		p.tint[3] = 1.0f;
		p.nvp[1] = 1.0f;  // aspect no-op (screen path is off anyway)
		Execute(d3d, lens, nullptr, p, a_in);
		lens.tex->Release();
		++g_diag.dims;
		return true;
	}
}
