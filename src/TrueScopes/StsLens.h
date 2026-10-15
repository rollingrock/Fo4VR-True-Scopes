#pragma once

#include <cstdint>
#include <string_view>

namespace TrueScopes::StsLens
{
	// R1: a See Through Scopes-format scope through our lens. STS optics carry no
	// HasScope, so the vanilla scope never runs for them; while Optic's gate is live
	// this takes the vanilla widget over (it exists from the main menu on) and puts
	// it on the fade disc:
	// - ScopeParent on the eyepiece, facing up-range, render_circle:0 sized to the
	//   fade radius, every other widget shape hidden;
	// - the effect shader's slot-6 cache pointed at render_circle:0, so its draw
	//   binds the lens RT 0x62 (the enable switch does this for vanilla scopes, and
	//   it never runs here);
	// - the in-tube reticle cards moved onto the disc plane, scaled to fill it, so
	//   the reticle sits in the picture; the fade vignette and eyepiece glass hidden;
	// - the scope camera on the optic's axis (PlaceCamera, from the render).
	// Everything is put back on the way out.

	// Game thread, every frame, from the fill hook right after Optic::Tick.
	void Frame(std::uintptr_t a_player);

	// The lens is up and placed this frame: the fill should render it.
	bool Live();

	// ScopeParent and the scope camera are ours: the vanilla fit stands aside.
	bool OwnsWidget();

	// From the render, STS renders only: the camera onto the optic's axis, looking
	// down-range.
	void PlaceCamera(std::uintptr_t a_cam);

	// From the render after the camera update: where the camera actually was for
	// this render, for the diag. Something rewrites the camera's local translate
	// every frame (FRIK's scope camera), so a read outside the render cannot see it.
	void NoteCamera(std::uintptr_t a_cam);

	// Put back everything touched. Game thread; idempotent.
	void StandDown(std::string_view a_why);

	struct Diag
	{
		bool          engaged;
		bool          cacheOurs;
		std::uint32_t cardsMoved;
		std::uint32_t partsHidden;
		float         discRadius;  // world units
		float         cardScale;
		float         disc[3];     // where the disc was put, world
		float         camMiss;     // the camera at render time, from where it was put
		float         camAxis;     // its view direction . the optic's axis
		std::uint32_t engages;
		char          lastRelease[48];
	};
	Diag GetDiag();
}
