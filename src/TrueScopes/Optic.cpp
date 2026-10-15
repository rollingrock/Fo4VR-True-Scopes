#include "TrueScopes/Optic.h"

#include "TrueScopes/ScopeIdent.h"

namespace TrueScopes::Optic
{
	namespace
	{
		// VR NiAVObject / NiNode
		constexpr std::uintptr_t kNameInObject = 0x10;  // BSFixedString
		constexpr std::uintptr_t kWorldRotate = 0x70;   // rows of 4 floats; a row is that axis in world
		constexpr std::uintptr_t kWorldTranslate = 0xa0;
		constexpr std::uintptr_t kWorldScale = 0xac;
		constexpr std::uintptr_t kWorldBound = 0xb0;  // {centre, radius}
		constexpr std::uintptr_t kObjFlags = 0x108;   // bit 0 hidden
		constexpr std::uintptr_t kChildren = 0x168;
		constexpr std::uintptr_t kChildCount = 0x172;  // u16 loop bound, null holes legal

		// VR BSGeometry and its shader property (see Collimator.cpp)
		constexpr std::uintptr_t kShaderPropertyInGeometry = 0x178;
		constexpr std::uintptr_t kIsGeometryVFunc = 0x40;
		constexpr std::uintptr_t kBSGeometryIsGeometry = 0x1c31c50;
		constexpr std::uintptr_t kPropertyFlags = 0x30;  // u64, technique builder 0x14279f360
		constexpr std::uint64_t  kEnvMap = 1ull << 7;
		constexpr std::uintptr_t kMaterialInProperty = 0x58;
		constexpr std::uintptr_t kBaseTexturePathInMaterial = 0x78;

		constexpr std::uintptr_t kImageSpan = 0x8000000;
		constexpr int            kMaxDepth = 12;
		constexpr std::uint32_t  kRescanFrames = 30;

		std::mutex     g_lock;
		Snapshot       g_snap{};  // under g_lock
		std::uintptr_t g_lastWeapon = 0;
		std::uint32_t  g_frames = 0;
		std::uint32_t  g_walks = 0;

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

		bool HasTypeName(std::uintptr_t a_obj, const char* a_name)
		{
			if (a_obj < 0x10000) {
				return false;
			}
			const auto base = REL::Module::get().base();
			const auto vtbl = *reinterpret_cast<std::uintptr_t*>(a_obj);
			if (vtbl < base || vtbl >= base + kImageSpan) {
				return false;
			}
			const auto col = *reinterpret_cast<std::uintptr_t*>(vtbl - 8);
			if (col < base || col >= base + kImageSpan) {
				return false;
			}
			const auto td = base + *reinterpret_cast<std::uint32_t*>(col + 0x0C);
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

		struct Frame
		{
			float rows[3][3];
			float t[3];
			float s;
		};

		struct Walker
		{
			const Frame&       frame;
			OpticClass::Input& in;

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
				// world bound into the attach frame: local = rows * (w - t) / s
				const auto* b = reinterpret_cast<const float*>(a_obj + kWorldBound);
				const float d[3] = { b[0] - frame.t[0], b[1] - frame.t[1], b[2] - frame.t[2] };
				for (int i = 0; i < 3; ++i) {
					s.centre[i] = (frame.rows[i][0] * d[0] + frame.rows[i][1] * d[1] + frame.rows[i][2] * d[2]) / frame.s;
				}
				s.radius = b[3] / frame.s;
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

		bool WalkGuarded(std::uintptr_t a_weapon, std::uintptr_t& a_pScope, OpticClass::Input& a_in)
		{
			__try {
				a_pScope = FindPScope(a_weapon, 0);
				if (!a_pScope) {
					return true;
				}
				Frame f{};
				const auto* m = reinterpret_cast<const float*>(a_pScope + kWorldRotate);
				for (int i = 0; i < 3; ++i) {
					for (int k = 0; k < 3; ++k) {
						f.rows[i][k] = m[i * 4 + k];
					}
				}
				const auto* t = reinterpret_cast<const float*>(a_pScope + kWorldTranslate);
				f.t[0] = t[0];
				f.t[1] = t[1];
				f.t[2] = t[2];
				f.s = *reinterpret_cast<const float*>(a_pScope + kWorldScale);
				if (!(f.s > 1e-4f)) {
					f.s = 1.0f;
				}
				Walker{ f, a_in }.Walk(a_pScope, 0, false, 0);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
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
		if (weapon == g_lastWeapon && ++g_frames < kRescanFrames) {
			return;
		}
		g_frames = 0;
		g_lastWeapon = weapon;

		Snapshot s{};
		s.weapon = weapon;
		s.faulted = weapon && !WalkGuarded(weapon, s.pScope, s.in);
		if (s.faulted) {
			s.pScope = 0;
			s.in = {};
		}
		s.result = OpticClass::Classify(s.in);
		s.walks = ++g_walks;
		if (weapon) {
			LogIfChanged(s);
		}
		std::scoped_lock lock(g_lock);
		g_snap = s;
	}

	Snapshot Get()
	{
		std::scoped_lock lock(g_lock);
		return g_snap;
	}
}
