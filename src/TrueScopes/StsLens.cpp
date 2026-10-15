#include "TrueScopes/StsLens.h"

#include "Settings/Settings.h"
#include "TrueScopes/Addresses.h"
#include "TrueScopes/LensComposite.h"
#include "TrueScopes/Optic.h"
#include "TrueScopes/ScopeIdent.h"

namespace TrueScopes::StsLens
{
	namespace
	{
		// VR NiAVObject
		constexpr std::uintptr_t kNameInObject = 0x10;
		constexpr std::uintptr_t kParent = 0x28;
		constexpr std::uintptr_t kLocalRotate = 0x30;  // rows of 4 floats, row i = local axis i in the parent
		constexpr std::uintptr_t kLocalTranslate = 0x60;
		constexpr std::uintptr_t kLocalScale = 0x6c;
		constexpr std::uintptr_t kWorldRotate = 0x70;
		constexpr std::uintptr_t kWorldTranslate = 0xa0;
		constexpr std::uintptr_t kWorldScale = 0xac;
		constexpr std::uintptr_t kObjFlags = 0x108;  // bit 0 hidden
		constexpr std::uintptr_t kNiAVObjectUpdate = 0x1c22fb0;  // NiAVObject::Update(obj, NiUpdateData), as ScopeRender

		constexpr std::uintptr_t kScopeParentInPlayer = 0x7d0;
		// WSScopeModel (Addresses.h kWidgetModelSingleton): render_circle:0 is the disc;
		// render_square, both housings, the three screen fades and render_UI stay hidden
		constexpr std::uintptr_t kRenderCircle = 0x40;
		constexpr std::uintptr_t kOtherWidgetShapes[] = { 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80 };
		constexpr float          kRenderCircleRadius = 7.852f;  // render_circle:0 at scale 1

		// the reticle sits this far in front of the disc
		constexpr float kCardLift = 0.05f;

		struct Xf
		{
			float r[3][3];  // r[i] = axis i in world
			float t[3];
			float s;
		};

		struct Flag
		{
			std::uintptr_t node;
			std::uint8_t   saved;
		};

		struct Moved
		{
			std::uintptr_t node;
			float          t[3];
			float          s;
			char           name[OpticClass::kNameLen];
		};

		struct Local
		{
			float rot[12];
			float t[3];
			float s;
		};

		bool           g_engaged = false;
		std::uintptr_t g_pScope = 0;
		std::uintptr_t g_sp = 0;
		Local          g_spSaved{};
		float          g_spWrote[3]{};
		bool           g_spWritten = false;
		std::uintptr_t g_circle = 0;
		std::uintptr_t g_cacheOld = 0;
		bool           g_cacheOurs = false;
		Flag           g_flags[40]{};
		std::uint32_t  g_flagCount = 0;
		std::uint32_t  g_widgetFlags = 0;  // the first g_widgetFlags of g_flags are the widget's
		std::uint32_t  g_partsHidden = 0;
		std::uint32_t  g_engages = 0;
		Moved          g_moved[8]{};
		std::uint32_t  g_movedCount = 0;
		float          g_cardScale = 0.0f;
		std::uintptr_t g_cam = 0;
		Local          g_camSaved{};
		bool           g_camTouched = false;
		float          g_camWant[3]{};
		float          g_camAxis[3]{};
		std::atomic_bool g_live{ false };

		std::mutex g_diagLock;
		Diag       g_diag{};

		using NiAVObjectUpdate_t = void (*)(std::uintptr_t, void*);

		void Update(std::uintptr_t a_node)
		{
			alignas(16) std::uint8_t upd[0x30]{};  // zeroed NiUpdateData, as ScopeRender's WriteScopeParent
			reinterpret_cast<NiAVObjectUpdate_t>(REL::Module::get().base() + kNiAVObjectUpdate)(a_node, upd);
		}

		bool ReadXf(std::uintptr_t a_node, Xf& a_out)
		{
			const auto* m = reinterpret_cast<const float*>(a_node + kWorldRotate);
			const auto* t = reinterpret_cast<const float*>(a_node + kWorldTranslate);
			for (int i = 0; i < 3; ++i) {
				for (int k = 0; k < 3; ++k) {
					a_out.r[i][k] = m[i * 4 + k];
					if (!std::isfinite(a_out.r[i][k])) {
						return false;
					}
				}
				a_out.t[i] = t[i];
				if (!std::isfinite(a_out.t[i])) {
					return false;
				}
			}
			a_out.s = *reinterpret_cast<const float*>(a_node + kWorldScale);
			return std::isfinite(a_out.s) && a_out.s > 1e-4f;
		}

