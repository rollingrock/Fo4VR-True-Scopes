#include "TrueScopes/OpticClass.h"

#include <cstdio>
#include <cstring>

using namespace TrueScopes::OpticClass;

namespace
{
	bool Require(bool a_value, const char* a_message)
	{
		if (!a_value) {
			std::fprintf(stderr, "FAILED: %s\n", a_message);
			return false;
		}
		return true;
	}

	struct Builder
	{
		Input in{};

		Shape& Add(const char* a_name, float a_y, float a_r, bool a_effect = false, const char* a_material = "",
			const char* a_texture = "")
		{
			auto& s = in.shapes[in.count++];
			s = {};
			strncpy_s(s.name, a_name, _TRUNCATE);
			strncpy_s(s.material, a_material, _TRUNCATE);
			strncpy_s(s.texture, a_texture, _TRUNCATE);
			s.effect = a_effect;
			s.centre[1] = a_y;
			s.centre[2] = 2.0f;
			s.radius = a_r;
			return s;
		}
	};

	const char* NameOf(const Input& a_in, int a_i)
	{
		return a_i >= 0 ? a_in.shapes[a_i].name : "";
	}
}

int main()
{
	{
		// flat STS hunting scope: closed model swapped out, fade disc, ocular glass,
		// an oversized reflection sheet and the in-tube reticle
		Builder b;
		b.in.keyNodes = kScopeAiming | kScopeNormal | kScopeViewParts | kReticleNode;
		b.Add("HuntingScope:0", -2.0f, 6.0f).underNormal = true;
		b.Add("ScopeFade:0", -12.0f, 1.0f, true, "", "textures\\Effects\\ScopeRadiusCircle.dds").underNormal = true;
		b.Add("HuntingScope:1", -2.0f, 6.0f).underAiming = true;
		b.Add("ScopeFade:0", -6.4f, 1.27f, true, "Materials\\Scope\\LensFadeEffect.BGEM").underAiming = true;
		b.Add("Glass:0", -6.3f, 3.2f, true, "Materials\\Scope\\Glass.BGEM").underAiming = true;
		b.Add("HuntingScopeGlass:1", -6.2f, 1.25f, false, "Materials\\Weapons\\HuntingScopeGlass.BGSM").underAiming = true;
		auto& ret = b.Add("Reticle:0", 4.0f, 1.8f, true);
		ret.underAiming = ret.underReticleNode = true;
		auto& dot = b.Add("Dot:0", 3.99f, 0.3f, true);
		dot.underAiming = dot.underReticleNode = true;
		const auto r = Classify(b.in);
		if (!Require(r.kind == Kind::kStsMagnified, "flat STS scope is sts-mag") ||
			!Require(r.fade == 3, "the aiming model's fade disc, not the closed one") ||
			!Require(std::strcmp(NameOf(b.in, r.glass), "HuntingScopeGlass:1") == 0, "ocular glass, not the reflection sheet") ||
			!Require(r.eyepieceFrom == 'f' && r.eyepiece == -6.4f, "eyepiece is the fade disc") ||
			!Require(r.nearCards == 2 && r.farCards == 0, "both reticle cards are in the tube") ||
			!Require(r.radius == 1.27f, "anchor radius is the fade disc's")) {
			return 1;
		}
	}
	{
		// vanilla reflex: glass at the sight, tritium card far down-range, a laser
		// beam under the sight that must not count
		Builder b;
		b.Add("ReflexSight:0", 0.0f, 3.0f);
		b.Add("ReflexGlass:1", 0.5f, 0.9f, true, "Materials\\Weapons\\ReflexGlass.BGEM", "textures\\Weapons\\ReflexGlass.dds").envMap = true;
		b.Add("TritiumDot019", 60.0f, 0.4f, true, "", "textures\\Effects\\GlowSpotLinear.dds");
		b.Add("LaserSightBeam:0", 600.0f, 600.0f, true, "Materials\\Effects\\LaserBeam.BGEM");
		const auto r = Classify(b.in);
		if (!Require(r.kind == Kind::kCollimated, "reflex sight is collimated") ||
			!Require(r.fade == -1 && r.eyepieceFrom == 'g', "reflex eyepiece is its glass") ||
			!Require(r.farCards == 1 && r.nearCards == 0, "the beam is not a card")) {
			return 1;
		}
	}
	{
		// STS-structured optic with a far reticle (ELCAN, HAMR)
		Builder b;
		b.in.keyNodes = kScopeAiming | kScopeViewParts;
		b.Add("ScopeFade:0", -3.0f, 1.1f, true, "Materials\\Scope\\LensFadeEffect.BGEM").underAiming = true;
		b.Add("TritiumDot002", 30.0f, 0.5f, true).underAiming = true;
		if (!Require(Classify(b.in).kind == Kind::kStsFarReticle, "fade disc + far card is sts-far")) {
			return 1;
		}
	}
	{
		// iron night sights: dots near the rear, nothing else
		Builder b;
		b.Add("PipeRevolver:0", 4.0f, 8.0f);
		b.Add("Dot:0", 2.0f, 0.1f, true);
		b.Add("Dot:1", 9.0f, 0.1f, true);
		const auto r = Classify(b.in);
		if (!Require(r.kind == Kind::kGlowPins, "glow pins are not an STS scope") ||
			!Require(r.fade == -1 && r.glass == -1, "glow pins have no anchor")) {
			return 1;
		}
	}
	{
		// a hidden shape and an STS mount without any card
		Builder b;
		b.in.keyNodes = kScopeAiming;
		b.Add("Mount:0", 0.0f, 2.0f).underAiming = true;
		b.Add("Reticle:0", 5.0f, 1.0f, true).hidden = true;
		if (!Require(Classify(b.in).kind == Kind::kStsNoReticle, "a hidden card does not count")) {
			return 1;
		}
		b.in.keyNodes = 0;
		if (!Require(Classify(b.in).kind == Kind::kNone, "no card, no STS nodes is none")) {
			return 1;
		}
	}
	{
		// a lens fade named like a reticle stays the fade; a Laser4xCross reticle
		// under ReticleNode stays a card
		Builder b;
		b.Add("Reticle:9", -1.0f, 1.0f, true, "Materials\\Scope\\LensFadeEffect.BGEM");
		b.Add("Laser4xCross:0", 6.0f, 1.0f, true).underReticleNode = true;
		if (!Require(Classify(b.in.shapes[0]) == Part::kFade, "fade before card") ||
			!Require(Classify(b.in.shapes[1]) == Part::kCard, "card before laser")) {
			return 1;
		}
	}
	std::puts("optic class tests passed");
	return 0;
}
