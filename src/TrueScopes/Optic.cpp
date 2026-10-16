#include "TrueScopes/Optic.h"

#include "TrueScopes/Addresses.h"
#include "TrueScopes/Hooks.h"
#include "TrueScopes/PoseGate.h"
#include "TrueScopes/ScopeIdent.h"
#include "TrueScopes/StsLens.h"

namespace TrueScopes::Optic
{
	namespace
	{
		// VR NiAVObject / NiNode
		constexpr std::uintptr_t kNameInObject = 0x10;  // BSFixedString
		constexpr std::uintptr_t kWorldRotate = 0x70;   // rows of 4 floats; a row is that axis in world
		constexpr std::uintptr_t kWorldTranslate = 0xa0;
		constexpr std::uintptr_t kWorldScale = 0xac;
		constexpr std::uintptr_t kObjFlags = 0x108;   // bit 0 hidden
		constexpr std::uintptr_t kChildren = 0x168;
		constexpr std::uintptr_t kChildCount = 0x172;  // u16 loop bound, null holes legal

		// VR BSGeometry and its shader property (flat +0x40; see Reticle.cpp)
		constexpr std::uintptr_t kShaderPropertyInGeometry = 0x178;
		constexpr std::uintptr_t kModelBound = 0x160;  // {centre, radius} in the shape's own space
		constexpr std::uintptr_t kIsGeometryVFunc = 0x40;
		constexpr std::uintptr_t kBSGeometryIsGeometry = 0x1c31c50;
		constexpr std::uintptr_t kPropertyFlags = 0x30;  // u64, technique builder 0x14279f360
		constexpr std::uint64_t  kEnvMap = 1ull << 7;
		constexpr std::uintptr_t kMaterialInProperty = 0x58;
		constexpr std::uintptr_t kBaseTexturePathInMaterial = 0x78;

		constexpr std::uintptr_t kImageSpan = 0x8000000;
		constexpr int            kMaxDepth = 12;
		constexpr std::uint32_t  kRescanFrames = 30;

		// ActorState sits at +0x128 in the player, as in flatrim, but weaponState is
		// one bit higher: bits 2..4 of the dword at +0xC (flatrim's 1..3 read 6 while
		// drawn). Headless 10-15: 3 drawn, 0 after an unequip and after an engine
		// attach, the weapon node's hidden bit agreeing each time. Trusted only once
		// the vtable there is PlayerCharacter's at COL offset 0x128.
		constexpr std::uintptr_t kActorStateInActor = 0x128;
		constexpr std::uintptr_t kStateBitsInActorState = 0xC;
		constexpr std::uint32_t  kWeaponDrawn = 3;

		std::mutex     g_lock;
		Snapshot       g_snap{};  // under g_lock; written by the game thread only
		std::uintptr_t g_lastWeapon = 0;
		std::uint32_t  g_frames = 0;
		std::uint32_t  g_walks = 0;

		PoseGate::Hysteresis g_hyst;
		Frame                g_frame{};  // P-Scope this frame, game thread

		// last logged answer
		OpticClass::Kind g_loggedKind = OpticClass::Kind::kNone;
		std::uintptr_t   g_loggedPScope = 0;
		int              g_loggedFade = -2;
		int              g_loggedGlass = -2;

		bool ReadName(std::uintptr_t a_field, char* a_out, std::size_t a_size)
		{
			a_out[0] = '\0';
			const auto entry = *reinterpret_cast<std::uintptr_t*>(a_field);
			if (entry < 0x10000) {
				return false;
			}
			const auto len = *reinterpret_cast<std::uint32_t*>(entry + 0x10);
			if (len == 0) {
				return false;
			}
			const auto n = (std::min)(static_cast<std::size_t>(len), a_size - 1);
			std::memcpy(a_out, reinterpret_cast<const char*>(entry + 0x18), n);
			a_out[n] = '\0';
			return true;
		}