		void ReadLocal(std::uintptr_t a_node, Local& a_out)
		{
			std::memcpy(a_out.rot, reinterpret_cast<const void*>(a_node + kLocalRotate), sizeof(a_out.rot));
			std::memcpy(a_out.t, reinterpret_cast<const void*>(a_node + kLocalTranslate), sizeof(a_out.t));
			a_out.s = *reinterpret_cast<const float*>(a_node + kLocalScale);
		}

		void WriteLocal(std::uintptr_t a_node, const Local& a_l)
		{
			std::memcpy(reinterpret_cast<void*>(a_node + kLocalRotate), a_l.rot, sizeof(a_l.rot));
			std::memcpy(reinterpret_cast<void*>(a_node + kLocalTranslate), a_l.t, sizeof(a_l.t));
			*reinterpret_cast<float*>(a_node + kLocalScale) = a_l.s;
		}

		float Dot(const float* a_a, const float* a_b)
		{
			return a_a[0] * a_b[0] + a_a[1] * a_b[1] + a_a[2] * a_b[2];
		}

		// a world pose as the local transform under a_parent
		Local ToLocal(const Xf& a_parent, const Xf& a_world)
		{
			Local l{};
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					l.rot[i * 4 + j] = Dot(a_world.r[i], a_parent.r[j]);
				}
			}
			const float d[3] = { a_world.t[0] - a_parent.t[0], a_world.t[1] - a_parent.t[1], a_world.t[2] - a_parent.t[2] };
			for (int j = 0; j < 3; ++j) {
				l.t[j] = Dot(a_parent.r[j], d) / a_parent.s;
			}
			l.s = a_world.s / a_parent.s;
			return l;
		}

		// a point in P-Scope's frame, to world
		void ToWorld(const Optic::Frame& a_f, const float (&a_p)[3], float (&a_out)[3])
		{
			for (int i = 0; i < 3; ++i) {
				a_out[i] = a_f.t[i] + a_f.s * (a_p[0] * a_f.rows[0][i] + a_p[1] * a_f.rows[1][i] + a_p[2] * a_f.rows[2][i]);
			}
		}

		bool NameIs(std::uintptr_t a_node, const char* a_name)
		{
			const auto entry = *reinterpret_cast<std::uintptr_t*>(a_node + kNameInObject);
			return entry >= 0x10000 && std::strcmp(reinterpret_cast<const char*>(entry + 0x18), a_name) == 0;
		}

		std::uintptr_t Model()
		{
			return *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + Addr::kWidgetModelSingleton);
		}

		void SetHidden(std::uintptr_t a_node, bool a_hidden)
		{
			if (!a_node || g_flagCount >= std::size(g_flags)) {
				return;
			}
			auto& f = *reinterpret_cast<std::uint8_t*>(a_node + kObjFlags);
			g_flags[g_flagCount++] = { a_node, f };
			f = a_hidden ? static_cast<std::uint8_t>(f | 1u) : static_cast<std::uint8_t>(f & ~1u);
		}

		bool Visible(const OpticClass::Input& a_in, std::uint32_t a_i)
		{
			const auto& s = a_in.shapes[a_i];
			const bool  twoModel = (a_in.keyNodes & OpticClass::kScopeAiming) && (a_in.keyNodes & OpticClass::kScopeNormal);
			return !s.hidden && !(twoModel && s.underNormal) && std::isfinite(s.radius) && s.radius > 0.0f;
		}

		// the census card rule (OpticClass::Classify), in the tube
		bool InTubeCard(const OpticClass::Input& a_in, const OpticClass::Result& a_r, std::uint32_t a_i)
		{
			const auto& s = a_in.shapes[a_i];
			const auto  part = OpticClass::Classify(s);
			const bool  card = Visible(a_in, a_i) && s.radius > 1e-3f && part != OpticClass::Part::kLaser &&
			                  (part == OpticClass::Part::kCard || OpticClass::detail::StartsWith(s.name, "tritiumdot") ||
								  OpticClass::detail::StartsWith(s.name, "reticle") || OpticClass::detail::StartsWith(s.name, "dot"));
			return card && s.centre[1] - a_r.eyepiece < OpticClass::kCollimatedFrom;
		}

		// ScopeParent onto the eyepiece: its axes P-Scope's (Y down-range, the disc
		// facing the eye), render_circle:0 sized to the fade
		bool PlaceDiscGuarded(std::uintptr_t a_player, const Optic::Lens& a_lens, float a_radius, float (&a_where)[3]) noexcept
		{
			__try {
				const auto sp = *reinterpret_cast<std::uintptr_t*>(a_player + kScopeParentInPlayer);
				if (!sp || sp != g_sp) {
					return false;
				}
				const auto parent = *reinterpret_cast<std::uintptr_t*>(sp + kParent);
				Xf         pw{};
				if (!parent || !ReadXf(parent, pw)) {
					return false;
				}
				Xf want{};
				for (int i = 0; i < 3; ++i) {
					for (int k = 0; k < 3; ++k) {
						want.r[i][k] = a_lens.frame.rows[i][k];
					}
				}
				ToWorld(a_lens.frame, a_lens.result->centre, want.t);
				want.s = a_radius * a_lens.frame.s / kRenderCircleRadius;
				const auto l = ToLocal(pw, want);
				WriteLocal(sp, l);
				Update(sp);
				std::memcpy(g_spWrote, l.t, sizeof(g_spWrote));
				g_spWritten = true;
				std::memcpy(a_where, want.t, sizeof(a_where));
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// The in-tube cards onto the disc plane: p' = k p + (A - k B) in P-Scope's
		// frame, A the disc centre lifted toward the eye, B the card's own point on
		// the axis. A similarity, so in world it is the same map on the card node's
		// translate with its scale times k; the rotation stays.
		std::uint32_t MoveCardsGuarded(const Optic::Lens& a_lens, float a_radius, float a_scale) noexcept
		{
			__try {
				const auto& in = *a_lens.in;
				const auto& r = *a_lens.result;
				const auto  n = in.count < OpticClass::kMaxShapes ? in.count : static_cast<std::uint32_t>(OpticClass::kMaxShapes);
				float       maxR = 0.0f;
				for (std::uint32_t i = 0; i < n; ++i) {
					if (InTubeCard(in, r, i)) {
						maxR = (std::max)(maxR, in.shapes[i].radius);
					}
				}
				if (!(maxR > 1e-3f)) {
					return 0;
				}
				// a card's bound is its half-diagonal; the reticle spans the card
				const float k = a_radius * 1.41421356f / maxR * a_scale;
				g_cardScale = k;
				const float A[3] = { r.centre[0], r.centre[1] - kCardLift / a_lens.frame.s, r.centre[2] };
				float       aw[3];
				ToWorld(a_lens.frame, A, aw);
				std::uint32_t moved = 0;
				for (std::uint32_t i = 0; i < n && g_movedCount < std::size(g_moved); ++i) {
					const auto node = a_lens.nodes[i];
					if (!InTubeCard(in, r, i) || !node || !NameIs(node, in.shapes[i].name)) {
						continue;
					}
					const auto parent = *reinterpret_cast<std::uintptr_t*>(node + kParent);
					Xf         nw{}, pw{};
					if (!parent || !ReadXf(node, nw) || !ReadXf(parent, pw)) {
						continue;
					}
					const float B[3] = { r.centre[0], in.shapes[i].centre[1], r.centre[2] };
					float       bw[3];
					ToWorld(a_lens.frame, B, bw);
					Xf want = nw;
					for (int c = 0; c < 3; ++c) {
						want.t[c] = k * nw.t[c] + aw[c] - k * bw[c];
					}
					want.s = k * nw.s;
					const auto l = ToLocal(pw, want);
					auto&      m = g_moved[g_movedCount++];
					m.node = node;
					std::memcpy(m.t, reinterpret_cast<const void*>(node + kLocalTranslate), sizeof(m.t));
					m.s = *reinterpret_cast<const float*>(node + kLocalScale);
					strncpy_s(m.name, in.shapes[i].name, _TRUNCATE);
					std::memcpy(reinterpret_cast<void*>(node + kLocalTranslate), l.t, sizeof(l.t));
					*reinterpret_cast<float*>(node + kLocalScale) = l.s;
					Update(node);
					++moved;
				}
				return moved;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		// fade vignette, eyepiece glass and screens off (ours replace them); with
		// stsReticleMode 2 the cards too
		std::uint32_t HideOpticPartsGuarded(const Optic::Lens& a_lens, bool a_parts, bool a_cards) noexcept
		{
			__try {
				const auto& in = *a_lens.in;
				const auto  n = in.count < OpticClass::kMaxShapes ? in.count : static_cast<std::uint32_t>(OpticClass::kMaxShapes);
				std::uint32_t hidden = 0;
				for (std::uint32_t i = 0; i < n; ++i) {
					const auto node = a_lens.nodes[i];
					if (!node || !Visible(in, i) || !NameIs(node, in.shapes[i].name)) {
						continue;
					}
					const auto part = OpticClass::Classify(in.shapes[i]);
					const bool eyepiece = part == OpticClass::Part::kFade || part == OpticClass::Part::kGlass || part == OpticClass::Part::kScreen;
					if ((a_parts && eyepiece) || (a_cards && InTubeCard(in, *a_lens.result, i))) {
						SetHidden(node, true);
						++hidden;
					}
				}
				return hidden;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		// The widget: ScopeParent and render_circle:0 shown, everything else in it
		// hidden, the lens-bind cache on the circle. The cache holds a reference;
		// swap it the way the enable switch does (new in, old kept for the way out).
		bool TakeWidgetGuarded(std::uintptr_t a_player) noexcept
		{
			__try {
				const auto model = Model();
				const auto sp = *reinterpret_cast<std::uintptr_t*>(a_player + kScopeParentInPlayer);
				if (!model || !sp) {
					return false;
				}
				const auto circle = *reinterpret_cast<std::uintptr_t*>(model + kRenderCircle);
				if (!circle || !NameIs(circle, "render_circle:0")) {
					return false;
				}
				g_sp = sp;
				ReadLocal(sp, g_spSaved);
				g_spWritten = false;
				g_circle = circle;
				SetHidden(sp, false);
				SetHidden(circle, false);
				for (const auto off : kOtherWidgetShapes) {
					const auto shape = *reinterpret_cast<std::uintptr_t*>(model + off);
					if (shape && shape != circle) {
						SetHidden(shape, true);
					}
				}
				auto& cell = *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + Addr::kLensBindCache);
				if (cell != circle) {
					reinterpret_cast<RE::NiRefObject*>(circle)->IncRefCount();
					g_cacheOld = cell;
					cell = circle;
					g_cacheOurs = true;
				}
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void ReleaseCacheGuarded() noexcept
		{
			__try {
				if (!g_cacheOurs) {
					return;
				}
				auto& cell = *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + Addr::kLensBindCache);
				if (cell == g_circle) {
					cell = g_cacheOld;  // its reference goes back with it
					reinterpret_cast<RE::NiRefObject*>(g_circle)->DecRefCount();
				} else if (g_cacheOld) {
					// the engine swapped it meanwhile and released ours; drop the one we kept
					reinterpret_cast<RE::NiRefObject*>(g_cacheOld)->DecRefCount();
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			g_cacheOurs = false;
			g_cacheOld = 0;
		}

		void RestoreFlagsGuarded() noexcept
		{
			while (g_flagCount) {
				const auto& f = g_flags[--g_flagCount];
				__try {
					auto& b = *reinterpret_cast<std::uint8_t*>(f.node + kObjFlags);
					b = static_cast<std::uint8_t>((b & ~1u) | (f.saved & 1u));
				} __except (EXCEPTION_EXECUTE_HANDLER) {
				}
			}
		}

		void RestoreCardsGuarded() noexcept
		{
			while (g_movedCount) {
				const auto& m = g_moved[--g_movedCount];
				__try {
					if (NameIs(m.node, m.name)) {
						std::memcpy(reinterpret_cast<void*>(m.node + kLocalTranslate), m.t, sizeof(m.t));
						*reinterpret_cast<float*>(m.node + kLocalScale) = m.s;
						Update(m.node);
					}
				} __except (EXCEPTION_EXECUTE_HANDLER) {
				}
			}
		}

		// ScopeParent back as found, unless something else has written it since
		void RestoreScopeParentGuarded(std::uintptr_t a_player) noexcept
		{
			__try {
				const auto sp = a_player ? *reinterpret_cast<std::uintptr_t*>(a_player + kScopeParentInPlayer) : 0;
				if (sp && sp == g_sp && (!g_spWritten || std::memcmp(reinterpret_cast<const void*>(sp + kLocalTranslate), g_spWrote, sizeof(g_spWrote)) == 0)) {
					WriteLocal(sp, g_spSaved);
					Update(sp);
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			g_sp = 0;
			g_spWritten = false;
		}

		void RestoreCameraGuarded() noexcept
		{
			if (!g_camTouched) {
				return;
			}
			__try {
				WriteLocal(g_cam, g_camSaved);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			g_camTouched = false;
			g_cam = 0;
		}

		void CameraMissGuarded(std::uintptr_t a_cam, float& a_miss, float& a_axis) noexcept
		{
			__try {
				Xf c{};
				if (ReadXf(a_cam, c)) {
					const float d[3] = { c.t[0] - g_camWant[0], c.t[1] - g_camWant[1], c.t[2] - g_camWant[2] };
					a_miss = std::sqrt(Dot(d, d));
					a_axis = Dot(c.r[0], g_camAxis);
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}

		void PublishDiag(const char* a_release = nullptr, const float* a_disc = nullptr, float a_radius = -1.0f)
		{
			std::scoped_lock lock(g_diagLock);
			g_diag.engaged = g_engaged;
			g_diag.cacheOurs = g_cacheOurs;
			g_diag.cardsMoved = g_movedCount;
			g_diag.partsHidden = g_partsHidden;
			g_diag.cardScale = g_cardScale;
			g_diag.engages = g_engages;
			if (a_radius >= 0.0f) {
				g_diag.discRadius = a_radius;
			}
			if (a_disc) {
				std::memcpy(g_diag.disc, a_disc, sizeof(g_diag.disc));
			}
			if (a_release) {
				strncpy_s(g_diag.lastRelease, a_release, _TRUNCATE);
			}
		}

		void Release(std::uintptr_t a_player, std::string_view a_why)
		{
			if (!g_engaged) {
				return;
			}
			// the optic's own nodes only while its 3D is still the one in the scene
			const bool attached = Optic::Attached(g_pScope);
			ReleaseCacheGuarded();
			LensComposite::RestoreReticleQuad();
			if (attached) {
				RestoreCardsGuarded();
			} else {
				g_movedCount = 0;
			}
			if (!attached) {
				// the shapes went with their 3D; the widget's flags are still ours to put back
				g_flagCount = (std::min)(g_flagCount, g_widgetFlags);
			}
			RestoreFlagsGuarded();
			RestoreScopeParentGuarded(a_player);
			RestoreCameraGuarded();
			g_engaged = false;
			g_live.store(false, std::memory_order_relaxed);
			g_pScope = 0;
			g_partsHidden = 0;
			char why[48];
			std::snprintf(why, sizeof(why), "%.*s", static_cast<int>(a_why.size()), a_why.data());
			PublishDiag(why);
			logger::info(FMT_STRING("sts lens: released ({}){}"), a_why, attached ? "" : " - optic 3D gone, its shapes left alone");
		}

		void Engage(std::uintptr_t a_player, const Optic::Lens& a_lens, float a_radius)
		{
			g_flagCount = 0;
			g_movedCount = 0;
			g_cardScale = 0.0f;
			if (!TakeWidgetGuarded(a_player)) {
				RestoreFlagsGuarded();
				static bool s_warned = false;
				if (!s_warned) {
					s_warned = true;
					logger::warn("sts lens: the vanilla scope widget is not there (no WSScopeModel / render_circle:0 / ScopeParent) - no STS lens"sv);
				}
				return;
			}
			g_engaged = true;
			g_pScope = a_lens.pScope;
			g_widgetFlags = g_flagCount;
			const auto mode = *Settings::stsReticleMode;
			g_partsHidden = HideOpticPartsGuarded(a_lens, *Settings::stsHideEyepieceParts, mode == 2);
			if (mode == 1) {
				MoveCardsGuarded(a_lens, a_radius, static_cast<float>(*Settings::stsReticleScale));
			}
			// the weapon's zoom data for the magnification
			ScopeIdent::Request();
			++g_engages;
			const auto& r = *a_lens.result;
			const auto  fade = r.fade >= 0 ? a_lens.in->shapes[r.fade].name : "-";
			PublishDiag();
			logger::info(FMT_STRING("sts lens: engaged on {} (fade '{}', disc r={:.2f}); cache {}; reticle mode {}: {} card(s) moved (x{:.2f}); "
									"{} shape(s) hidden"),
				OpticClass::KindName(r.kind), fade, a_radius, g_cacheOurs ? "taken" : "already on the circle", mode, g_movedCount,
				g_cardScale, g_partsHidden);
		}
	}

	void Frame(std::uintptr_t a_player)
	{
		const auto lens = Optic::Current();
		const bool enabled = *Settings::stsLensEnabled;
		const bool want = enabled && a_player && lens.live && lens.pScope && lens.result->radius > 0.0f;
		if (g_engaged && (!want || lens.pScope != g_pScope)) {
			Release(a_player, !enabled ? "stsLensEnabled off"sv : lens.pScope != g_pScope ? "optic changed"sv : "gate off"sv);
		}
		if (!want) {
			return;
		}
		const float radius = lens.result->radius * std::clamp(static_cast<float>(*Settings::stsLensApertureScale), 0.5f, 2.0f);
		if (!g_engaged) {
			Engage(a_player, lens, radius);
			if (!g_engaged) {
				return;
			}
		}
		float where[3]{};
		if (!PlaceDiscGuarded(a_player, lens, radius, where)) {
			Release(a_player, "disc placement failed"sv);
			return;
		}
		g_live.store(true, std::memory_order_relaxed);
		PublishDiag(nullptr, where, radius * lens.frame.s);
	}

	bool Live()
	{
		return g_live.load(std::memory_order_relaxed);
	}

	bool OwnsWidget()
	{
		return g_engaged;
	}

	void PlaceCamera(std::uintptr_t a_cam)
	{
		if (!g_engaged || !a_cam) {
			return;
		}
		const auto lens = Optic::Current();
		if (lens.pScope != g_pScope) {
			return;
		}
		__try {
			const auto parent = *reinterpret_cast<std::uintptr_t*>(a_cam + kParent);
			Xf         pw{};
			if (!parent || !ReadXf(parent, pw)) {
				return;
			}
			// NiCamera looks down its X, Y up: the scope camera's rows are P-Scope's
			// Y, Z, X (the vanilla rig, measured headless 10-15)
			Xf want{};
			for (int k = 0; k < 3; ++k) {
				want.r[0][k] = lens.frame.rows[1][k];
				want.r[1][k] = lens.frame.rows[2][k];
				want.r[2][k] = lens.frame.rows[0][k];
			}
			float p[3] = { lens.result->centre[0], lens.result->centre[1] + static_cast<float>(*Settings::stsCamForwardUnits) / lens.frame.s,
				lens.result->centre[2] };
			ToWorld(lens.frame, p, want.t);
			want.s = pw.s * *reinterpret_cast<const float*>(a_cam + kLocalScale);
			std::memcpy(g_camWant, want.t, sizeof(g_camWant));
			std::memcpy(g_camAxis, lens.frame.rows[1], sizeof(g_camAxis));
			if (!g_camTouched || g_cam != a_cam) {
				ReadLocal(a_cam, g_camSaved);
				g_cam = a_cam;
				g_camTouched = true;
			}
			auto l = ToLocal(pw, want);
			l.s = g_camSaved.s;
			WriteLocal(a_cam, l);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	void NoteCamera(std::uintptr_t a_cam)
	{
		if (!g_engaged || !a_cam) {
			return;
		}
		float miss = -1.0f, axis = 0.0f;
		CameraMissGuarded(a_cam, miss, axis);
		std::scoped_lock lock(g_diagLock);
		g_diag.camMiss = miss;
		g_diag.camAxis = axis;
	}

	void StandDown(std::string_view a_why)
	{
		const auto player = *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + Addr::kPlayerGlobal);
		Release(player, a_why);
	}

	Diag GetDiag()
	{
		std::scoped_lock lock(g_diagLock);
		return g_diag;
	}
}
