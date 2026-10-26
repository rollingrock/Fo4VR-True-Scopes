#include "TrueScopes/Reticle.h"

#include "Settings/Settings.h"
#include "TrueScopes/Addresses.h"
#include "TrueScopes/Optic.h"
#include "TrueScopes/ReticleFit.h"
#include "TrueScopes/ReticleShader.h"
#include "TrueScopes/ScopeIdent.h"

#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <mutex>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace TrueScopes::Reticle
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using namespace DirectX;
		using ReticleFit::Role;
		using ReticleFit::Vec3;

		// VR NiAVObject / NiNode
		constexpr std::uintptr_t kName = 0x10;  // BSFixedString, NiObjectNET (properties and textures too)
		constexpr std::uintptr_t kParent = 0x28;
		constexpr std::uintptr_t kLocal = 0x30;  // NiTransform: rows +0x00/+0x10/+0x20, translate +0x30, scale +0x3c
		constexpr std::uintptr_t kWorld = 0x70;
		constexpr std::uintptr_t kFlags = 0x108;  // bit 0 app culled
		constexpr std::uintptr_t kChildren = 0x168;
		constexpr std::uintptr_t kChildCount = 0x172;  // u16 loop bound, null holes legal

		// VR BSGeometry / BSTriShape, flat +0x40 (the +0x198 type byte is witnessed at install)
		constexpr std::uintptr_t kAlphaProperty = 0x170;
		constexpr std::uintptr_t kShaderProperty = 0x178;
		constexpr std::uintptr_t kSkinInstance = 0x180;
		constexpr std::uintptr_t kRendererData = 0x188;
		constexpr std::uintptr_t kVertexDesc = 0x190;
		constexpr std::uintptr_t kGeometryType = 0x198;  // 3 BSTriShape, 8 BSSubIndexTriShape
		constexpr std::uintptr_t kTriangleCount = 0x1a0;
		constexpr std::uintptr_t kVertexCount = 0x1a4;
		constexpr std::uintptr_t kIsTriShapeVFunc = 0x50;

		// effect shader property, its material, the alpha property
		constexpr std::uintptr_t kPropertyAlpha = 0x28;  // float
		constexpr std::uintptr_t kPropertyFlags = 0x30;  // bit 6 falloff, 36 two-sided, 37 vertex colours
		constexpr std::uintptr_t kPropertyEffectData = 0x50;
		constexpr std::uintptr_t kPropertyMaterial = 0x58;
		constexpr std::uintptr_t kMaterialUv = 0x0c;  // offset u, v, scale u, v
		constexpr std::uintptr_t kMaterialColor = 0x38;
		constexpr std::uintptr_t kMaterialEmission = 0x74;
		constexpr std::uintptr_t kMaterialTexturePath = 0x78;  // BSFixedString; the NiTexture's own name is empty in VR
		constexpr std::uintptr_t kAlphaFlags = 0x28;           // u16, 0x1 blend, 0x200 test
		constexpr std::uintptr_t kAlphaRef = 0x2a;

		constexpr std::uint64_t kFalloff = 1ull << 6;
		constexpr std::uint64_t kTwoSided = 1ull << 36;
		constexpr std::uint64_t kVertexColours = 1ull << 37;

		// vertex descriptor flags, (desc >> 44)
		constexpr std::uint64_t kVfVertex = 0x1;
		constexpr std::uint64_t kVfUv = 0x2;
		constexpr std::uint64_t kVfColors = 0x20;
		constexpr std::uint64_t kVfSkinned = 0x40;
		constexpr std::uint64_t kVfFullPrec = 0x400;

		// the 0x58-byte render pass (ctor 0x142890350; both allocators and the
		// dispatch 0x142891040 agree)
		constexpr std::uintptr_t kPassShader = 0x08;
		constexpr std::uintptr_t kPassGeometry = 0x18;
		constexpr std::uintptr_t kPassTechnique = 0x48;
		constexpr std::size_t kPassSize = 0x58;
		constexpr std::uint32_t kTexturedTechnique = 4;  // the effect technique builder's source-texture bit

		// a cached batch, and the job that owns its passes
		constexpr std::uintptr_t kBatchJob = 0x00;
		constexpr std::uintptr_t kBatchGroupEnds = 0x08;
		constexpr std::uintptr_t kBatchGroupCount = 0x18;
		constexpr std::uintptr_t kBatchPackets = 0x20;
		constexpr std::uintptr_t kBatchPacketCount = 0x30;
		constexpr std::uintptr_t kJobPasses = 0x3d0;

		// the stereo camera records: eye stride 0x210, view-projection +0xd0;
		// the per-eye floating origin at +0x2590
		constexpr std::uintptr_t kStereoRecords = 0x25d0;
		constexpr std::uintptr_t kStereoOrigins = 0x2590;
		constexpr std::uintptr_t kEyeStride = 0x210;
		constexpr std::uintptr_t kEyeViewProj = 0xd0;

		constexpr std::size_t kMaxReticles = 4;
		constexpr std::size_t kMaxParentDepth = 64;
		constexpr std::size_t kMaxOpticChildren = 96;
		constexpr float kHalfAngle = 0.016f;  // tan of the art's half-height at reticle scale 1
		constexpr std::uint64_t kSampleMaxAgeMs = 100;
		constexpr std::uint64_t kReselectMs = 500;

		// the two hooked entries: displaced prologue bytes, then the rest of the
		// prologue as a witness. Six for DrawPass (a five-byte write would split
		// push r14), five for DrawBatches; nothing RIP-relative moves.
		constexpr std::array<std::uint8_t, 18> kDrawPassEntry{ 0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C,
			0x24, 0xC1, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00 };
		constexpr std::size_t kDrawPassDisplaced = 6;
		constexpr std::array<std::uint8_t, 18> kDrawBatchesEntry{ 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x41, 0x54, 0x41,
			0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30 };
		constexpr std::size_t kDrawBatchesDisplaced = 5;
		constexpr std::array<std::uint8_t, 22> kRunPassEntry{ 0x48, 0x89, 0x74, 0x24, 0x20, 0x88, 0x54, 0x24, 0x10, 0x57,
			0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20 };

		using DrawPass_t = void (*)(void*, void*);
		using DrawBatches_t = void (*)(void*, std::uint8_t, void*);
		using RunPass_t = std::uint8_t (*)(void*, std::uint8_t, void*);
		using RunPackets_t = void (*)(void**, std::uint32_t, void*);
		using ContextFn_t = void (*)(void*);
		using DrawShape_t = void (*)(void*, void*, std::uint32_t, std::uint32_t);

		enum class Stage : std::uint32_t
		{
			kIdle,
			kNativeContext,
			kResources,
			kDepth,
			kTexture,
			kDrawn,
			kAwaitingPass,
			kSampleBusy,
			kNoSample,
			kSampleInvalidated,
			kSampleStale,
			kWrongReticle,
			kAncestorCulled,
			kParentLimit,
			kOtherShader,
			kOpticChanged,
			kQualifying,
			kUnsupported,
			kException,
		};

		const char* StageName(Stage a_stage)
		{
			constexpr const char* kNames[] = { "idle", "native-context", "gpu-resources", "depth-buffer", "reticle-texture",
				"drawn", "awaiting-reticle-pass", "sample-busy", "no-target-sample", "sample-invalidated", "sample-stale",
				"different-reticle-instance", "ancestor-culled", "outside-weapon", "non-effect-pass", "optic-geometry-changed",
				"reading-optic-meshes", "native-unsupported-optic", "render-exception" };
			const auto i = static_cast<std::uint32_t>(a_stage);
			return i < std::size(kNames) ? kNames[i] : "unknown";
		}

		struct Sample
		{
			ReticleFit::Target target;
			std::uint64_t generation;
			std::uint64_t epoch;
			std::uint64_t ms;
			std::uintptr_t root;  // the weapon node; compared, walked to, never owned
			std::array<std::uintptr_t, kMaxReticles> candidates;
			Vec3 bore;
			float halfAngle;
			float brightness;
			bool assist;
			bool emitter;
			float glow, bloom, core, hot, mask;
			float dotHalfAngle;  // a vanilla glow-spot dot's own size; rings and crosshairs take halfAngle
		};

		struct alignas(16) Constants
		{
			XMFLOAT4X4 localToWorld{};
			XMFLOAT4X4 viewProjection[2]{};
			XMFLOAT4 originAdjust[2]{};
			XMFLOAT4 eye[2]{};
			XMFLOAT4 aimForward[2]{};
			XMFLOAT4 aimRight[2]{};
			XMFLOAT4 aimUp[2]{};
			XMFLOAT4 depthPlane{};
			XMFLOAT4 depthRange{};
			XMFLOAT4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };
			XMFLOAT4 uvTransform{ 0.0f, 0.0f, 1.0f, 1.0f };
			XMFLOAT4 visibility{};
			XMFLOAT4 look{};
			XMFLOAT4 look2{};
		};
		static_assert(sizeof(Constants) == 464);

		struct Xform
		{
			float r[3][3];  // rows = axes in the parent
			Vec3 t;
			float s;
		};

		struct MeshIdentity
		{
			std::uintptr_t shape;
			std::uintptr_t data;
			std::uintptr_t material;
			std::uint64_t descriptor;
			std::uint64_t flags;
			std::uint32_t triangles;
			std::uint16_t vertices;
			std::uint16_t alphaFlags;
			Role role;
			Xform local;  // relative to the assembly
		};

		// a reticle card and its optic's shapes, all in the assembly node's frame
		struct Assembly
		{
			std::uintptr_t parent;
			std::array<std::uintptr_t, ReticleFit::kMaxMeshes> shapes;
			std::array<MeshIdentity, ReticleFit::kMaxMeshes> ids;
			std::size_t count;
		};

		// one bounded async copy of a GPU buffer; polling never waits on the GPU
		class Readback
		{
		public:
			static constexpr UINT kMaxBytes = 1024 * 1024;
			enum class Poll
			{
				kPending,
				kReady,
				kFailed,
			};

			bool Begin(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, ID3D11Buffer* a_src, UINT a_offset, UINT a_bytes)
			{
				if (!a_src || !a_bytes || a_bytes > kMaxBytes) {
					return false;
				}
				D3D11_BUFFER_DESC src{};
				a_src->GetDesc(&src);
				if (a_offset > src.ByteWidth || a_bytes > src.ByteWidth - a_offset) {
					return false;
				}
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = a_bytes;
				desc.Usage = D3D11_USAGE_STAGING;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				if (FAILED(a_device->CreateBuffer(&desc, nullptr, m_buffer.ReleaseAndGetAddressOf()))) {
					return false;
				}
				const D3D11_BOX box{ a_offset, 0, 0, a_offset + a_bytes, 1, 1 };
				a_ctx->CopySubresourceRegion(m_buffer.Get(), 0, 0, 0, 0, a_src, 0, &box);
				return true;
			}

			template <class F>
			Poll Inspect(ID3D11DeviceContext* a_ctx, F&& a_inspect)
			{
				if (!m_buffer) {
					return Poll::kFailed;
				}
				D3D11_MAPPED_SUBRESOURCE mapped{};
				const auto hr = a_ctx->Map(m_buffer.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
					return Poll::kPending;
				}
				if (FAILED(hr)) {
					return Poll::kFailed;
				}
				const bool ok = a_inspect(static_cast<const std::byte*>(mapped.pData));
				a_ctx->Unmap(m_buffer.Get(), 0);
				return ok ? Poll::kReady : Poll::kFailed;
			}

			void Reset() { m_buffer.Reset(); }

		private:
			ComPtr<ID3D11Buffer> m_buffer;
		};

		// first-use measure of the bound reticle art: plain, illuminate a faint
		// one, or no usable art (draw a dot). Never waits on the GPU.
		class VisibilityProbe
		{
		public:
			enum class Result
			{
				kPending,
				kOriginal,
				kIlluminate,
				kDot,
				kFailed,
			};
			using Key = std::array<float, 7>;  // effective UV transform and material RGB

			Result Update(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, ID3D11ShaderResourceView* a_src,
				ID3D11ComputeShader* a_shader, ID3D11Buffer* a_constants, ID3D11SamplerState* a_sampler, const Key& a_key)
			{
				if (m_result == Result::kPending && m_queued) {
					D3D11_MAPPED_SUBRESOURCE mapped{};
					const auto hr = a_ctx->Map(m_readback.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
					if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
						return Result::kPending;
					}
					if (FAILED(hr)) {
						m_result = Result::kFailed;
					} else {
						const auto* v = static_cast<const std::uint32_t*>(mapped.pData);
						m_peakAlpha = static_cast<float>(v[2]) / 65535.0f;
						m_result = v[0] == 0 ? Result::kDot :
						                       (static_cast<double>(v[1]) < static_cast<double>(v[0]) * 0.25 || m_peakAlpha < 0.35f ? Result::kIlluminate : Result::kOriginal);
						a_ctx->Unmap(m_readback.Get(), 0);
					}
					m_queued = false;
				}
				if (m_seen && m_source.Get() == a_src && m_key == a_key) {
					return m_result;
				}
				m_seen = true;
				m_source = a_src;
				m_key = a_key;
				m_peakAlpha = 1.0f;
				if (!a_src) {
					return m_result = Result::kDot;
				}
				m_result = Result::kFailed;
				D3D11_SHADER_RESOURCE_VIEW_DESC view{};
				a_src->GetDesc(&view);
				if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
					return m_result;
				}
				ComPtr<ID3D11Resource> resource;
				ComPtr<ID3D11Texture2D> texture;
				a_src->GetResource(resource.GetAddressOf());
				if (!resource || FAILED(resource.As(&texture))) {
					return m_result;
				}
				D3D11_TEXTURE2D_DESC desc{};
				texture->GetDesc(&desc);
				if (view.Texture2D.MostDetailedMip >= desc.MipLevels) {
					return m_result;
				}
				const auto width = (std::max)(1u, desc.Width >> view.Texture2D.MostDetailedMip);
				const auto height = (std::max)(1u, desc.Height >> view.Texture2D.MostDetailedMip);
				// 4096^2 * 255 still fits the u32 counters
				if (width > 4096 || height > 4096 || !a_shader || !Create(a_device)) {
					return m_result;
				}
				{
					SavedCompute saved(a_ctx);
					const UINT zeros[4]{};
					a_ctx->ClearUnorderedAccessViewUint(m_view.Get(), zeros);
					a_ctx->CSSetShader(a_shader, nullptr, 0);
					a_ctx->CSSetShaderResources(0, 1, &a_src);
					a_ctx->CSSetConstantBuffers(0, 1, &a_constants);
					a_ctx->CSSetSamplers(0, 1, &a_sampler);
					a_ctx->CSSetUnorderedAccessViews(0, 1, m_view.GetAddressOf(), nullptr);
					a_ctx->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
				}
				a_ctx->CopyResource(m_readback.Get(), m_output.Get());
				m_queued = true;
				return m_result = Result::kPending;
			}

			float PeakAlpha() const { return m_peakAlpha; }

		private:
			struct SavedCompute
			{
				ID3D11DeviceContext* ctx;
				ComPtr<ID3D11ComputeShader> shader;
				ComPtr<ID3D11ShaderResourceView> source;
				ComPtr<ID3D11UnorderedAccessView> output;
				ComPtr<ID3D11Buffer> constants;
				ComPtr<ID3D11SamplerState> sampler;
				std::array<ID3D11ClassInstance*, 256> instances{};
				UINT count = 256;

				explicit SavedCompute(ID3D11DeviceContext* a_ctx) :
					ctx(a_ctx)
				{
					ctx->CSGetShader(shader.GetAddressOf(), instances.data(), &count);
					ctx->CSGetShaderResources(0, 1, source.GetAddressOf());
					ctx->CSGetUnorderedAccessViews(0, 1, output.GetAddressOf());
					ctx->CSGetConstantBuffers(0, 1, constants.GetAddressOf());
					ctx->CSGetSamplers(0, 1, sampler.GetAddressOf());
				}

				~SavedCompute()
				{
					ctx->CSSetUnorderedAccessViews(0, 1, output.GetAddressOf(), nullptr);
					ctx->CSSetShaderResources(0, 1, source.GetAddressOf());
					ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
					ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
					ctx->CSSetShader(shader.Get(), instances.data(), count);
					for (auto* i : instances) {
						if (i) {
							i->Release();
						}
					}
				}
			};

			bool Create(ID3D11Device* a_device)
			{
				if (m_output && m_view && m_readback) {
					return true;
				}
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = 16;
				desc.StructureByteStride = 4;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				if (FAILED(a_device->CreateBuffer(&desc, nullptr, m_output.ReleaseAndGetAddressOf())) ||
					FAILED(a_device->CreateUnorderedAccessView(m_output.Get(), nullptr, m_view.ReleaseAndGetAddressOf()))) {
					return false;
				}
				desc.Usage = D3D11_USAGE_STAGING;
				desc.BindFlags = 0;
				desc.MiscFlags = 0;
				desc.StructureByteStride = 0;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				return SUCCEEDED(a_device->CreateBuffer(&desc, nullptr, m_readback.ReleaseAndGetAddressOf()));
			}

			ComPtr<ID3D11ShaderResourceView> m_source;
			ComPtr<ID3D11Buffer> m_output;
			ComPtr<ID3D11Buffer> m_readback;
			ComPtr<ID3D11UnorderedAccessView> m_view;
			Key m_key{};
			Result m_result = Result::kPending;
			float m_peakAlpha = 1.0f;
			bool m_seen = false;
			bool m_queued = false;
		};

		struct MeshCopy
		{
			Readback vertices;
			Readback indices;
			std::vector<ReticleFit::Vertex> local;
			std::vector<ReticleFit::Vertex> decoded;
			std::vector<std::uint16_t> triangles;
			UINT stride = 0;
			UINT uvOffset = 0;
			UINT colorOffset = 0;
			std::array<float, 4> vertexTint{ 1.0f, 1.0f, 1.0f, 1.0f };
			bool fullPrecision = false;
			bool vertexReady = false;
			bool indexReady = false;
			bool hasColor = false;
		};

		struct OpticCache
		{
			std::uint64_t generation = 0;
			std::array<MeshIdentity, ReticleFit::kMaxMeshes> ids{};
			std::array<MeshCopy, ReticleFit::kMaxMeshes> copies;
			std::size_t count = 0;
			std::size_t nextCopy = 0;
			ReticleFit::CacheState status = ReticleFit::CacheState::kEmpty;
			bool buffersReady = false;
			bool retry = false;
			ReticleFit::Frame frame{};
			VisibilityProbe visibility;
			VisibilityProbe::Result reportedVisibility = VisibilityProbe::Result::kPending;
			std::uint64_t lastVisibilityReport = 0;
			std::uint64_t lastReport = 0;
			std::uint64_t lastRejection = 0;
		};

		struct Gpu
		{
			ComPtr<ID3D11Device> device;
			ComPtr<ID3D11VertexShader> vertex;
			ComPtr<ID3D11PixelShader> pixel;
			ComPtr<ID3D11ComputeShader> visibility;
			ComPtr<ID3D11BlendState> assistBlend;
			ComPtr<ID3D11BlendState> emitterBlend;
			ComPtr<ID3D11InputLayout> halfLayout;
			ComPtr<ID3D11InputLayout> fullLayout;
			ComPtr<ID3D11Buffer> constants;
			ComPtr<ID3D11SamplerState> sampler;
			ComPtr<ID3D11DepthStencilState> depth;
			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			std::array<ComPtr<ID3D11RasterizerState>, 2> rasters;
			std::array<D3D11_RASTERIZER_DESC, 2> rasterDescs{};
		};

		// a RunPass inside our correction-only retry draws nothing natively
		struct CorrectionOnly;
		thread_local CorrectionOnly* t_correction = nullptr;
		struct CorrectionOnly
		{
			void* pass;
			bool drawn = false;
			CorrectionOnly* previous = t_correction;

			explicit CorrectionOnly(void* a_pass) :
				pass(a_pass)
			{
				t_correction = this;
			}
			~CorrectionOnly() { t_correction = previous; }
			CorrectionOnly(const CorrectionOnly&) = delete;
			CorrectionOnly& operator=(const CorrectionOnly&) = delete;
		};

		// game thread publishes, render thread reads; both try_lock, neither waits
		std::mutex g_sampleLock;
		Sample g_sample{};
		std::atomic<std::uint64_t> g_epoch{ 1 };
		std::array<std::atomic<std::uintptr_t>, kMaxReticles> g_witness{};
		std::atomic<bool> g_resetCache{ false };
		std::atomic<Stage> g_stage{ Stage::kIdle };

		std::atomic<std::uint64_t> g_reticlePasses{ 0 };
		std::atomic<std::uint64_t> g_effectPasses{ 0 };
		std::atomic<std::uint64_t> g_draws{ 0 };
		std::atomic<std::uint64_t> g_packetReroutes{ 0 };
		std::atomic<std::uint64_t> g_techniqueRecoveries{ 0 };
		std::atomic<std::uint64_t> g_geometryReloads{ 0 };
		std::atomic<std::uint64_t> g_poseRefits{ 0 };
		std::array<std::atomic<std::uint64_t>, kMaxReticles> g_rejectedTechniqueGen{};

		// render thread only
		Gpu g_gpu;
		std::array<OpticCache, kMaxReticles> g_optics;
		std::array<std::uint64_t, kMaxReticles> g_rejectedOpticGen{};
		std::array<std::uint64_t, kMaxReticles> g_rejectedMaterialGen{};
		std::atomic<DWORD> g_renderThread{ 0 };

		// game thread only
		std::uintptr_t g_weapon = 0;
		std::uint64_t g_generation = 0;
		std::uint64_t g_nextSelect = 0;
		std::array<std::uintptr_t, kMaxReticles> g_selected{};
		std::uint32_t g_excluded = 0;
		bool g_published = false;

		// install
		bool g_installed = false;
		ComPtr<ID3DBlob> g_vsCode, g_psCode, g_csCode;
		std::uintptr_t g_contextSlot = 0;
		std::uintptr_t g_stereoSlot = 0;
		std::uintptr_t g_renderer = 0;
		std::uintptr_t g_effectShaderVtbl = 0;
		std::uintptr_t g_effectMaterialVtbl = 0;
		DrawPass_t g_originalDrawPass = nullptr;
		DrawBatches_t g_originalDrawBatches = nullptr;
		ContextFn_t g_waitForPackets = nullptr;
		RunPackets_t g_runPackets = nullptr;
		RunPass_t g_runPass = nullptr;
		ContextFn_t g_restorePass = nullptr;
		DrawShape_t g_drawShape = nullptr;

		// DevBench
		std::mutex g_diagLock;
		Diag g_diag{};

		void SetLastReject(const char* a_why)
		{
			std::lock_guard l(g_diagLock);
			strncpy_s(g_diag.lastReject, a_why, _TRUNCATE);
		}

		bool ReadBytes(std::uintptr_t a_p, void* a_out, std::size_t a_n) noexcept
		{
			if (a_p < 0x10000 || a_p > 0x00007FFFFFFFFFFF - a_n) {
				return false;
			}
			__try {
				std::memcpy(a_out, reinterpret_cast<const void*>(a_p), a_n);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		template <class T>
		bool Read(std::uintptr_t a_p, T& a_out) noexcept
		{
			return ReadBytes(a_p, &a_out, sizeof(T));
		}

		bool Plausible(std::uintptr_t a_p)
		{
			return a_p >= 0x10000 && a_p <= 0x00007FFFFFFFFFFF && (a_p & 7) == 0;
		}

		template <class T>
		T At(std::uintptr_t a_p)
		{
			return *reinterpret_cast<const T*>(a_p);
		}

		// BSFixedString field -> its pool text (entry +0x10 length, +0x18 chars)
		std::string_view NameAt(std::uintptr_t a_field)
		{
			std::uintptr_t entry = 0;
			std::uint32_t len = 0;
			char end = 1;
			if (!Read(a_field, entry) || entry < 0x10000 || !Read(entry + 0x10, len) || !len || len > 1024 ||
				!Read(entry + 0x18 + len, end) || end != '\0') {
				return {};
			}
			return { reinterpret_cast<const char*>(entry + 0x18), len };
		}

		bool Culled(std::uintptr_t a_obj) { return (At<std::uint8_t>(a_obj + kFlags) & 1u) != 0; }

		std::uintptr_t AsTriShape(std::uintptr_t a_obj)
		{
			using Fn = std::uintptr_t (*)(std::uintptr_t);
			const auto vtbl = At<std::uintptr_t>(a_obj);
			return At<Fn>(vtbl + kIsTriShapeVFunc)(a_obj);
		}

		std::uint16_t ChildCount(std::uintptr_t a_node) { return At<std::uintptr_t>(a_node + kChildren) ? At<std::uint16_t>(a_node + kChildCount) : 0; }
		std::uintptr_t Child(std::uintptr_t a_node, std::uint16_t a_i) { return At<std::uintptr_t>(At<std::uintptr_t>(a_node + kChildren) + a_i * 8ull); }

		Xform ReadXform(std::uintptr_t a_at)
		{
			Xform x{};
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					x.r[i][j] = At<float>(a_at + i * 0x10 + j * 4);
				}
			}
			x.t = { At<float>(a_at + 0x30), At<float>(a_at + 0x34), At<float>(a_at + 0x38) };
			x.s = At<float>(a_at + 0x3c);
			return x;
		}

		bool FiniteXform(const Xform& a_x)
		{
			if (!std::isfinite(a_x.t.x) || !std::isfinite(a_x.t.y) || !std::isfinite(a_x.t.z) || !std::isfinite(a_x.s) || std::fabs(a_x.s) <= 1e-5f) {
				return false;
			}
			for (const auto& row : a_x.r) {
				for (const float f : row) {
					if (!std::isfinite(f)) {
						return false;
					}
				}
			}
			return true;
		}

		Xform Compose(const Xform& a_parent, const Xform& a_local)
		{
			Xform out{};
			out.s = a_parent.s * a_local.s;
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					out.r[i][j] = a_local.r[i][0] * a_parent.r[0][j] + a_local.r[i][1] * a_parent.r[1][j] + a_local.r[i][2] * a_parent.r[2][j];
				}
			}
			const auto& t = a_local.t;
			const auto& p = a_parent;
			out.t = { p.t.x + p.s * (t.x * p.r[0][0] + t.y * p.r[1][0] + t.z * p.r[2][0]),
				p.t.y + p.s * (t.x * p.r[0][1] + t.y * p.r[1][1] + t.z * p.r[2][1]),
				p.t.z + p.s * (t.x * p.r[0][2] + t.y * p.r[1][2] + t.z * p.r[2][2]) };
			return out;
		}

		Vec3 Apply(const Xform& a_x, Vec3 a_p)
		{
			return { a_x.t.x + a_x.s * (a_p.x * a_x.r[0][0] + a_p.y * a_x.r[1][0] + a_p.z * a_x.r[2][0]),
				a_x.t.y + a_x.s * (a_p.x * a_x.r[0][1] + a_p.y * a_x.r[1][1] + a_p.z * a_x.r[2][1]),
				a_x.t.z + a_x.s * (a_p.x * a_x.r[0][2] + a_p.y * a_x.r[1][2] + a_p.z * a_x.r[2][2]) };
		}

		// local direction -> parent
		Vec3 Rotate(const Xform& a_x, Vec3 a_d)
		{
			return { a_d.x * a_x.r[0][0] + a_d.y * a_x.r[1][0] + a_d.z * a_x.r[2][0],
				a_d.x * a_x.r[0][1] + a_d.y * a_x.r[1][1] + a_d.z * a_x.r[2][1],
				a_d.x * a_x.r[0][2] + a_d.y * a_x.r[1][2] + a_d.z * a_x.r[2][2] };
		}

		// parent direction -> local
		Vec3 Unrotate(const Xform& a_x, Vec3 a_d)
		{
			return { a_x.r[0][0] * a_d.x + a_x.r[0][1] * a_d.y + a_x.r[0][2] * a_d.z,
				a_x.r[1][0] * a_d.x + a_x.r[1][1] * a_d.y + a_x.r[1][2] * a_d.z,
				a_x.r[2][0] * a_d.x + a_x.r[2][1] * a_d.y + a_x.r[2][2] * a_d.z };
		}

		std::uintptr_t ShaderProperty(std::uintptr_t a_shape) { return At<std::uintptr_t>(a_shape + kShaderProperty); }
		std::string_view MaterialName(std::uintptr_t a_shape)
		{
			const auto p = ShaderProperty(a_shape);
			return Plausible(p) ? NameAt(p + kName) : std::string_view{};
		}

		// geometry +0x178 -> property +0x58, and the material's vtable is the effect material's
		bool EffectMaterial(std::uintptr_t a_shape, std::uintptr_t& a_material)
		{
			const auto property = ShaderProperty(a_shape);
			std::uintptr_t vtbl = 0;
			return Plausible(property) && Read(property + kPropertyMaterial, a_material) && Plausible(a_material) &&
			       Read(a_material, vtbl) && vtbl == g_effectMaterialVtbl;
		}

		Role ShapeRole(std::uintptr_t a_shape)
		{
			const auto node = NameAt(a_shape + kName);
			const auto named = ReticleFit::RoleOf(node, MaterialName(a_shape));
			if (named != Role::kOther) {
				return named;
			}
			std::uintptr_t material = 0;
			if (!EffectMaterial(a_shape, material)) {
				return named;
			}
			const auto file = NameAt(material + kMaterialTexturePath);
			const auto textured = ReticleFit::RoleOf(node, MaterialName(a_shape), file);
			if (textured != Role::kOther) {
				return textured;
			}
			std::uint64_t flags = 0;
			if (!Read(ShaderProperty(a_shape) + kPropertyFlags, flags) || !ReticleFit::OpticalGlowDot(node, file, (flags & kFalloff) != 0, true)) {
				return textured;
			}
			// a vanilla glow dot only counts with glass beside it
			const auto parent = At<std::uintptr_t>(a_shape + kParent);
			if (parent && ChildCount(parent) <= kMaxOpticChildren) {
				for (std::uint16_t i = 0; i < ChildCount(parent); ++i) {
					const auto sibling = Child(parent, i);
					const auto tri = sibling ? AsTriShape(sibling) : 0;
					if (tri && !Culled(tri) && ReticleFit::RoleOf(NameAt(tri + kName), MaterialName(tri)) == Role::kLens) {
						return Role::kReticle;
					}
				}
			}
			return textured;
		}

		// A null reason means accepted. Collects the reticle and every visible shape
		// under a_parent in a_parent's frame.
		const char* CollectAt(std::uintptr_t a_reticle, std::uintptr_t a_parent, Assembly& a_out)
		{
			a_out = {};
			a_out.parent = a_parent;
			if (!a_parent) {
				return "missing-optic-parent";
			}
			struct Pending
			{
				std::uintptr_t node;
				Xform parent;
			};
			std::array<Pending, kMaxOpticChildren> pending{};
			if (ChildCount(a_parent) > pending.size()) {
				return "parent-child-capacity";
			}
			Xform identity{};
			identity.s = 1.0f;
			for (int i = 0; i < 3; ++i) {
				identity.r[i][i] = 1.0f;
			}
			std::size_t n = 0, visited = 0, lenses = 0, triangles = 0;
			bool found = false;
			a_out.count = 1;
			for (std::uint16_t i = 0; i < ChildCount(a_parent); ++i) {
				if (const auto c = Child(a_parent, i)) {
					pending[n++] = { c, identity };
				}
			}
			while (n) {
				const auto item = pending[--n];
				const auto node = item.node;
				if (Culled(node)) {
					continue;
				}
				if (++visited > pending.size()) {
					return "subtree-node-limit";
				}
				const auto localX = ReadXform(node + kLocal);
				if (!FiniteXform(localX) || localX.s <= 0.0f) {
					return "invalid-local-transform";
				}
				const auto local = Compose(item.parent, localX);
				if (ScopeIdent::IsNiNode(node)) {
					if (ChildCount(node) > pending.size() - n) {
						return "subtree-child-capacity";
					}
					for (std::uint16_t i = 0; i < ChildCount(node); ++i) {
						if (const auto c = Child(node, i)) {
							pending[n++] = { c, local };
						}
					}
					continue;
				}
				const auto shape = AsTriShape(node);
				if (!shape) {
					continue;
				}
				const auto role = ShapeRole(shape);
				if (role == Role::kReticle && shape != a_reticle) {
					continue;
				}
				const auto type = At<std::uint8_t>(shape + kGeometryType);
				const auto desc = At<std::uint64_t>(shape + kVertexDesc);
				const auto tris = At<std::uint32_t>(shape + kTriangleCount);
				const auto verts = At<std::uint16_t>(shape + kVertexCount);
				if (type != 3 && type != 8) {
					return "unsupported-geometry-buffers";
				}
				if (At<std::uintptr_t>(shape + kSkinInstance) || ((desc >> 44) & kVfSkinned)) {
					return "skinned-geometry";
				}
				if (!At<std::uintptr_t>(shape + kRendererData)) {
					return "missing-renderer-data";
				}
				if (!((desc >> 44) & kVfVertex)) {
					return "missing-position";
				}
				if (verts < 3 || !tris) {
					return "mesh-counts";
				}
				triangles += tris;
				if (triangles > ReticleFit::kMaxTriangles) {
					return "triangle-budget";
				}
				const auto property = ShaderProperty(shape);
				std::uint64_t flags = 0;
				std::uintptr_t material = 0;
				std::uint16_t alpha = 0;
				if (!Plausible(property) || !Read(property + kPropertyFlags, flags) || !Read(property + kPropertyMaterial, material) || !Plausible(material)) {
					return "material-read";
				}
				const auto alphaProperty = At<std::uintptr_t>(shape + kAlphaProperty);
				if (alphaProperty && !Read(alphaProperty + kAlphaFlags, alpha)) {
					return "alpha-property-read";
				}
				std::size_t index = 0;
				if (shape == a_reticle) {
					if (!EffectMaterial(shape, material)) {
						return "reticle-material-type";
					}
					if (!((desc >> 44) & kVfUv)) {
						return "reticle-uv-missing";
					}
					found = true;
				} else {
					if (a_out.count == a_out.shapes.size()) {
						return "mesh-budget";
					}
					index = a_out.count++;
					if (role == Role::kLens && (alpha & 0x201)) {
						++lenses;
					}
				}
				a_out.shapes[index] = shape;
				a_out.ids[index] = { shape, At<std::uintptr_t>(shape + kRendererData), material, desc, flags, tris, verts, alpha, role, local };
			}
			if (!found) {
				return "reticle-hidden";
			}
			if (!lenses) {
				return "lens-count";
			}
			return nullptr;
		}

		// Wrapper nodes are common around a reticle: walk out to the first
		// assembly with a lens, else the nearest one with something beside the card.
		const char* Collect(std::uintptr_t a_reticle, Assembly& a_out)
		{
			auto parent = At<std::uintptr_t>(a_reticle + kParent);
			const char* reason = "missing-optic-parent";
			Assembly authored{};
			bool haveAuthored = false;
			for (unsigned depth = 0; parent && depth < 6; ++depth, parent = At<std::uintptr_t>(parent + kParent)) {
				reason = CollectAt(a_reticle, parent, a_out);
				if (!reason) {
					return nullptr;
				}
				if (std::string_view(reason) != "lens-count") {
					break;
				}
				if (!haveAuthored && a_out.count >= 2) {
					authored = a_out;
					haveAuthored = true;
				}
			}
			if (haveAuthored) {
				a_out = authored;
				return nullptr;
			}
			return reason;
		}

		bool SameResources(const MeshIdentity& a_a, const MeshIdentity& a_b)
		{
			// bits 39 and 43 flip at runtime without the mesh changing
			constexpr std::uint64_t kVolatile = (1ull << 39) | (1ull << 43);
			return a_a.shape == a_b.shape && a_a.data == a_b.data && a_a.material == a_b.material && a_a.descriptor == a_b.descriptor &&
			       ((a_a.flags ^ a_b.flags) & ~kVolatile) == 0 && a_a.triangles == a_b.triangles && a_a.vertices == a_b.vertices &&
			       a_a.alphaFlags == a_b.alphaFlags && a_a.role == a_b.role;
		}

		bool SamePose(const MeshIdentity& a_a, const MeshIdentity& a_b)
		{
			return std::memcmp(&a_a.local, &a_b.local, sizeof(Xform)) == 0;
		}

		// Reads every mesh of the optic back from the GPU, a copy a frame, then fits
		// the optical frame. False until ready; a rejection keeps the native draw.
		bool QualifyOptic(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, const Assembly& a_asm, const Sample& a_sample, std::size_t a_index)
		{
			auto& cache = g_optics[a_index];
			bool sameResources = cache.generation == a_sample.generation && cache.count == a_asm.count;
			bool samePose = sameResources;
			for (std::size_t i = 0; sameResources && i < a_asm.count; ++i) {
				sameResources = SameResources(cache.ids[i], a_asm.ids[i]);
				samePose = samePose && SamePose(cache.ids[i], a_asm.ids[i]);
			}
			const auto now = ::GetTickCount64();
			const auto action = ReticleFit::Choose(cache.status, sameResources, samePose, cache.buffersReady, cache.retry && now - cache.lastRejection >= 500);
			using Action = ReticleFit::CacheAction;
			using State = ReticleFit::CacheState;
			if (action == Action::kReuse) {
				return true;
			}
			if (action == Action::kKeepRejected) {
				g_stage.store(Stage::kUnsupported, std::memory_order_relaxed);
				return false;
			}
			if (cache.status == State::kRejected && sameResources && now - cache.lastRejection < 500) {
				g_stage.store(Stage::kUnsupported, std::memory_order_relaxed);
				return false;
			}
			if (action == Action::kReload) {
				g_geometryReloads.fetch_add(1, std::memory_order_relaxed);
				const auto lastReport = cache.generation == a_sample.generation ? cache.lastReport : 0;
				cache = {};
				cache.generation = a_sample.generation;
				cache.lastReport = lastReport;
				cache.ids = a_asm.ids;
				cache.count = a_asm.count;
				cache.status = State::kPending;
			}
			if (action == Action::kRefit) {
				g_poseRefits.fetch_add(1, std::memory_order_relaxed);
			}
			const auto reject = [&](const char* a_why, bool a_retry = false) {
				cache.status = State::kRejected;
				cache.lastRejection = now;
				cache.retry = a_retry;
				for (auto& c : cache.copies) {
					c.vertices.Reset();
					c.indices.Reset();
				}
				if (!cache.lastReport || now - cache.lastReport >= 5000) {
					logger::info(FMT_STRING("reflex reticle: generation {} not corrected ({}), native draw kept"), a_sample.generation, a_why);
					cache.lastReport = now;
				}
				SetLastReject(a_why);
				return false;
			};
			g_stage.store(Stage::kQualifying, std::memory_order_relaxed);

			// start the next copy: bind the shape so the engine sets its buffers,
			// take them, then put the reticle's binding back
			if (cache.nextCopy < a_asm.count) {
				const auto i = cache.nextCopy++;
				auto& copy = cache.copies[i];
				const auto& id = a_asm.ids[i];
				g_drawShape(reinterpret_cast<void*>(g_renderer), reinterpret_cast<void*>(id.data), 0, 0);
				ComPtr<ID3D11Buffer> vb, ib;
				UINT offset = 0, indexOffset = 0;
				DXGI_FORMAT format{};
				a_ctx->IAGetVertexBuffers(0, 1, vb.GetAddressOf(), &copy.stride, &offset);
				a_ctx->IAGetIndexBuffer(ib.GetAddressOf(), &format, &indexOffset);
				g_drawShape(reinterpret_cast<void*>(g_renderer), reinterpret_cast<void*>(a_asm.ids[0].data), 0, 0);
				copy.fullPrecision = ((id.descriptor >> 44) & kVfFullPrec) != 0;
				copy.uvOffset = static_cast<UINT>((id.descriptor >> 6) & 0x3c);
				if (!vb || !ib || !copy.stride) {
					return reject("buffers-not-ready", true);
				}
				copy.hasColor = i == 0 && (id.flags & kVertexColours) != 0;
				if (copy.hasColor) {
					copy.colorOffset = static_cast<UINT>((id.descriptor >> 22) & 0x3c);
					if (!((id.descriptor >> 44) & kVfColors) || copy.colorOffset < copy.uvOffset + 4 || copy.colorOffset + 4 > copy.stride) {
						return reject("invalid-reticle-color-layout");
					}
				}
				const UINT minStride = copy.fullPrecision ? 16u : 8u;
				if (format != DXGI_FORMAT_R16_UINT || copy.stride != (id.descriptor & 15) * 4 || copy.stride < minStride ||
					(i == 0 && (copy.uvOffset < minStride || copy.uvOffset + 4 > copy.stride)) || copy.stride * id.vertices > Readback::kMaxBytes) {
					return reject("unsupported-buffer-layout");
				}
				if (!copy.vertices.Begin(a_device, a_ctx, vb.Get(), offset, copy.stride * id.vertices) ||
					!copy.indices.Begin(a_device, a_ctx, ib.Get(), indexOffset, id.triangles * 6)) {
					return reject("buffer-copy-unavailable", true);
				}
				copy.local.resize(id.vertices);
				copy.decoded.resize(id.vertices);
				copy.triangles.resize(id.triangles * 3ull);
				return false;
			}

			using Poll = Readback::Poll;
			for (std::size_t i = 0; i < a_asm.count; ++i) {
				auto& copy = cache.copies[i];
				if (!copy.vertexReady) {
					auto colour = ReticleFit::Tint::kUniform;
					const auto result = copy.vertices.Inspect(a_ctx, [&](const std::byte* a_bytes) {
						if (copy.hasColor) {
							colour = ReticleFit::ReadUniformTint({ a_bytes, copy.stride * copy.decoded.size() }, copy.decoded.size(),
								copy.stride, copy.colorOffset, copy.vertexTint);
							if (colour != ReticleFit::Tint::kUniform) {
								return false;
							}
						}
						for (std::size_t v = 0; v < copy.decoded.size(); ++v) {
							const auto* data = a_bytes + v * copy.stride;
							Vec3 p{};
							if (copy.fullPrecision) {
								std::memcpy(&p, data, sizeof(p));
							} else {
								std::array<PackedVector::HALF, 4> h{};
								std::memcpy(h.data(), data, 8);
								p = { PackedVector::XMConvertHalfToFloat(h[0]), PackedVector::XMConvertHalfToFloat(h[1]),
									PackedVector::XMConvertHalfToFloat(h[2]) };
							}
							copy.local[v].position = p;
							if (i == 0) {
								std::array<PackedVector::HALF, 2> uv{};
								std::memcpy(uv.data(), data + copy.uvOffset, 4);
								copy.local[v].u = PackedVector::XMConvertHalfToFloat(uv[0]);
								copy.local[v].v = PackedVector::XMConvertHalfToFloat(uv[1]);
							}
						}
						return true;
					});
					if (result == Poll::kFailed) {
						return reject(colour == ReticleFit::Tint::kVarying ? "varying-reticle-vertex-colour" : "vertex-readback-failed",
							colour == ReticleFit::Tint::kUniform);
					}
					if (result == Poll::kPending) {
						return false;
					}
					copy.vertexReady = true;
					copy.vertices.Reset();
				}
				if (!copy.indexReady) {
					const auto result = copy.indices.Inspect(a_ctx, [&](const std::byte* a_bytes) {
						std::memcpy(copy.triangles.data(), a_bytes, copy.triangles.size() * 2);
						return true;
					});
					if (result == Poll::kFailed) {
						return reject("index-readback-failed", true);
					}
					if (result == Poll::kPending) {
						return false;
					}
					copy.indexReady = true;
					copy.indices.Reset();
				}
			}
			cache.buffersReady = true;
			cache.ids = a_asm.ids;
			for (std::size_t i = 0; i < a_asm.count; ++i) {
				auto& copy = cache.copies[i];
				for (std::size_t v = 0; v < copy.local.size(); ++v) {
					copy.decoded[v] = copy.local[v];
					copy.decoded[v].position = Apply(a_asm.ids[i].local, copy.local[v].position);
				}
			}
			std::array<ReticleFit::Mesh, ReticleFit::kMaxMeshes> meshes{};
			for (std::size_t i = 0; i < a_asm.count; ++i) {
				const auto& id = cache.ids[i];
				const bool lens = id.role == Role::kLens && (id.alphaFlags & 0x201);
				meshes[i] = { cache.copies[i].decoded, cache.copies[i].triangles, (id.flags & kTwoSided) != 0, lens, (id.alphaFlags & 0x201) == 0 };
			}
			// the bore in the assembly's frame decides which side of the card faces the eye
			const auto parentWorld = ReadXform(a_asm.parent + kWorld);
			const auto result = ReticleFit::Qualify({ meshes.data(), a_asm.count }, cache.frame, Unrotate(parentWorld, a_sample.bore));
			if (result != ReticleFit::Result::kReady) {
				return reject(ReticleFit::ResultName(result));
			}
			cache.status = State::kReady;
			if (!cache.lastReport || now - cache.lastReport >= 5000) {
				const auto aperture = NameAt(a_asm.shapes[cache.frame.lensMesh] + kName);
				logger::info(FMT_STRING("reflex reticle: generation {} ready, {} meshes, reticle '{}', aperture '{}' ({})"), a_sample.generation,
					a_asm.count, NameAt(a_asm.shapes[0] + kName), aperture, cache.frame.lensMesh ? "glass" : "reticle card");
				cache.lastReport = now;
				std::lock_guard l(g_diagLock);
				const auto n = (std::min)(aperture.size(), sizeof(g_diag.aperture) - 1);
				std::memcpy(g_diag.aperture, aperture.data(), n);
				g_diag.aperture[n] = '\0';
			}
			return true;
		}

		bool MaterialTint(std::uintptr_t a_reticle, Constants& a_c)
		{
			std::uintptr_t material = 0, effectData = 0;
			std::array<float, 4> uv{};
			float emission = 0.0f, propertyAlpha = 0.0f;
			const auto property = ShaderProperty(a_reticle);
			auto& tint = a_c.tint;
			if (!EffectMaterial(a_reticle, material) || !Read(property + kPropertyAlpha, propertyAlpha) || !std::isfinite(propertyAlpha) ||
				!Read(property + kPropertyEffectData, effectData) || effectData || !Read(material + kMaterialUv, uv) ||
				!Read(material + kMaterialColor, tint) || !Read(material + kMaterialEmission, emission) || !std::isfinite(emission) ||
				emission < 0.0f || emission > 64.0f || !std::isfinite(tint.w) || tint.w < 0.0f || tint.w > 1.0f) {
				return false;
			}
			for (const float f : uv) {
				if (!std::isfinite(f)) {
					return false;
				}
			}
			if (std::fabs(uv[2]) < 1e-6f || std::fabs(uv[3]) < 1e-6f) {
				return false;
			}
			a_c.uvTransform = { uv[0], uv[1], uv[2], uv[3] };
			a_c.look2.z = ReticleFit::detail::Contains(ReticleFit::detail::BaseName(NameAt(material + kMaterialTexturePath)), "glowspot") ? 1.0f : 0.0f;
			tint.w *= std::clamp(propertyAlpha, 0.0f, 1.0f);
			for (auto* c : { &tint.x, &tint.y, &tint.z }) {
				if (!std::isfinite(*c) || *c < 0.0f || *c * emission > 64.0f) {
					return false;
				}
				*c = std::pow(*c * emission, 2.2f);
			}
			const auto alphaProperty = At<std::uintptr_t>(a_reticle + kAlphaProperty);
			if (alphaProperty) {
				std::uint16_t flags = 0;
				std::uint8_t ref = 0;
				if (!Read(alphaProperty + kAlphaFlags, flags) || !Read(alphaProperty + kAlphaRef, ref)) {
					return false;
				}
				// the same cutoff the engine's effect setup derives
				if (flags & 0x200) {
					a_c.depthRange.z = static_cast<float>(ref) / 255.0f + 0.0039215292781591415f + (ref == 4 ? 1.0f / 255.0f : 0.0f);
				}
			}
			return true;
		}

		// eye position from its view-projection, in its own floating origin
		bool RecoverEye(const XMFLOAT4X4& a_m, const XMFLOAT4& a_origin, Vec3& a_eye)
		{
			a_eye = {};
			for (const auto& row : a_m.m) {
				for (const float f : row) {
					if (!std::isfinite(f) || std::fabs(f) > 1e8f) {
						return false;
					}
				}
			}
			if (!std::isfinite(a_origin.x) || !std::isfinite(a_origin.y) || !std::isfinite(a_origin.z)) {
				return false;
			}
			XMVECTOR det{};
			const auto inv = XMMatrixInverse(&det, XMLoadFloat4x4(&a_m));
			if (!std::isfinite(XMVectorGetX(det)) || std::fabs(XMVectorGetX(det)) < 1e-9f) {
				return false;
			}
			const auto h = XMVector4Transform(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), inv);
			const float w = XMVectorGetW(h);
			if (!std::isfinite(w) || std::fabs(w) < 1e-8f) {
				return false;
			}
			a_eye = { XMVectorGetX(h) / w + a_origin.x, XMVectorGetY(h) / w + a_origin.y, XMVectorGetZ(h) / w + a_origin.z };
			return ReticleFit::detail::Finite(a_eye);
		}

		bool FillConstants(const Assembly& a_asm, const Sample& a_sample, std::size_t a_index, Constants& a_c)
		{
			const auto& frame = g_optics[a_index].frame;
			const auto glass = a_asm.shapes[frame.lensMesh];
			const auto world = ReadXform(glass + kWorld);
			const auto parent = ReadXform(a_asm.parent + kWorld);
			if (!FiniteXform(world) || world.s <= 0.0f || !FiniteXform(parent) || parent.s <= 0.0f) {
				return false;
			}
			std::uintptr_t root = 0, records = 0;
			if (!Read(g_stereoSlot, root) || !Plausible(root) || !Read(root + kStereoRecords, records) || !Plausible(records) ||
				!Read(root + kStereoOrigins, a_c.originAdjust) || !Read(records + kEyeViewProj, a_c.viewProjection[0]) ||
				!Read(records + kEyeStride + kEyeViewProj, a_c.viewProjection[1])) {
				return false;
			}
			auto& m = a_c.localToWorld;
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					m.m[i][j] = world.r[i][j] * world.s;
				}
			}
			m._41 = world.t.x;
			m._42 = world.t.y;
			m._43 = world.t.z;
			m._44 = 1.0f;
			auto& uv = a_c.uvTransform;
			uv = { uv.x + frame.uvBounds[0] * uv.z, uv.y + frame.uvBounds[1] * uv.w, uv.z * frame.uvBounds[2], uv.w * frame.uvBounds[3] };
			const auto forward = Rotate(parent, frame.forward);
			const auto right = Rotate(parent, frame.right);
			const auto lensCenter = Apply(parent, frame.lensCenter);
			const auto reticleCenter = Apply(parent, frame.reticleCenter);
			a_c.depthPlane = { forward.x, forward.y, forward.z, ReticleFit::detail::Dot(forward, reticleCenter) };
			for (int e = 0; e < 2; ++e) {
				Vec3 eye{};
				if (!RecoverEye(a_c.viewProjection[e], a_c.originAdjust[e], eye)) {
					return false;
				}
				ReticleFit::EyeAim aim{};
				const bool visible = ReticleFit::BuildEyeAim(eye, lensCenter, forward, right, a_sample.target, aim);
				a_c.eye[e] = { eye.x, eye.y, eye.z, visible ? 1.0f : 0.0f };
				a_c.aimForward[e] = { aim.forward.x, aim.forward.y, aim.forward.z, a_c.look2.z > 0.5f ? a_sample.dotHalfAngle : a_sample.halfAngle };
				a_c.aimRight[e] = { aim.right.x, aim.right.y, aim.right.z, frame.aspect };
				a_c.aimUp[e] = { aim.up.x, aim.up.y, aim.up.z, 0.0f };
			}
			return true;
		}

		bool CreateResources(ID3D11Device* a_device)
		{
			if (g_gpu.device.Get() == a_device && g_gpu.vertex) {
				return true;
			}
			Gpu gpu;
			auto vs = g_vsCode.Get();
			auto ps = g_psCode.Get();
			auto cs = g_csCode.Get();
			gpu.device = a_device;
			if (!vs || !ps || !cs ||
				FAILED(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, gpu.vertex.GetAddressOf())) ||
				FAILED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, gpu.pixel.GetAddressOf())) ||
				FAILED(a_device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, gpu.visibility.GetAddressOf()))) {
				return false;
			}
			D3D11_BLEND_DESC blend{};
			auto& rt = blend.RenderTarget[0];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
			rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
			rt.DestBlendAlpha = D3D11_BLEND_ONE;
			rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(a_device->CreateBlendState(&blend, gpu.assistBlend.GetAddressOf()))) {
				return false;
			}
			// premultiplied: the emitter's light adds, its core alpha hides the scene
			rt.SrcBlend = D3D11_BLEND_ONE;
			if (FAILED(a_device->CreateBlendState(&blend, gpu.emitterBlend.GetAddressOf()))) {
				return false;
			}
			D3D11_INPUT_ELEMENT_DESC position{ "POSITION", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
			if (FAILED(a_device->CreateInputLayout(&position, 1, vs->GetBufferPointer(), vs->GetBufferSize(), gpu.halfLayout.GetAddressOf()))) {
				return false;
			}
			position.Format = DXGI_FORMAT_R32G32B32_FLOAT;
			if (FAILED(a_device->CreateInputLayout(&position, 1, vs->GetBufferPointer(), vs->GetBufferSize(), gpu.fullLayout.GetAddressOf()))) {
				return false;
			}
			D3D11_BUFFER_DESC buffer{};
			buffer.ByteWidth = sizeof(Constants);
			buffer.Usage = D3D11_USAGE_DYNAMIC;
			buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(a_device->CreateBuffer(&buffer, nullptr, gpu.constants.GetAddressOf()))) {
				return false;
			}
			D3D11_SAMPLER_DESC sampler{};
			sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
			sampler.MaxLOD = D3D11_FLOAT32_MAX;
			sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
			if (FAILED(a_device->CreateSamplerState(&sampler, gpu.sampler.GetAddressOf()))) {
				return false;
			}
			for (auto& o : g_optics) {
				o = {};
			}
			g_gpu = std::move(gpu);
			return true;
		}

		struct SavedPipeline
		{
			ID3D11DeviceContext* ctx;
			ComPtr<ID3D11VertexShader> vs;
			ComPtr<ID3D11PixelShader> ps;
			ComPtr<ID3D11GeometryShader> gs;
			ComPtr<ID3D11HullShader> hs;
			ComPtr<ID3D11DomainShader> ds;
			ComPtr<ID3D11InputLayout> layout;
			ComPtr<ID3D11Buffer> vb, ib, vsBuffer, psBuffer;
			ComPtr<ID3D11SamplerState> sampler;
			ComPtr<ID3D11BlendState> blend;
			ComPtr<ID3D11DepthStencilState> depth;
			ComPtr<ID3D11RasterizerState> raster;
			UINT stride = 0, offset = 0, indexOffset = 0, mask = 0, stencil = 0;
			DXGI_FORMAT indexFormat{};
			D3D11_PRIMITIVE_TOPOLOGY topology{};
			float blendFactor[4]{};

			explicit SavedPipeline(ID3D11DeviceContext* a_ctx) :
				ctx(a_ctx)
			{
				ctx->VSGetShader(vs.GetAddressOf(), nullptr, nullptr);
				ctx->PSGetShader(ps.GetAddressOf(), nullptr, nullptr);
				ctx->GSGetShader(gs.GetAddressOf(), nullptr, nullptr);
				ctx->HSGetShader(hs.GetAddressOf(), nullptr, nullptr);
				ctx->DSGetShader(ds.GetAddressOf(), nullptr, nullptr);
				ctx->IAGetInputLayout(layout.GetAddressOf());
				ctx->IAGetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
				ctx->IAGetIndexBuffer(ib.GetAddressOf(), &indexFormat, &indexOffset);
				ctx->IAGetPrimitiveTopology(&topology);
				ctx->VSGetConstantBuffers(0, 1, vsBuffer.GetAddressOf());
				ctx->PSGetConstantBuffers(0, 1, psBuffer.GetAddressOf());
				ctx->PSGetSamplers(0, 1, sampler.GetAddressOf());
				ctx->OMGetBlendState(blend.GetAddressOf(), blendFactor, &mask);
				ctx->OMGetDepthStencilState(depth.GetAddressOf(), &stencil);
				ctx->RSGetState(raster.GetAddressOf());
			}

			~SavedPipeline()
			{
				ctx->VSSetShader(vs.Get(), nullptr, 0);
				ctx->PSSetShader(ps.Get(), nullptr, 0);
				ctx->GSSetShader(gs.Get(), nullptr, 0);
				ctx->HSSetShader(hs.Get(), nullptr, 0);
				ctx->DSSetShader(ds.Get(), nullptr, 0);
				ctx->IASetInputLayout(layout.Get());
				ctx->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
				ctx->IASetIndexBuffer(ib.Get(), indexFormat, indexOffset);
				ctx->IASetPrimitiveTopology(topology);
				ctx->VSSetConstantBuffers(0, 1, vsBuffer.GetAddressOf());
				ctx->PSSetConstantBuffers(0, 1, psBuffer.GetAddressOf());
				ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
				ctx->OMSetBlendState(blend.Get(), blendFactor, mask);
				ctx->OMSetDepthStencilState(depth.Get(), stencil);
				ctx->RSSetState(raster.Get());
			}
		};

		bool UploadConstants(ID3D11DeviceContext* a_ctx, const Constants& a_c)
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(a_ctx->Map(g_gpu.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				return false;
			}
			std::memcpy(mapped.pData, &a_c, sizeof(a_c));
			a_ctx->Unmap(g_gpu.constants.Get(), 0);
			return true;
		}

		// The selected reticle's pass, inside its own draw: the engine has staged
		// its material and depth; we draw the lens glass with our shaders instead.
		bool DrawCorrected(std::uintptr_t a_reticle, const Assembly& a_asm, const Sample& a_sample, std::size_t a_index)
		{
			g_stage.store(Stage::kNativeContext, std::memory_order_relaxed);
			ID3D11DeviceContext* ctx = nullptr;
			std::array<std::uint8_t, 5> mode{};
			// main view only: stereo on (+1, +2) and not our scope render (+4)
			if (!Read(g_contextSlot, ctx) || !Plausible(reinterpret_cast<std::uintptr_t>(ctx)) || !Read(g_renderer, mode) || !mode[1] ||
				!mode[2] || mode[4] || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
				return false;
			}
			const DWORD thread = ::GetCurrentThreadId();
			DWORD unclaimed = 0;
			if (!g_renderThread.compare_exchange_strong(unclaimed, thread, std::memory_order_acq_rel) && unclaimed != thread) {
				return false;
			}
			g_stage.store(Stage::kResources, std::memory_order_relaxed);
			ComPtr<ID3D11Device> device;
			ctx->GetDevice(device.GetAddressOf());
			if (!device || !CreateResources(device.Get()) || !QualifyOptic(device.Get(), ctx, a_asm, a_sample, a_index)) {
				return false;
			}
			// one sight, one treatment: a corrected dot beside a native circle splits
			// the reticle in two (the combat shotgun's tube sight, 10-26)
			for (std::size_t j = 0; j < kMaxReticles; ++j) {
				if (j != a_index && a_sample.candidates[j] && g_optics[j].generation == a_sample.generation &&
					g_optics[j].status == ReticleFit::CacheState::kRejected) {
					g_stage.store(Stage::kUnsupported, std::memory_order_relaxed);
					SetLastReject("another-card-native");
					return false;
				}
			}
			auto& cache = g_optics[a_index];
			const auto glass = a_asm.shapes[cache.frame.lensMesh];
			Constants c;
			if (!MaterialTint(a_reticle, c) || !FillConstants(a_asm, a_sample, a_index, c)) {
				g_stage.store(Stage::kUnsupported, std::memory_order_relaxed);
				if (g_rejectedMaterialGen[a_index] != a_sample.generation) {
					g_rejectedMaterialGen[a_index] = a_sample.generation;
					logger::info(FMT_STRING("reflex reticle: generation {} material values unsupported ('{}'), native draw kept"), a_sample.generation,
						MaterialName(a_reticle));
					SetLastReject("unsupported-material-values");
				}
				return false;
			}
			const auto& vt = cache.copies[0].vertexTint;
			c.tint.x *= vt[0];
			c.tint.y *= vt[1];
			c.tint.z *= vt[2];
			c.tint.w *= vt[3];

			// commit the staged state with a zero-count bind: the reticle first so
			// the engine's cache and t0 agree with its setup, then the glass
			const auto reticleData = At<std::uintptr_t>(a_reticle + kRendererData);
			const auto glassData = At<std::uintptr_t>(glass + kRendererData);
			if (!reticleData || !glassData) {
				return false;
			}
			g_drawShape(reinterpret_cast<void*>(g_renderer), reinterpret_cast<void*>(reticleData), 0, 0);
			g_drawShape(reinterpret_cast<void*>(g_renderer), reinterpret_cast<void*>(glassData), 0, 0);
			SavedPipeline saved(ctx);  // after the glass bind: restore what the engine's cache now records

			g_stage.store(Stage::kDepth, std::memory_order_relaxed);
			UINT viewports = 1;
			D3D11_VIEWPORT viewport{};
			ctx->RSGetViewports(&viewports, &viewport);
			if (viewports != 1 || !std::isfinite(viewport.MinDepth) || !std::isfinite(viewport.MaxDepth) || viewport.MaxDepth <= viewport.MinDepth) {
				return false;
			}
			c.depthRange.x = viewport.MinDepth;
			c.depthRange.y = viewport.MaxDepth - viewport.MinDepth;
			ComPtr<ID3D11DepthStencilView> depthView;
			ctx->OMGetRenderTargets(0, nullptr, depthView.GetAddressOf());
			D3D11_DEPTH_STENCIL_DESC depth{};
			if (!depthView || !saved.depth) {
				return false;
			}
			saved.depth->GetDesc(&depth);
			if (!depth.DepthEnable || (depth.DepthFunc != D3D11_COMPARISON_LESS && depth.DepthFunc != D3D11_COMPARISON_LESS_EQUAL)) {
				return false;
			}
			depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			if (!g_gpu.depth || std::memcmp(&depth, &g_gpu.depthDesc, sizeof(depth)) != 0) {
				ComPtr<ID3D11DepthStencilState> state;
				if (FAILED(device->CreateDepthStencilState(&depth, state.GetAddressOf()))) {
					return false;
				}
				g_gpu.depth = std::move(state);
				g_gpu.depthDesc = depth;
			}
			D3D11_RASTERIZER_DESC raster{};
			if (saved.raster) {
				saved.raster->GetDesc(&raster);
			} else {
				raster.FillMode = D3D11_FILL_SOLID;
				raster.DepthClipEnable = TRUE;
			}
			raster.CullMode = (cache.ids[cache.frame.lensMesh].flags & kTwoSided) ? D3D11_CULL_NONE : D3D11_CULL_BACK;
			const auto rasterIndex = raster.CullMode == D3D11_CULL_NONE ? 1u : 0u;
			auto& rasterState = g_gpu.rasters[rasterIndex];
			if (!rasterState || std::memcmp(&raster, &g_gpu.rasterDescs[rasterIndex], sizeof(raster)) != 0) {
				ComPtr<ID3D11RasterizerState> state;
				if (FAILED(device->CreateRasterizerState(&raster, state.GetAddressOf()))) {
					return false;
				}
				rasterState = std::move(state);
				g_gpu.rasterDescs[rasterIndex] = raster;
			}

			g_stage.store(Stage::kTexture, std::memory_order_relaxed);
			ComPtr<ID3D11ShaderResourceView> texture;
			ctx->PSGetShaderResources(0, 1, texture.GetAddressOf());
			if (!texture && !a_sample.assist) {
				return false;
			}
			if (a_sample.emitter) {
				c.look = { 1.0f, a_sample.glow, a_sample.core, a_sample.hot };
				c.look2.x = a_sample.mask;
				c.look2.y = a_sample.bloom;
				c.look2.w = a_sample.brightness;
			}
			if (!UploadConstants(ctx, c)) {
				return false;
			}
			if (a_sample.assist) {
				const VisibilityProbe::Key key{ c.uvTransform.x, c.uvTransform.y, c.uvTransform.z, c.uvTransform.w, c.tint.x, c.tint.y, c.tint.z };
				const auto result = cache.visibility.Update(device.Get(), ctx, texture.Get(), g_gpu.visibility.Get(), g_gpu.constants.Get(), g_gpu.sampler.Get(), key);
				using Result = VisibilityProbe::Result;
				c.visibility = { a_sample.brightness, result == Result::kDot ? 2.0f : result == Result::kIlluminate ? 1.0f :
																													  0.0f,
					1.0f / (std::max)(cache.visibility.PeakAlpha(), 0.01f), 0.0f };
				const auto now = ::GetTickCount64();
				if (result != Result::kPending && result != cache.reportedVisibility && (!cache.lastVisibilityReport || now - cache.lastVisibilityReport >= 5000)) {
					const char* how = result == Result::kDot ? "generated dot" : result == Result::kIlluminate ? "illuminated art" :
					                                                         result == Result::kOriginal       ? "original art" :
					                                                                                             "analysis unavailable";
					logger::info(FMT_STRING("reflex reticle: '{}' drawn as {} (peak alpha {:.3f})"), NameAt(a_reticle + kName), how,
						cache.visibility.PeakAlpha());
					cache.reportedVisibility = result;
					cache.lastVisibilityReport = now;
				}
				if (!UploadConstants(ctx, c)) {
					return false;
				}
			}

			// the glass, both eyes, two instances
			const bool full = ((cache.ids[cache.frame.lensMesh].descriptor >> 44) & kVfFullPrec) != 0;
			ctx->IASetInputLayout(full ? g_gpu.fullLayout.Get() : g_gpu.halfLayout.Get());
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->VSSetShader(g_gpu.vertex.Get(), nullptr, 0);
			ctx->PSSetShader(g_gpu.pixel.Get(), nullptr, 0);
			ctx->GSSetShader(nullptr, nullptr, 0);
			ctx->HSSetShader(nullptr, nullptr, 0);
			ctx->DSSetShader(nullptr, nullptr, 0);
			ctx->VSSetConstantBuffers(0, 1, g_gpu.constants.GetAddressOf());
			ctx->PSSetConstantBuffers(0, 1, g_gpu.constants.GetAddressOf());
			ctx->PSSetSamplers(0, 1, g_gpu.sampler.GetAddressOf());
			// the art keeps the engine's blend; the emitter and the assisted forms need a defined one
			if (a_sample.emitter && c.visibility.y < 1.5f) {
				ctx->OMSetBlendState(g_gpu.emitterBlend.Get(), nullptr, saved.mask);
			} else if (c.visibility.y > 0.0f) {
				ctx->OMSetBlendState(g_gpu.assistBlend.Get(), nullptr, saved.mask);
			}
			ctx->OMSetDepthStencilState(g_gpu.depth.Get(), saved.stencil);
			ctx->RSSetState(rasterState.Get());
			if (a_sample.epoch != g_epoch.load(std::memory_order_acquire)) {
				g_stage.store(Stage::kSampleInvalidated, std::memory_order_relaxed);
				return false;
			}
			ctx->DrawIndexedInstanced(At<std::uint32_t>(glass + kTriangleCount) * 3, 2, 0, 0, 0);
			g_draws.fetch_add(1, std::memory_order_relaxed);
			g_stage.store(Stage::kDrawn, std::memory_order_relaxed);
			return true;
		}

		bool InWeapon(std::uintptr_t a_geometry, const Sample& a_sample)
		{
			auto node = a_geometry;
			for (std::size_t i = 0; node && i < kMaxParentDepth; ++i, node = At<std::uintptr_t>(node + kParent)) {
				if (Culled(node)) {
					g_stage.store(Stage::kAncestorCulled, std::memory_order_relaxed);
					return false;
				}
				if (node == a_sample.root) {
					return true;
				}
			}
			g_stage.store(Stage::kParentLimit, std::memory_order_relaxed);
			return false;
		}

		bool CorrectPass(std::uintptr_t a_geometry, std::uintptr_t a_shader, const Sample& a_sample, std::size_t a_index)
		{
			if (!InWeapon(a_geometry, a_sample)) {
				return false;
			}
			std::uintptr_t vtbl = 0;
			if (!Read(a_shader, vtbl) || vtbl != g_effectShaderVtbl) {
				g_stage.store(Stage::kOtherShader, std::memory_order_relaxed);
				return false;
			}
			g_effectPasses.fetch_add(1, std::memory_order_relaxed);
			Assembly asm_;
			if (const auto why = Collect(a_geometry, asm_)) {
				g_stage.store(Stage::kOpticChanged, std::memory_order_relaxed);
				if (g_rejectedOpticGen[a_index] != a_sample.generation) {
					g_rejectedOpticGen[a_index] = a_sample.generation;
					logger::info(FMT_STRING("reflex reticle: generation {} '{}' under '{}' not an optic ({}), native draw kept"), a_sample.generation,
						NameAt(a_geometry + kName), NameAt(asm_.parent + kName), why);
					SetLastReject(why);
				}
				return false;
			}
			return DrawCorrected(a_geometry, asm_, a_sample, a_index);
		}

		bool CorrectPassGuarded(std::uintptr_t a_geometry, std::uintptr_t a_shader, const Sample& a_sample, std::size_t a_index)
		{
			__try {
				return CorrectPass(a_geometry, a_shader, a_sample, a_index);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				g_stage.store(Stage::kException, std::memory_order_relaxed);
				return false;
			}
		}

		bool TakeSample(std::uintptr_t a_geometry, Sample& a_out, std::size_t& a_index)
		{
			std::unique_lock lock(g_sampleLock, std::try_to_lock);
			if (!lock.owns_lock()) {
				g_stage.store(Stage::kSampleBusy, std::memory_order_relaxed);
				return false;
			}
			const auto hit = std::find(g_sample.candidates.begin(), g_sample.candidates.end(), a_geometry);
			if (hit == g_sample.candidates.end()) {
				g_stage.store(Stage::kWrongReticle, std::memory_order_relaxed);
				return false;
			}
			a_out = g_sample;
			a_index = static_cast<std::size_t>(hit - g_sample.candidates.begin());
			return true;
		}

		bool Fresh(const Sample& a_sample)
		{
			if (!a_sample.generation) {
				g_stage.store(Stage::kNoSample, std::memory_order_relaxed);
				return false;
			}
			if (a_sample.epoch != g_epoch.load(std::memory_order_acquire)) {
				g_stage.store(Stage::kSampleInvalidated, std::memory_order_relaxed);
				return false;
			}
			if (::GetTickCount64() - a_sample.ms > kSampleMaxAgeMs) {
				g_stage.store(Stage::kSampleStale, std::memory_order_relaxed);
				return false;
			}
			return true;
		}

		bool Witnessed(std::uintptr_t a_geometry)
		{
			for (const auto& w : g_witness) {
				if (a_geometry && w.load(std::memory_order_relaxed) == a_geometry) {
					return true;
				}
			}
			return false;
		}

		// the shared pass draw: every immediate effect pass comes through here
		void DrawPassHook(void* a_pass, void* a_ctx)
		{
			if (g_resetCache.load(std::memory_order_relaxed) && g_renderThread.load(std::memory_order_relaxed) == ::GetCurrentThreadId() &&
				g_resetCache.exchange(false, std::memory_order_acq_rel)) {
				for (auto& o : g_optics) {
					o = {};
				}
			}
			bool replaced = false;
			const auto pass = reinterpret_cast<std::uintptr_t>(a_pass);
			const auto geometry = At<std::uintptr_t>(pass + kPassGeometry);
			if (Witnessed(geometry)) {
				g_reticlePasses.fetch_add(1, std::memory_order_relaxed);
				Sample sample{};
				std::size_t index = 0;
				if (TakeSample(geometry, sample, index) && Fresh(sample)) {
					replaced = CorrectPassGuarded(geometry, At<std::uintptr_t>(pass + kPassShader), sample, index);
				}
			}
			// a failure keeps the native draw; nothing of the engine's was changed
			if (t_correction && t_correction->pass == a_pass) {
				t_correction->drawn = replaced;
			} else if (!replaced) {
				g_originalDrawPass(a_pass, a_ctx);
			}
		}

		// the native technique refused a selected pass: retry once as a plain
		// textured pass that only our correction may draw; else the original packet
		void SubmitSelected(void* a_pass, std::uint8_t a_alpha, void* a_ctx, void** a_packet, const Sample& a_sample)
		{
			if (g_runPass(a_pass, a_alpha, a_ctx)) {
				return;
			}
			const auto technique = At<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_pass) + kPassTechnique);
			bool corrected = false;
			if (technique != kTexturedTechnique) {
				alignas(16) std::array<std::byte, kPassSize> copy;
				std::memcpy(copy.data(), a_pass, kPassSize);
				std::memcpy(copy.data() + kPassTechnique, &kTexturedTechnique, sizeof(kTexturedTechnique));
				CorrectionOnly correction(copy.data());
				(void)g_runPass(copy.data(), a_alpha, a_ctx);
				corrected = correction.drawn;
			}
			if (corrected) {
				g_techniqueRecoveries.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_restorePass(a_ctx);
				if (*a_packet) {
					g_runPackets(a_packet, 1, a_ctx);
				}
			}
			const auto geometry = At<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(a_pass) + kPassGeometry);
			const auto hit = std::find(a_sample.candidates.begin(), a_sample.candidates.end(), geometry);
			const auto index = static_cast<std::size_t>(hit - a_sample.candidates.begin());
			if (index < kMaxReticles && g_rejectedTechniqueGen[index].exchange(a_sample.generation, std::memory_order_relaxed) != a_sample.generation) {
				logger::info(FMT_STRING("reflex reticle: native technique {:#x} refused '{}', {}"), technique, NameAt(geometry + kName),
					corrected ? "drawn as a textured pass" : "original packet kept");
			}
		}

		// cached batches skip the shared draw entry: split out the selected pass
		// so it runs through it, leave every other range on its native route
		void DrawBatchesHook(void* a_batch, std::uint8_t a_alpha, void* a_ctx)
		{
			Sample sample{};
			{
				std::unique_lock lock(g_sampleLock, std::try_to_lock);
				if (!lock.owns_lock()) {
					g_originalDrawBatches(a_batch, a_alpha, a_ctx);
					return;
				}
				sample = g_sample;
			}
			if (!sample.candidates[0] || sample.epoch != g_epoch.load(std::memory_order_acquire) || ::GetTickCount64() - sample.ms > kSampleMaxAgeMs) {
				g_originalDrawBatches(a_batch, a_alpha, a_ctx);
				return;
			}
			const auto batch = reinterpret_cast<std::uintptr_t>(a_batch);
			const auto job = At<void*>(batch + kBatchJob);
			// the native entry's own worker barrier; borrow the arrays only after it
			g_waitForPackets(job);
			const auto ends = At<const std::uint32_t*>(batch + kBatchGroupEnds);
			const auto groups = At<std::uint32_t>(batch + kBatchGroupCount);
			const auto packets = At<void**>(batch + kBatchPackets);
			const auto count = At<std::uint32_t>(batch + kBatchPacketCount);
			void** passes = nullptr;
			if (!packets || !ends || groups > ReticleFit::kMaxPackets || !Read(reinterpret_cast<std::uintptr_t>(job) + kJobPasses, passes) || !passes) {
				g_originalDrawBatches(a_batch, a_alpha, a_ctx);
				return;
			}
			const auto matches = [&](std::uint32_t a_i) {
				const auto pass = reinterpret_cast<std::uintptr_t>(passes[a_i]);
				if (!pass) {
					return false;
				}
				const auto geometry = At<std::uintptr_t>(pass + kPassGeometry);
				if (std::find(sample.candidates.begin(), sample.candidates.end(), geometry) == sample.candidates.end()) {
					return false;
				}
				std::uintptr_t vtbl = 0;
				return Read(At<std::uintptr_t>(pass + kPassShader), vtbl) && vtbl == g_effectShaderVtbl;
			};
			const auto emit = [&](std::uint32_t a_begin, std::uint32_t a_end, bool a_selected) {
				if (!a_selected && packets[a_begin]) {
					// cached submission binds its own shaders and clears the context's technique cache
					g_runPackets(packets + a_begin, a_end - a_begin, a_ctx);
					return;
				}
				if (a_selected) {
					g_packetReroutes.fetch_add(1, std::memory_order_relaxed);
				}
				for (auto i = a_begin; i < a_end; ++i) {
					if (!a_selected) {
						(void)g_runPass(passes[i], a_alpha, a_ctx);
					} else {
						SubmitSelected(passes[i], a_alpha, a_ctx, packets + i, sample);
					}
				}
				g_restorePass(a_ctx);
			};
			if (!ReticleFit::Route({ ends, groups }, count, matches, emit)) {
				g_originalDrawBatches(a_batch, a_alpha, a_ctx);
			}
		}

		// game thread: the reticle cards under the weapon, hidden variants too
		// (a live draw and visible ancestry are checked at draw time)
		struct Selection
		{
			std::array<std::uintptr_t, kMaxReticles> picked{};
			std::uint32_t excluded = 0;
			std::uint32_t shapes = 0;
			bool truncated = false;
		};

		// A magnified scope's reticle is in its image plane, not at infinity: the
		// lens kinds' cards stay as they are (the STS lens moves them), and so do
		// a combo sight's in-tube cards.
		std::uintptr_t ScopeNodeAbove(std::uintptr_t a_obj)
		{
			for (std::size_t i = 0; a_obj && i < kMaxParentDepth; ++i, a_obj = At<std::uintptr_t>(a_obj + kParent)) {
				const auto name = NameAt(a_obj + kName);
				if (name.size() == 7 && ReticleFit::detail::Contains(name, "p-scope")) {
					return a_obj;
				}
			}
			return 0;
		}

		bool Magnified(std::uintptr_t a_shape, const Optic::Lens& a_lens)
		{
			const auto scope = ScopeNodeAbove(a_shape);
			if (!scope) {
				return false;
			}
			// Optic rescans every 30 frames and a swap keeps the hand's Weapon node,
			// so right after one it still describes the last sight: wait for it
			if (scope != a_lens.pScope || !a_lens.result) {
				return true;
			}
			using OpticClass::Kind;
			switch (a_lens.result->kind) {
			case Kind::kStsMagnified:
			case Kind::kStsNoFade:
			case Kind::kStsFarReticle:
				return true;
			case Kind::kCombo:
				for (std::uint32_t i = 0; i < a_lens.in->count && i < OpticClass::kMaxShapes; ++i) {
					if (a_lens.nodes[i] == a_shape) {
						return a_lens.in->shapes[i].centre[1] - a_lens.result->eyepiece < OpticClass::kCollimatedFrom;
					}
				}
				return true;
			default:
				return false;
			}
		}

		void Select(std::uintptr_t a_root, Selection& a_out)
		{
			const auto lens = Optic::Current();
			std::array<std::uintptr_t, 512> pending{};
			std::size_t n = 1, visited = 0, found = 0;
			pending[0] = a_root;
			while (n && visited++ < pending.size()) {
				const auto node = pending[--n];
				if (!node) {
					continue;
				}
				if (const auto shape = AsTriShape(node)) {
					++a_out.shapes;
					std::uintptr_t material = 0;
					if (ShapeRole(shape) == Role::kReticle && EffectMaterial(shape, material)) {
						if (Magnified(shape, lens)) {
							++a_out.excluded;
						} else {
							a_out.picked[found++] = shape;
							if (found == a_out.picked.size()) {
								break;
							}
						}
					}
				}
				if (ScopeIdent::IsNiNode(node)) {
					if (ChildCount(node) > pending.size() - n) {
						a_out.truncated = true;
						break;
					}
					for (std::uint16_t i = 0; i < ChildCount(node); ++i) {
						if (const auto c = Child(node, i)) {
							pending[n++] = c;
						}
					}
				}
			}
		}

		bool SelectGuarded(std::uintptr_t a_root, Selection& a_out)
		{
			__try {
				Select(a_root, a_out);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_out = {};
				return false;
			}
		}

		// The bore: P-Scope's +Y when there is one, else the first reticle's
		// assembly; the zero turns it about that frame's own X and Z.
		bool Bore(std::uintptr_t a_reticle, Vec3& a_out)
		{
			const auto pScope = ScopeNodeAbove(a_reticle);
			const auto frame = pScope ? pScope : At<std::uintptr_t>(a_reticle + kParent);
			if (!frame) {
				return false;
			}
			const auto w = ReadXform(frame + kWorld);
			constexpr float kMoa = 3.14159265f / (180.0f * 60.0f);
			const float up = std::tan(static_cast<float>(*Settings::reflexZeroUpMoa) * kMoa);
			const float right = std::tan(static_cast<float>(*Settings::reflexZeroRightMoa) * kMoa);
			const Vec3 x{ w.r[0][0], w.r[0][1], w.r[0][2] }, y{ w.r[1][0], w.r[1][1], w.r[1][2] }, z{ w.r[2][0], w.r[2][1], w.r[2][2] };
			using namespace ReticleFit::detail;
			return Normalize(Add(y, Add(Mul(z, up), Mul(x, right))), a_out);
		}

		bool BoreGuarded(std::uintptr_t a_reticle, Vec3& a_out)
		{
			__try {
				return Bore(a_reticle, a_out);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void NameInto(std::uintptr_t a_shape, char (&a_out)[64])
		{
			a_out[0] = '\0';
			if (!a_shape) {
				return;
			}
			const auto s = NameAt(a_shape + kName);
			const auto n = (std::min)(s.size(), sizeof(a_out) - 1);
			std::memcpy(a_out, s.data(), n);
			a_out[n] = '\0';
		}

		// a frame that does not publish takes back the last one
		void Revoke()
		{
			if (g_published) {
				g_published = false;
				g_epoch.fetch_add(1, std::memory_order_acq_rel);
			}
		}

		bool ScopeMenuOpen()
		{
			static const RE::BSFixedString kScopeMenu{ "ScopeMenu" };
			const auto* ui = RE::UI::GetSingleton();
			return ui && ui->GetMenuOpen(kScopeMenu);
		}

		bool ResolveRip(std::uintptr_t a_site, std::array<std::uint8_t, 3> a_prefix, std::uintptr_t& a_out)
		{
			const auto base = REL::Module::get().base();
			std::array<std::uint8_t, 7> bytes{};
			if (!ReadBytes(base + a_site, bytes.data(), bytes.size()) || !std::equal(a_prefix.begin(), a_prefix.end(), bytes.begin())) {
				logger::warn(FMT_STRING("reflex reticle: unexpected bytes at +{:#x}"), a_site);
				return false;
			}
			std::int32_t rel = 0;
			std::memcpy(&rel, bytes.data() + 3, sizeof(rel));
			a_out = base + a_site + 7 + static_cast<std::intptr_t>(rel);
			return true;
		}

		// The DLL behind a patched entry: E9 rel32 or FF 25 [rip], through a
		// trampoline or two. Empty when it leads nowhere we can name.
		std::string PatchOwner(std::uintptr_t a_site)
		{
			auto at = a_site;
			for (int hop = 0; hop < 4; ++hop) {
				std::array<std::uint8_t, 6> b{};
				if (!ReadBytes(at, b.data(), b.size())) {
					return {};
				}
				std::int32_t rel = 0;
				if (b[0] == 0xE9) {
					std::memcpy(&rel, b.data() + 1, sizeof(rel));
					at = at + 5 + static_cast<std::intptr_t>(rel);
				} else if (b[0] == 0xFF && b[1] == 0x25) {
					std::memcpy(&rel, b.data() + 2, sizeof(rel));
					if (!ReadBytes(at + 6 + static_cast<std::intptr_t>(rel), &at, sizeof(at))) {
						return {};
					}
				} else if (hop == 0) {
					return {};
				}
				HMODULE mod = nullptr;
				wchar_t path[MAX_PATH]{};
				if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCWSTR>(at), &mod) &&
					mod && ::GetModuleFileNameW(mod, path, MAX_PATH)) {
					const std::wstring_view w(path);
					std::string name;
					for (const auto c : w.substr(w.find_last_of(L"\\/") + 1)) {
						name.push_back(c < 128 ? static_cast<char>(c) : '?');
					}
					return name;
				}
			}
			return {};
		}

		bool Executable(std::uintptr_t a_address)
		{
			MEMORY_BASIC_INFORMATION page{};
			if (!::VirtualQuery(reinterpret_cast<const void*>(a_address), &page, sizeof(page)) || page.State != MEM_COMMIT || (page.Protect & PAGE_GUARD)) {
				return false;
			}
			const auto p = page.Protect & 0xff;
			return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
		}

		bool CompileShaders()
		{
			const auto compile = [](const char* a_entry, const char* a_profile, ComPtr<ID3DBlob>& a_out) {
				ComPtr<ID3DBlob> errors;
				const auto hr = D3DCompile(ReticleShader::kSource, sizeof(ReticleShader::kSource) - 1, "TrueScopesReticle", nullptr, nullptr,
					a_entry, a_profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out.GetAddressOf(), errors.GetAddressOf());
				if (FAILED(hr)) {
					logger::error(FMT_STRING("reflex reticle: shader {} failed: {}"), a_entry,
						errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler output");
				}
				return SUCCEEDED(hr);
			};
			return compile("VSMain", "vs_5_0", g_vsCode) && compile("PSMain", "ps_5_0", g_psCode) && compile("CSVisibility", "cs_5_0", g_csCode);
		}
	}

	bool Install()
	{
		if (g_installed) {
			return true;
		}
		if (!CompileShaders()) {
			return false;
		}
		const auto base = REL::Module::get().base();
		bool ok = true;
		const auto expect = [&](std::uintptr_t a_rva, std::span<const std::uint8_t> a_bytes, const char* a_what) {
			std::array<std::uint8_t, 32> actual{};
			if (a_bytes.size() > actual.size() || !ReadBytes(base + a_rva, actual.data(), a_bytes.size()) ||
				!std::equal(a_bytes.begin(), a_bytes.end(), actual.begin())) {
				if (const auto owner = PatchOwner(base + a_rva); !owner.empty()) {
					logger::warn(FMT_STRING("reflex reticle: {} at +{:#x} is already patched by {}; that plugin draws the reticles"), a_what, a_rva,
						owner);
				} else {
					logger::warn(FMT_STRING("reflex reticle: {} does not match at +{:#x}"), a_what, a_rva);
				}
				ok = false;
			}
		};
		ok = ResolveRip(Addr::kReticleContextSlotSite, { 0x48, 0x8B, 0x15 }, g_contextSlot) && ok;
		ok = ResolveRip(Addr::kReticleStereoSlotSite, { 0x48, 0x8B, 0x1D }, g_stereoSlot) && ok;
		ok = ResolveRip(Addr::kReticleRendererSite, { 0x48, 0x8D, 0x0D }, g_renderer) && ok;
		ok = ResolveRip(Addr::kEffectMaterialVtblSite, { 0x48, 0x8D, 0x05 }, g_effectMaterialVtbl) && ok;
		ok = ResolveRip(Addr::kEffectShaderVtblSite, { 0x48, 0x8D, 0x05 }, g_effectShaderVtbl) && ok;
		std::uintptr_t materialWitness = 0, shaderWitness = 0;
		ok = ResolveRip(Addr::kEffectMaterialVtblSite2, { 0x48, 0x8D, 0x05 }, materialWitness) && ok;
		ok = ResolveRip(Addr::kEffectShaderVtblSite2, { 0x48, 0x8D, 0x05 }, shaderWitness) && ok;
		if (g_effectMaterialVtbl != materialWitness || g_effectShaderVtbl != shaderWitness || g_effectShaderVtbl != base + Addr::kBSEffectShaderVtbl ||
			g_renderer != base + Addr::kRendererInstance) {
			logger::warn(FMT_STRING("reflex reticle: vtables or renderer disagree (material {:#x}/{:#x}, shader {:#x}/{:#x}, renderer {:#x})"),
				g_effectMaterialVtbl, materialWitness, g_effectShaderVtbl, shaderWitness, g_renderer);
			ok = false;
		}
		// the two entries we patch must be exactly as expected
		expect(Addr::kReticleDrawPass, kDrawPassEntry, "the pass draw entry");
		expect(Addr::kReticleDrawBatches, kDrawBatchesEntry, "the batch dispatch entry");
		const auto runPass = ReticleFit::InspectEntry(base + Addr::kReticleRunPass, kRunPassEntry, ReadBytes, Executable);
		if (!runPass.valid) {
			logger::warn("reflex reticle: RunPass entry is not one we can call"sv);
			ok = false;
		} else if (runPass.detour) {
			logger::info(FMT_STRING("reflex reticle: RunPass carries another mod's detour to {:#x}; calling through it"), runPass.detour);
		}
		// the engine layouts the draw path reads
		expect(Addr::kReticleWaitForPackets, std::array<std::uint8_t, 8>{ 0x40, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x33, 0xC0 }, "WaitForPackets");
		expect(Addr::kReticleRunPackets, std::array<std::uint8_t, 10>{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10 }, "RunPackets");
		expect(Addr::kReticleRestorePass, std::array<std::uint8_t, 9>{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 }, "RestorePass");
		expect(0x28cf0e1, std::array<std::uint8_t, 10>{ 0xF3, 0x0F, 0x10, 0x47, 0x38, 0xF3, 0x0F, 0x59, 0x47, 0x74 }, "material colour x emission");
		expect(0x28cef3a, std::array<std::uint8_t, 5>{ 0x8B, 0x47, 0x0C, 0x89, 0x02 }, "material uv");
		expect(0x1d9a695, std::array<std::uint8_t, 4>{ 0x48, 0xC1, 0xE8, 0x06 }, "vertex desc uv offset");
		expect(0x28ceefa, std::array<std::uint8_t, 4>{ 0x4C, 0x8B, 0x47, 0x48 }, "material texture");
		expect(0x28cef0b, std::array<std::uint8_t, 4>{ 0x4D, 0x8B, 0x40, 0x38 }, "texture renderer data");
		expect(0x27dc180, std::array<std::uint8_t, 3>{ 0x4D, 0x8B, 0x10 }, "draw record");
		expect(0x1c24d45, std::array<std::uint8_t, 4>{ 0x48, 0x8D, 0x4B, 0x10 }, "object name");
		expect(0x1c241f6, std::array<std::uint8_t, 4>{ 0x48, 0x8D, 0x4B, 0x10 }, "object name");
		expect(0x1d568ef, std::array<std::uint8_t, 7>{ 0xC6, 0x83, 0x98, 0x01, 0x00, 0x00, 0x08 }, "geometry type +0x198");
		expect(0x1d569ad, std::array<std::uint8_t, 7>{ 0xC6, 0x83, 0x98, 0x01, 0x00, 0x00, 0x08 }, "geometry type +0x198");
		expect(0x28d1247, std::array<std::uint8_t, 4>{ 0x0F, 0xB6, 0x41, 0x2A }, "alpha reference");
		expect(0x28d4e1d, std::array<std::uint8_t, 4>{ 0x0F, 0xB6, 0x41, 0x2A }, "alpha reference");
		expect(0x2890396, std::array<std::uint8_t, 4>{ 0x44, 0x89, 0x4B, 0x48 }, "pass technique");
		expect(0x2890683, std::array<std::uint8_t, 6>{ 0x41, 0xB8, 0x58, 0x00, 0x00, 0x00 }, "pass size");
		expect(0x279f61b, std::array<std::uint8_t, 3>{ 0x83, 0xC8, 0x04 }, "textured technique bit");
		if (!ok) {
			logger::warn("reflex reticle NOT installed (engine mismatch) - reticles stay native"sv);
			return false;
		}
		g_drawShape = reinterpret_cast<DrawShape_t>(base + Addr::kReticleDrawShape);
		g_waitForPackets = reinterpret_cast<ContextFn_t>(base + Addr::kReticleWaitForPackets);
		g_runPackets = reinterpret_cast<RunPackets_t>(base + Addr::kReticleRunPackets);
		g_runPass = reinterpret_cast<RunPass_t>(base + Addr::kReticleRunPass);
		g_restorePass = reinterpret_cast<ContextFn_t>(base + Addr::kReticleRestorePass);

		// originals: the displaced prologue, then jmp [rip+0] back past it
		auto& trampoline = F4SE::GetTrampoline();
		const auto thunk = [&](std::uintptr_t a_entry, std::size_t a_displaced) {
			auto* code = static_cast<std::uint8_t*>(trampoline.allocate(a_displaced + 14));
			const auto resume = a_entry + a_displaced;
			std::memcpy(code, reinterpret_cast<const void*>(a_entry), a_displaced);
			const std::uint8_t jmp[6]{ 0xFF, 0x25, 0, 0, 0, 0 };
			std::memcpy(code + a_displaced, jmp, sizeof(jmp));
			std::memcpy(code + a_displaced + 6, &resume, sizeof(resume));
			::FlushInstructionCache(::GetCurrentProcess(), code, a_displaced + 14);
			return code;
		};
		g_originalDrawPass = reinterpret_cast<DrawPass_t>(thunk(base + Addr::kReticleDrawPass, kDrawPassDisplaced));
		g_originalDrawBatches = reinterpret_cast<DrawBatches_t>(thunk(base + Addr::kReticleDrawBatches, kDrawBatchesDisplaced));
		trampoline.write_branch<6>(base + Addr::kReticleDrawPass, &DrawPassHook);
		trampoline.write_branch<5>(base + Addr::kReticleDrawBatches, &DrawBatchesHook);
		::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<const void*>(base + Addr::kReticleDrawPass), kDrawPassDisplaced);
		::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<const void*>(base + Addr::kReticleDrawBatches), kDrawBatchesDisplaced);
		g_installed = true;
		{
			std::lock_guard l(g_diagLock);
			g_diag.installed = true;
		}
		logger::info("reflex reticle hooks installed (pass draw + cached batches)"sv);
		return true;
	}

	void Tick(std::uintptr_t a_player)
	{
		if (!g_installed) {
			return;
		}
		const bool on = *Settings::reflexReticleEnabled;
		const auto weapon = on ? ScopeIdent::FirstPersonWeaponNode(a_player) : 0;
		if (weapon != g_weapon) {
			g_weapon = weapon;
			++g_generation;
			g_nextSelect = 0;
			Revoke();
		}
		if (!on || !weapon || ScopeMenuOpen()) {
			Revoke();
			return;
		}
		const auto now = ::GetTickCount64();
		if (now >= g_nextSelect) {
			g_nextSelect = now + kReselectMs;
			Selection sel;
			SelectGuarded(weapon, sel);
			if (sel.picked != g_selected || sel.excluded != g_excluded) {
				if (sel.picked != g_selected) {
					++g_generation;  // another sight; the Weapon node is the same one
				}
				g_selected = sel.picked;
				g_excluded = sel.excluded;
				std::uint32_t n = 0;
				for (const auto p : g_selected) {
					n += p != 0;
				}
				logger::info(FMT_STRING("reflex reticle: generation {} - {} reticle card(s) on the weapon, {} left to a magnified optic ({} shapes{})"),
					g_generation, n, g_excluded, sel.shapes, sel.truncated ? ", walk truncated" : "");
				g_stage.store(n ? Stage::kAwaitingPass : Stage::kNoSample, std::memory_order_relaxed);
				std::lock_guard l(g_diagLock);
				g_diag.candidates = n;
				g_diag.excluded = g_excluded;
				for (std::size_t i = 0; i < kMaxReticles; ++i) {
					NameInto(g_selected[i], g_diag.names[i]);
				}
			}
		}
		Vec3 bore{};
		if (!g_selected[0] || !BoreGuarded(g_selected[0], bore)) {
			Revoke();
			return;
		}
		const float scale = std::clamp(static_cast<float>(*Settings::reflexReticleScale), 0.1f, 4.0f);
		const float dotScale = std::clamp(static_cast<float>(*Settings::reflexReticleDotScale), 0.1f, 4.0f);
		std::unique_lock lock(g_sampleLock, std::try_to_lock);
		if (!lock.owns_lock()) {
			return;  // the render thread has it; the last sample stands for one more frame
		}
		const auto knob = [](double a_v, float a_lo, float a_hi) { return std::clamp(static_cast<float>(a_v), a_lo, a_hi); };
		g_sample = { { bore, false }, g_generation, g_epoch.load(std::memory_order_acquire), now, weapon, g_selected, bore, kHalfAngle * scale,
			knob(*Settings::reflexReticleBrightness, 0.25f, 16.0f), *Settings::reflexReticleAssist, *Settings::reflexReticleLook == 1,
			knob(*Settings::reflexReticleGlow, 0.0f, 2.0f), knob(*Settings::reflexReticleBloom, 0.0f, 1.0f),
			knob(*Settings::reflexReticleCore, 0.1f, 0.9f), knob(*Settings::reflexReticleHot, 0.0f, 1.0f), knob(*Settings::reflexReticleMask, 0.0f, 1.0f),
			kHalfAngle * dotScale };
		for (std::size_t i = 0; i < kMaxReticles; ++i) {
			g_witness[i].store(g_selected[i], std::memory_order_relaxed);
		}
		g_published = true;
		std::lock_guard l(g_diagLock);
		g_diag.generation = g_generation;
		g_diag.bore[0] = bore.x;
		g_diag.bore[1] = bore.y;
		g_diag.bore[2] = bore.z;
		g_diag.halfAngle = kHalfAngle * scale;
	}

	void Clear()
	{
		g_published = false;
		g_epoch.fetch_add(1, std::memory_order_acq_rel);
		g_resetCache.store(true, std::memory_order_release);
		g_nextSelect = 0;
	}

	Diag GetDiag()
	{
		Diag d{};
		{
			std::lock_guard l(g_diagLock);
			d = g_diag;
		}
		d.stage = StageName(g_stage.load(std::memory_order_relaxed));
		d.reticlePasses = g_reticlePasses.load(std::memory_order_relaxed);
		d.effectPasses = g_effectPasses.load(std::memory_order_relaxed);
		d.draws = g_draws.load(std::memory_order_relaxed);
		d.packetReroutes = g_packetReroutes.load(std::memory_order_relaxed);
		d.techniqueRecoveries = g_techniqueRecoveries.load(std::memory_order_relaxed);
		d.geometryReloads = g_geometryReloads.load(std::memory_order_relaxed);
		d.poseRefits = g_poseRefits.load(std::memory_order_relaxed);
		return d;
	}
}
