#include "TrueScopes/VerdictInput.h"

#include <atomic>

#include "Settings/Settings.h"

namespace TrueScopes::VerdictInput
{
	namespace
	{
		std::atomic_bool g_requested{ false };
		// IVRSystem_017 ABI (openvr 1.0.10; the version string inside FO4VR's
		// shipped openvr_api.dll is exactly "IVRSystem_017").
		// vtable slots counted from the header's declaration order:
		//   18 GetTrackedDeviceIndexForControllerRole(role)  role: 1=left, 2=right
		//   33 GetControllerState(index, state*, size)
		//   35 TriggerHapticPulse(index, axis, microsec)
		// VRControllerState_t: { u32 packetNum; u64 buttonPressed; u64 buttonTouched;
		//   float axes[5][2]; } under pack(8) -> buttonPressed at +8, size 0x40.
		constexpr std::uint32_t kSlotRoleIndex = 18;
		constexpr std::uint32_t kSlotControllerState = 33;
		constexpr std::uint32_t kSlotHaptic = 35;

		struct ControllerState
		{
			std::uint32_t packetNum;
			std::uint64_t buttonPressed;
			std::uint64_t buttonTouched;
			float         axes[5][2];
		};
		static_assert(sizeof(ControllerState) == 0x40);

		// EVRButtonId masks
		constexpr std::uint64_t kMaskGrip = 1ull << 2;      // k_EButton_Grip
		constexpr std::uint64_t kMaskA = 1ull << 7;         // k_EButton_A
		constexpr std::uint64_t kMaskB = 1ull << 1;         // k_EButton_ApplicationMenu (B / Y on Touch)
		constexpr std::uint64_t kMaskTrigger = 1ull << 33;  // k_EButton_SteamVR_Trigger

		using RoleIndex_t = std::uint32_t(*)(void*, std::uint32_t);
		using GetState_t = bool(*)(void*, std::uint32_t, ControllerState*, std::uint32_t);
		using Haptic_t = void(*)(void*, std::uint32_t, std::uint32_t, std::uint16_t);
		using GetGenericInterface_t = void* (*)(const char*, int*);

		void*            g_system = nullptr;
		bool             g_lookupTried = false;
		std::uint64_t    g_prevButtons[2] = { 0, 0 };  // per hand
		std::atomic<std::uint64_t> g_seq{ 0 };
		std::atomic<std::uint8_t>  g_lastVerdict{ 0 };
		std::atomic<std::uint64_t> g_lastTick{ 0 };
		// haptic ack: remaining frames to pulse, and which device
		std::uint32_t g_ackFrames = 0;
		std::uint32_t g_ackDevice = ~0u;

		template <class F>
		F Slot(std::uint32_t a_index) noexcept
		{
			return reinterpret_cast<F>((*reinterpret_cast<void***>(g_system))[a_index]);
		}

		void ResolveOnce() noexcept
		{
			if (g_lookupTried) {
				return;
			}
			g_lookupTried = true;
			const auto mod = ::GetModuleHandleW(L"openvr_api.dll");
			if (!mod) {
				logger::warn("VerdictInput: openvr_api.dll not loaded"sv);
				return;
			}
			const auto get = reinterpret_cast<GetGenericInterface_t>(::GetProcAddress(mod, "VR_GetGenericInterface"));
			if (!get) {
				logger::warn("VerdictInput: VR_GetGenericInterface not exported"sv);
				return;
			}
			int err = 0;
			g_system = get("IVRSystem_017", &err);
			if (!g_system) {
				logger::warn(FMT_STRING("VerdictInput: IVRSystem_017 unavailable (err {})"), err);
				return;
			}
			logger::info("VerdictInput: IVRSystem_017 resolved — grip+A yes, grip+B no, grip+trigger skip"sv);
		}

		void Register(Verdict a_v, std::uint32_t a_device) noexcept
		{
			g_lastVerdict.store(static_cast<std::uint8_t>(a_v));
			g_lastTick.store(::GetTickCount64());
			g_seq.fetch_add(1);
			// ack: yes = short buzz, no = long, skip = medium
			g_ackDevice = a_device;
			g_ackFrames = a_v == Verdict::kYes ? 10 : a_v == Verdict::kNo ? 40 : 20;
			logger::info(FMT_STRING("VERDICT #{}: {}"), g_seq.load(),
				a_v == Verdict::kYes ? "yes" : a_v == Verdict::kNo ? "no" : "skip");
		}
	}

	void Poll() noexcept
	{
		if (!*Settings::verdictInputEnabled && !g_requested.load(std::memory_order_relaxed)) {
			return;
		}
		ResolveOnce();
		if (!g_system) {
			return;
		}
		__try {
			for (std::uint32_t hand = 0; hand < 2; ++hand) {
				const auto idx = Slot<RoleIndex_t>(kSlotRoleIndex)(g_system, hand + 1);  // 1=left, 2=right
				if (idx == ~0u || idx == 0) {  // invalid, or the HMD
					g_prevButtons[hand] = 0;
					continue;
				}
				ControllerState st{};
				if (!Slot<GetState_t>(kSlotControllerState)(g_system, idx, &st, sizeof(st))) {
					g_prevButtons[hand] = 0;
					continue;
				}
				const auto now = st.buttonPressed;
				const auto rose = now & ~g_prevButtons[hand];
				g_prevButtons[hand] = now;
				if (!(now & kMaskGrip)) {
					continue;
				}
				if (rose & kMaskA) {
					Register(Verdict::kYes, idx);
				} else if (rose & kMaskB) {
					Register(Verdict::kNo, idx);
				} else if (rose & kMaskTrigger) {
					Register(Verdict::kSkip, idx);
				}
			}
			if (g_ackFrames && g_ackDevice != ~0u) {
				--g_ackFrames;
				Slot<Haptic_t>(kSlotHaptic)(g_system, g_ackDevice, 0, 3000);
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			// wrong ABI guess or a dying runtime: disable for the session, loudly.
			g_system = nullptr;
			logger::error("VerdictInput: faulted reading controller state — disabled for this session"sv);
		}
	}

	Event Latest() noexcept
	{
		Event e{};
		e.seq = g_seq.load();
		e.verdict = static_cast<Verdict>(g_lastVerdict.load());
		e.tickMs = g_lastTick.load();
		return e;
	}

	void RequestStart() noexcept
	{
		if (!g_requested.exchange(true, std::memory_order_relaxed)) {
			logger::info("VerdictInput: armed by tool request"sv);
		}
	}

	bool Available() noexcept
	{
		return g_system != nullptr;
	}
}
