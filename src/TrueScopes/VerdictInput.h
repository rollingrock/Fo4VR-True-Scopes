#pragma once

// Controller verdicts for guided test passes: quick feedback through
// controller chords, no headset removal, no voice.
//
//   grip + A        -> "yes"   (short haptic ack)
//   grip + B        -> "no"    (long haptic ack)
//   grip + trigger  -> "skip"
//
// Either hand. Read passively off the game's own openvr_api.dll (IVRSystem_017,
// version string verified inside the shipped DLL): VR_GetGenericInterface once,
// then GetControllerState per frame — no input hooking, nothing stolen from the
// game; the chords are ones vanilla gameplay never uses as a pair-press.
// DevBench serves the events (`/verdict?since=N`) so a driver script can attach a
// scope, wait for the tester's chord, and advance.

#include <cstdint>

namespace TrueScopes::VerdictInput
{
	enum class Verdict : std::uint8_t
	{
		kNone = 0,
		kYes,
		kNo,
		kSkip,
	};

	struct Event
	{
		std::uint64_t seq;     // monotonically increasing, 1-based
		Verdict       verdict;
		std::uint64_t tickMs;  // GetTickCount64 at registration
	};

	// Poll once per frame from the render/frame hook. Cheap when idle (two
	// controller-state reads); does nothing until the OpenVR interface resolves.
	void Poll() noexcept;

	// Latest event (seq 0 = none yet).
	[[nodiscard]] Event Latest() noexcept;

	// Arm the poller at runtime (first 'verdict' tool call) even when
	// verdictInputEnabled is false - the drivers need chords without a TOML edit.
	void RequestStart() noexcept;

	// Diagnostics: did the IVRSystem_017 lookup succeed?
	[[nodiscard]] bool Available() noexcept;
}