		// The MSVC complete object locator behind a vtable, or 0
		std::uintptr_t Col(std::uintptr_t a_obj)
		{
			if (a_obj < 0x10000) {
				return 0;
			}
			const auto base = REL::Module::get().base();
			const auto vtbl = *reinterpret_cast<std::uintptr_t*>(a_obj);
			if (vtbl < base || vtbl >= base + kImageSpan) {
				return 0;
			}
			const auto col = *reinterpret_cast<std::uintptr_t*>(vtbl - 8);
			return col >= base && col < base + kImageSpan ? col : 0;
		}

		bool HasTypeName(std::uintptr_t a_obj, const char* a_name)
		{
			const auto col = Col(a_obj);
			if (!col) {
				return false;
			}
			const auto td = REL::Module::get().base() + *reinterpret_cast<std::uint32_t*>(col + 0x0C);
			return std::strcmp(reinterpret_cast<const char*>(td + 0x10), a_name) == 0;
		}

		bool IsGeometry(std::uintptr_t a_obj)
		{
			const auto vtbl = *reinterpret_cast<std::uintptr_t*>(a_obj);
			return vtbl && *reinterpret_cast<std::uintptr_t*>(vtbl + kIsGeometryVFunc) == REL::Module::get().base() + kBSGeometryIsGeometry;
		}

		// "ScopeAiming:2" -> kScopeAiming
		std::uint32_t KeyNodeBit(const char* a_name)
		{
			using namespace OpticClass;
			constexpr std::pair<const char*, std::uint32_t> kKeys[] = {
				{ "scopeaiming", kScopeAiming },
				{ "scopenormal", kScopeNormal },
				{ "scopeviewparts", kScopeViewParts },
				{ "scopealways", kScopeAlways },
				{ "reticlenode", kReticleNode },
			};
			for (const auto& [name, bit] : kKeys) {
				if (detail::BaseIs(a_name, name)) {
					return bit;
				}
			}
			return 0;
		}

		bool ReadFrame(std::uintptr_t a_node, Frame& a_out);

		struct Walker
		{
			const Frame&       frame;
			OpticClass::Input& in;
			std::uintptr_t*    nodes;

			void Shape(std::uintptr_t a_obj, const char* a_name, bool a_hidden, std::uint32_t a_under)
			{
				if (in.count >= OpticClass::kMaxShapes) {
					++in.overflow;
					return;
				}
				auto& s = in.shapes[in.count];
				s = {};
				strncpy_s(s.name, a_name, _TRUNCATE);
				const auto property = *reinterpret_cast<std::uintptr_t*>(a_obj + kShaderPropertyInGeometry);
				s.effect = HasTypeName(property, ".?AVBSEffectShaderProperty@@");
				if (s.effect || HasTypeName(property, ".?AVBSLightingShaderProperty@@")) {
					ReadName(property + kNameInObject, s.material, sizeof(s.material));
					s.envMap = (*reinterpret_cast<const std::uint64_t*>(property + kPropertyFlags) & kEnvMap) != 0;
				}
				if (s.effect) {
					if (const auto material = *reinterpret_cast<std::uintptr_t*>(property + kMaterialInProperty)) {
						ReadName(material + kBaseTexturePathInMaterial, s.texture, sizeof(s.texture));
					}
				}
				s.hidden = a_hidden;
				s.underAiming = (a_under & OpticClass::kScopeAiming) != 0;
				s.underNormal = (a_under & OpticClass::kScopeNormal) != 0;
				s.underReticleNode = (a_under & OpticClass::kReticleNode) != 0;
				// The model bound through the shape's world transform, then into the attach
				// frame: local = rows * (w - t) / s. Not the world bound (+0xb0): nothing
				// refreshes it when FRIK poses a drawn weapon, and headless 10-15 it sat
				// 4.7 units off on every shape while the transforms were current.
				Frame shape{};
				ReadFrame(a_obj, shape);
				const auto* b = reinterpret_cast<const float*>(a_obj + kModelBound);
				float       w[3];
				for (int i = 0; i < 3; ++i) {
					w[i] = shape.t[i] + shape.s * (b[0] * shape.rows[0][i] + b[1] * shape.rows[1][i] + b[2] * shape.rows[2][i]);
				}
				const float d[3] = { w[0] - frame.t[0], w[1] - frame.t[1], w[2] - frame.t[2] };
				for (int i = 0; i < 3; ++i) {
					s.centre[i] = (frame.rows[i][0] * d[0] + frame.rows[i][1] * d[1] + frame.rows[i][2] * d[2]) / frame.s;
				}
				s.radius = b[3] * shape.s / frame.s;
				nodes[in.count] = a_obj;
				++in.count;
			}

