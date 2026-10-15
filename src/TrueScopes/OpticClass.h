#pragma once

// What kind of sight hangs under P-Scope, from its shapes alone: an STS-format
// magnified scope (reticle card inside the tube), a collimated reflex/holo sight
// (cards far down-range), both, or neither. Rules follow the mesh census
// (investigation repo, RESEARCH_2026-10-13_STS_MESH_ANATOMY.md section 4): in-tube
// reticles sit at most 16.2 units past the eyepiece, collimated cards at least 18.1.
//
// No engine types here, so tools/optic-class/check.py compiles this file as is
// and runs it over every surveyed mesh. The runtime keeps no vertex positions
// (BSGraphics::TriShape holds only GPU buffers), so geometry is each shape's
// bound sphere in the P-Scope frame, Y down-range.

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace TrueScopes::OpticClass
{
	inline constexpr std::size_t kMaxShapes = 48;
	inline constexpr std::size_t kNameLen = 64;
	inline constexpr std::size_t kPathLen = 160;

	// cards nearer the eyepiece than this are in the tube
	inline constexpr float kCollimatedFrom = 17.5f;

	struct Shape
	{
		char  name[kNameLen];
		char  material[kPathLen];  // shader property name, the BGEM/BGSM path; may be empty
		char  texture[kPathLen];   // effect material base texture; empty for lit shapes
		bool  effect;              // BSEffectShaderProperty
		bool  envMap;
		bool  hidden;              // hidden at rest, not counting the STS toggle nodes' own flags
		bool  underAiming;
		bool  underNormal;
		bool  underReticleNode;
		float centre[3];
		float radius;
	};

	enum KeyNode : std::uint32_t
	{
		kScopeAiming = 1u << 0,
		kScopeNormal = 1u << 1,
		kScopeViewParts = 1u << 2,
		kScopeAlways = 1u << 3,
		kReticleNode = 1u << 4,
	};

	struct Input
	{
		Shape         shapes[kMaxShapes];
		std::uint32_t count;
		std::uint32_t overflow;  // shapes past kMaxShapes, not classified
		std::uint32_t keyNodes;  // KeyNode bits, nodes only (VRWO also names shapes like them)
	};

	enum class Kind : std::uint8_t
	{
		kNone,           // no reticle card, no STS nodes: irons, plain parts
		kStsNoReticle,   // STS nodes, no card (lasers, some mounts)
		kStsMagnified,   // cards in the tube behind a fade disc
		kStsNoFade,      // cards in the tube, no fade disc
		kGlowPins,       // near cards with no glass, fade or STS nodes: iron night sights
		kStsFarReticle,  // fade disc, every card collimated (ELCAN, HAMR)
		kCollimated,     // every card collimated, no fade disc
		kCombo,          // cards both in the tube and far
	};

	enum class Part : std::uint8_t
	{
		kBody,
		kFx,
		kFade,
		kCard,
		kGlass,
		kScreen,
		kLaser,
		kTextureLoader,
	};

	struct Result
	{
		Kind          kind;
		int           fade;       // shape index, -1 none
		int           glass;      // ocular glass, -1 none
		char          eyepieceFrom;  // 'f' fade, 'g' glass, 'h' housing, 'o' origin
		float         eyepiece;   // eyepiece Y
		std::uint32_t nearCards;
		std::uint32_t farCards;
		float         nearest;    // smallest card distance past the eyepiece
		float         farthest;
		float         centre[3];  // the anchor (fade, else glass)
		float         radius;
	};

	namespace detail
	{
		inline bool StartsWith(const char* a_s, const char* a_prefix)
		{
			return _strnicmp(a_s, a_prefix, std::strlen(a_prefix)) == 0;
		}

		inline bool Contains(const char* a_s, const char* a_needle)
		{
			const auto n = std::strlen(a_needle);
			for (; *a_s; ++a_s) {
				if (_strnicmp(a_s, a_needle, n) == 0) {
					return true;
				}
			}
			return false;
		}

		// "Reticle:3" -> true for "reticle"
		inline bool BaseIs(const char* a_name, const char* a_base)
		{
			const auto n = std::strlen(a_base);
			if (_strnicmp(a_name, a_base, n) != 0) {
				return false;
			}
			const char* rest = a_name + n;
			if (!*rest) {
				return true;
			}
			if (*rest++ != ':' || !*rest) {
				return false;
			}
			for (; *rest; ++rest) {
				if (!std::isdigit(static_cast<unsigned char>(*rest))) {
					return false;
				}
			}
			return true;
		}

		inline bool IsFadeMaterial(const char* a_s)
		{
			return Contains(a_s, "lensfade") || Contains(a_s, "scoperadius");
		}
	}

	// Order matters: a lens fade named Reticle is a fade, and a broad laser match
	// would swallow Laser4xCross reticles and LaserReflexGlass lenses.
	inline Part Classify(const Shape& a_s)
	{
		using namespace detail;
		if (StartsWith(a_s.name, "textureloader")) {
			return Part::kTextureLoader;
		}
		if (StartsWith(a_s.name, "scopefade") || StartsWith(a_s.name, "edgeblur") ||
			IsFadeMaterial(a_s.material) || IsFadeMaterial(a_s.texture)) {
			return Part::kFade;
		}
		if (a_s.underReticleNode || (a_s.effect && (StartsWith(a_s.name, "reticle") || StartsWith(a_s.name, "dot") ||
														StartsWith(a_s.name, "tritiumdot")))) {
			return Part::kCard;
		}
		if (StartsWith(a_s.name, "laser") || StartsWith(a_s.name, "beam") || Contains(a_s.material, "laser_red") ||
			Contains(a_s.material, "laser_green") || Contains(a_s.material, "laser_blue") ||
			Contains(a_s.material, "laserspot") || Contains(a_s.material, "beam")) {
			return Part::kLaser;
		}
		if (Contains(a_s.material, "thermalglass") || Contains(a_s.material, "nvglass") ||
			Contains(a_s.material, "targetingglass")) {
			return Part::kScreen;
		}
		if (Contains(a_s.name, "glass") || Contains(a_s.name, "lens") || Contains(a_s.material, "glass") ||
			Contains(a_s.material, "lens") || (a_s.effect && (Contains(a_s.texture, "reflexglass") || a_s.envMap))) {
			return Part::kGlass;
		}
		return a_s.effect ? Part::kFx : Part::kBody;
	}

	inline const char* PartName(Part a_part)
	{
		constexpr const char* kNames[] = { "body", "fx", "fade", "card", "glass", "screen", "laser", "textureloader" };
		return kNames[static_cast<std::uint8_t>(a_part)];
	}

	inline const char* KindName(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::kStsNoReticle:
			return "sts-noreticle";
		case Kind::kStsMagnified:
			return "sts-mag";
		case Kind::kStsNoFade:
			return "sts-nofade";
		case Kind::kGlowPins:
			return "glow-pins";
		case Kind::kStsFarReticle:
			return "sts-far";
		case Kind::kCollimated:
			return "collimated";
		case Kind::kCombo:
			return "combo";
		default:
			return "none";
		}
	}

	inline Result Classify(const Input& a_in)
	{
		Result r{ Kind::kNone, -1, -1, 'o', 0.0f, 0, 0, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f };
		const auto n = a_in.count < kMaxShapes ? a_in.count : static_cast<std::uint32_t>(kMaxShapes);

		// visible while aiming: flat STS swaps ScopeNormal out for ScopeAiming
		const bool twoModel = (a_in.keyNodes & kScopeAiming) && (a_in.keyNodes & kScopeNormal);
		bool       visible[kMaxShapes]{};
		Part       part[kMaxShapes]{};
		bool       card[kMaxShapes]{};
		for (std::uint32_t i = 0; i < n; ++i) {
			const auto& s = a_in.shapes[i];
			visible[i] = !s.hidden && !(twoModel && s.underNormal) && std::isfinite(s.radius) && s.radius > 0.0f;
			part[i] = Classify(s);
			card[i] = visible[i] && s.radius > 1e-3f && part[i] != Part::kLaser &&
			          (part[i] == Part::kCard || detail::StartsWith(s.name, "tritiumdot") ||
						  detail::StartsWith(s.name, "reticle") || detail::StartsWith(s.name, "dot"));
		}

		// eyepiece: the rear-most fade disc, else glass, else the housing's rear
		int fade = -1;
		int rearGlass = -1;
		int rearOther = -1;
		for (std::uint32_t i = 0; i < n; ++i) {
			if (!visible[i]) {
				continue;
			}
			const auto& s = a_in.shapes[i];
			const auto  p = part[i];
			if (p == Part::kFade) {
				if (fade < 0 || s.centre[1] < a_in.shapes[fade].centre[1]) {
					fade = static_cast<int>(i);
				}
			}
			if (p == Part::kGlass || p == Part::kScreen) {
				if (rearGlass < 0 || s.centre[1] < a_in.shapes[rearGlass].centre[1]) {
					rearGlass = static_cast<int>(i);
				}
			}
			if (!card[i] && p != Part::kLaser && p != Part::kTextureLoader) {
				const float back = s.centre[1] - s.radius;
				if (rearOther < 0 || back < a_in.shapes[rearOther].centre[1] - a_in.shapes[rearOther].radius) {
					rearOther = static_cast<int>(i);
				}
			}
		}
		if (fade >= 0) {
			r.eyepiece = a_in.shapes[fade].centre[1];
			r.eyepieceFrom = 'f';
		} else if (rearGlass >= 0) {
			r.eyepiece = a_in.shapes[rearGlass].centre[1];
			r.eyepieceFrom = 'g';
		} else if (rearOther >= 0) {
			r.eyepiece = a_in.shapes[rearOther].centre[1] - a_in.shapes[rearOther].radius;
			r.eyepieceFrom = 'h';
		}
		r.fade = fade;

		// reticle: the card named Reticle, else any card, nearest the eye
		int reticle = -1;
		for (int pass = 0; pass < 2 && reticle < 0; ++pass) {
			for (std::uint32_t i = 0; i < n; ++i) {
				if (card[i] && (pass || detail::BaseIs(a_in.shapes[i].name, "reticle")) &&
					(reticle < 0 || a_in.shapes[i].centre[1] < a_in.shapes[reticle].centre[1])) {
					reticle = static_cast<int>(i);
				}
			}
		}

		bool any = false;
		for (std::uint32_t i = 0; i < n; ++i) {
			if (!card[i]) {
				continue;
			}
			const float d = a_in.shapes[i].centre[1] - r.eyepiece;
			if (d < kCollimatedFrom) {
				++r.nearCards;
			} else {
				++r.farCards;
			}
			r.nearest = any && r.nearest < d ? r.nearest : d;
			r.farthest = any && r.farthest > d ? r.farthest : d;
			any = true;
		}

		// ocular glass: concentric with the fade disc and about its size (drops flat
		// STS's oversized Glass:0 reflection sheet), else the rear-most glass in
		// front of the reticle, env-mapped first
		const auto isGlass = [&](std::uint32_t i) {
			return visible[i] && (part[i] == Part::kGlass || part[i] == Part::kScreen);
		};
		float rearY = 0.0f;
		bool  haveRear = false;
		for (std::uint32_t i = 0; fade < 0 && i < n; ++i) {
			const auto& g = a_in.shapes[i];
			if (isGlass(i) && (reticle < 0 || g.centre[1] < a_in.shapes[reticle].centre[1]) && (!haveRear || g.centre[1] < rearY)) {
				rearY = g.centre[1];
				haveRear = true;
			}
		}
		float bestKey = 0.0f;
		for (std::uint32_t i = 0; i < n; ++i) {
			if (!isGlass(i)) {
				continue;
			}
			const auto& g = a_in.shapes[i];
			float       key = 0.0f;
			if (fade >= 0) {
				const auto& f = a_in.shapes[fade];
				const float dy = std::fabs(g.centre[1] - f.centre[1]);
				const float off = std::hypot(g.centre[0] - f.centre[0], g.centre[2] - f.centre[2]);
				const float ratio = g.radius / (f.radius > 1e-6f ? f.radius : 1e-6f);
				if (dy > 1.5f || off > 0.5f * f.radius + 0.25f || ratio < 0.7f || ratio > 1.4f) {
					continue;
				}
				key = (std::fabs(ratio - 1.0f) > 0.1f ? 1000.0f : 0.0f) + (g.envMap ? 0.0f : 100.0f) + dy;
			} else {
				if ((reticle >= 0 && g.centre[1] >= a_in.shapes[reticle].centre[1]) || g.centre[1] - rearY > 0.75f) {
					continue;
				}
				key = (g.envMap ? 0.0f : 1000.0f) + g.centre[1];
			}
			if (r.glass < 0 || key < bestKey) {
				r.glass = static_cast<int>(i);
				bestKey = key;
			}
		}

		const int anchor = fade >= 0 ? fade : r.glass;
		if (anchor >= 0) {
			const auto& a = a_in.shapes[anchor];
			r.centre[0] = a.centre[0];
			r.centre[1] = a.centre[1];
			r.centre[2] = a.centre[2];
			r.radius = a.radius;
		}

		if (!any) {
			r.kind = a_in.keyNodes ? Kind::kStsNoReticle : Kind::kNone;
		} else if (r.nearCards && r.farCards) {
			r.kind = Kind::kCombo;
		} else if (r.farCards) {
			r.kind = fade >= 0 ? Kind::kStsFarReticle : Kind::kCollimated;
		} else if (fade >= 0) {
			r.kind = Kind::kStsMagnified;
		} else {
			r.kind = r.glass >= 0 || a_in.keyNodes ? Kind::kStsNoFade : Kind::kGlowPins;
		}
		return r;
	}
}