			// the STS toggle nodes ship hidden and are shown by animation, so their own
			// flag does not count
			void Walk(std::uintptr_t a_obj, int a_depth, bool a_hidden, std::uint32_t a_under)
			{
				if (!a_obj || a_depth > kMaxDepth) {
					return;
				}
				char name[OpticClass::kNameLen];
				ReadName(a_obj + kNameInObject, name, sizeof(name));
				if (ScopeIdent::IsNiNode(a_obj)) {
					const auto key = KeyNodeBit(name);
					in.keyNodes |= key;
					const bool toggle = key == OpticClass::kScopeAiming || key == OpticClass::kScopeNormal || key == OpticClass::kScopeAlways;
					const bool hidden = a_hidden || (a_depth > 0 && !toggle && (*reinterpret_cast<const std::uint8_t*>(a_obj + kObjFlags) & 1u));
					const auto kids = *reinterpret_cast<std::uintptr_t*>(a_obj + kChildren);
					const auto count = *reinterpret_cast<std::uint16_t*>(a_obj + kChildCount);
					for (std::uint16_t i = 0; kids && i < count; ++i) {
						Walk(*reinterpret_cast<std::uintptr_t*>(kids + i * 8ull), a_depth + 1, hidden, a_under | key);
					}
					return;
				}
				if (IsGeometry(a_obj)) {
					Shape(a_obj, name, a_hidden || (*reinterpret_cast<const std::uint8_t*>(a_obj + kObjFlags) & 1u), a_under);
				}
			}
		};

		std::uintptr_t FindPScope(std::uintptr_t a_obj, int a_depth)
		{
			if (!a_obj || a_depth > kMaxDepth || !ScopeIdent::IsNiNode(a_obj)) {
				return 0;
			}
			char name[16];
			if (ReadName(a_obj + kNameInObject, name, sizeof(name)) && _stricmp(name, "P-Scope") == 0) {
				return a_obj;
			}
			const auto kids = *reinterpret_cast<std::uintptr_t*>(a_obj + kChildren);
			const auto count = *reinterpret_cast<std::uint16_t*>(a_obj + kChildCount);
			for (std::uint16_t i = 0; kids && i < count; ++i) {
				if (const auto hit = FindPScope(*reinterpret_cast<std::uintptr_t*>(kids + i * 8ull), a_depth + 1)) {
					return hit;
				}
			}
			return 0;
		}

		bool ReadFrame(std::uintptr_t a_node, Frame& a_out)
		{
			const auto* m = reinterpret_cast<const float*>(a_node + kWorldRotate);
			for (int i = 0; i < 3; ++i) {
				for (int k = 0; k < 3; ++k) {
					a_out.rows[i][k] = m[i * 4 + k];
				}
			}
			const auto* t = reinterpret_cast<const float*>(a_node + kWorldTranslate);
			a_out.t[0] = t[0];
			a_out.t[1] = t[1];
			a_out.t[2] = t[2];
			a_out.s = *reinterpret_cast<const float*>(a_node + kWorldScale);
			for (int i = 0; i < 3; ++i) {
				for (int k = 0; k < 3; ++k) {
					if (!std::isfinite(a_out.rows[i][k])) {
						return false;
					}
				}
				if (!std::isfinite(a_out.t[i])) {
					return false;
				}
			}
			return std::isfinite(a_out.s);
		}

		bool WalkGuarded(std::uintptr_t a_weapon, std::uintptr_t& a_pScope, OpticClass::Input& a_in, std::uintptr_t* a_nodes)
		{
			__try {
				a_pScope = FindPScope(a_weapon, 0);
				if (!a_pScope) {
					return true;
				}
				Frame f{};
				ReadFrame(a_pScope, f);
				if (!(f.s > 1e-4f)) {
					f.s = 1.0f;
				}
				Walker{ f, a_in, a_nodes }.Walk(a_pScope, 0, false, 0);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// P-Scope as it stands this frame, re-found under the weapon so a node from an
		// earlier walk is never read after the 3D under it was rebuilt
		bool LiveFrame(std::uintptr_t a_weapon, std::uintptr_t a_pScope, Frame& a_out)
		{
			__try {
				return FindPScope(a_weapon, 0) == a_pScope && ReadFrame(a_pScope, a_out) && a_out.s > 1e-4f;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool AttachedGuarded(std::uintptr_t a_weapon, std::uintptr_t a_pScope)
		{
			__try {
				return a_weapon && FindPScope(a_weapon, 0) == a_pScope;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// 1 drawn, 0 not, -1 unknown
		int WeaponDrawn(std::uintptr_t a_player)
		{
			static int s_layout = -1;
			__try {
				if (s_layout < 0) {
					const auto col = Col(a_player + kActorStateInActor);
					s_layout = col && HasTypeName(a_player + kActorStateInActor, ".?AVPlayerCharacter@@") &&
					                   *reinterpret_cast<const std::uint32_t*>(col + 4) == kActorStateInActor ?
					               1 :
					               0;
					if (!s_layout) {
						logger::warn("optic gate: no PlayerCharacter ActorState at +0x128 - not waiting on the weapon being drawn"sv);
					}
				}
				if (!s_layout) {
					return -1;
				}
				const auto bits = *reinterpret_cast<const std::uint32_t*>(a_player + kActorStateInActor + kStateBitsInActorState);
				return ((bits >> 2) & 7u) == kWeaponDrawn ? 1 : 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}

		// what R1 puts a picture in
		bool LensKind(OpticClass::Kind a_kind)
		{
			using enum OpticClass::Kind;
			return a_kind == kStsMagnified || a_kind == kStsNoFade || a_kind == kStsFarReticle;
		}

		void RunGate(std::uintptr_t a_player, std::uintptr_t a_weapon)
		{
			const auto& r = g_snap.result;
			Gate        g = g_snap.gate;
			g.evaluating = false;
			g.live = false;
			g.sentinel = false;
			g.why[0] = '\0';
			g.drawn = a_player ? WeaponDrawn(a_player) : -1;
			const char* why = nullptr;
			if (!a_weapon || !g_snap.pScope) {
				why = "no P-Scope";
			} else if (!LensKind(r.kind) || !(r.radius > 0.0f)) {
				why = "not a lens optic";
			} else if (PoseGate::SiteEverRan() && !PoseGate::SiteStale(15)) {
				why = "HasScope weapon";
			} else if (Hooks::BlockingMenuOpen()) {
				why = "menu open";
			} else if (g.drawn == 0) {
				why = "weapon not drawn";
			} else if (!LiveFrame(a_weapon, g_snap.pScope, g_frame)) {
				why = "P-Scope changed";
			}
			PoseGate::Reading rd{};
			if (!why) {
				const auto& f = g_frame;
				const auto* a = r.centre;
				for (int i = 0; i < 3; ++i) {
					g.ocular[i] = f.t[i] + f.s * (a[0] * f.rows[0][i] + a[1] * f.rows[1][i] + a[2] * f.rows[2][i]);
					g.axis[i] = f.rows[1][i];
				}
				rd = PoseGate::JudgeEyepiece(g_hyst, g.ocular, g.axis);
				if (!rd.valid) {
					why = "no head pose";
				}
			}
			if (why) {
				g_hyst = {};
				strncpy_s(g.why, why, _TRUNCATE);
			} else {
				g.evaluating = true;
				g.live = rd.live;
				g.sentinel = rd.sentinel;
				g.dist = rd.dist;
				g.lateral = rd.lateral;
				g.lookDeg = rd.lookDeg;
				++g.evals;
			}
			if (g.live != g_snap.gate.live) {
				++g.edges;
				if (why) {
					logger::info(FMT_STRING("optic gate live -> false ({})"), why);
				} else if (rd.sentinel) {
					logger::info(FMT_STRING("optic gate live -> {} (both eyes ahead of the eyepiece)"), g.live);
				} else {
					logger::info(FMT_STRING("optic gate live -> {} (dist={:.1f} lat={:.2f} look={:.1f}deg, eyepiece ({:.1f},{:.1f},{:.1f}))"),
						g.live, g.dist, g.lateral, g.lookDeg, g.ocular[0], g.ocular[1], g.ocular[2]);
				}
			}
			std::scoped_lock lock(g_lock);
			g_snap.gate = g;
		}

		void LogIfChanged(const Snapshot& a_s)
		{
			const auto& r = a_s.result;
			if (a_s.pScope == g_loggedPScope && r.kind == g_loggedKind && r.fade == g_loggedFade && r.glass == g_loggedGlass) {
				return;
			}
			g_loggedPScope = a_s.pScope;
			g_loggedKind = r.kind;
			g_loggedFade = r.fade;
			g_loggedGlass = r.glass;
			if (!a_s.pScope) {
				logger::info(FMT_STRING("optic: no P-Scope on weapon {:x}"), a_s.weapon);
				return;
			}
			const auto name = [&](int a_i) { return a_i >= 0 ? a_s.in.shapes[a_i].name : "-"; };
			logger::info(FMT_STRING("optic: {} ({} shapes{}, key nodes 0x{:x}); fade '{}', glass '{}', anchor ({:.2f},{:.2f},{:.2f}) r={:.2f}; "
									"eyepiece {:.2f} ({}), cards near {} far {} ({:.1f}..{:.1f})"),
				OpticClass::KindName(r.kind), a_s.in.count, a_s.in.overflow ? " +overflow" : "", a_s.in.keyNodes, name(r.fade),
				name(r.glass), r.centre[0], r.centre[1], r.centre[2], r.radius, r.eyepiece, r.eyepieceFrom, r.nearCards,
				r.farCards, r.nearest, r.farthest);
		}
	}

	void Tick(std::uintptr_t a_player)
	{
		const auto weapon = ScopeIdent::FirstPersonWeaponNode(a_player);
		// While the STS lens holds the optic its fade is hidden and its cards moved,
		// and a walk would classify that; a different optic still shows at once,
		// since RunGate re-finds P-Scope every frame.
		if (weapon != g_lastWeapon || (++g_frames >= kRescanFrames && !StsLens::OwnsWidget())) {
			g_frames = 0;
			g_lastWeapon = weapon;

			auto s = std::make_unique<Snapshot>();
			s->weapon = weapon;
			s->faulted = weapon && !WalkGuarded(weapon, s->pScope, s->in, s->nodes);
			if (s->faulted) {
				s->pScope = 0;
				s->in = {};
			}
			s->result = OpticClass::Classify(s->in);
			s->walks = ++g_walks;
			if (weapon) {
				LogIfChanged(*s);
			}
			s->gate = g_snap.gate;
			if (s->pScope != g_snap.pScope) {
				g_hyst = {};  // another sight: its own episode
			}
			std::scoped_lock lock(g_lock);
			g_snap = *s;
		}
		RunGate(a_player, weapon);
	}

	Snapshot Get()
	{
		std::scoped_lock lock(g_lock);
		return g_snap;
	}

	Lens Current()
	{
		Lens l{};
		l.live = g_snap.gate.live;
		l.pScope = g_snap.pScope;
		l.frame = g_frame;
		l.in = &g_snap.in;
		l.result = &g_snap.result;
		l.nodes = g_snap.nodes;
		return l;
	}

	bool Attached(std::uintptr_t a_pScope)
	{
		const auto player = *reinterpret_cast<std::uintptr_t*>(REL::Module::get().base() + Addr::kPlayerGlobal);
		return a_pScope && player && AttachedGuarded(ScopeIdent::FirstPersonWeaponNode(player), a_pScope);
	}
}
