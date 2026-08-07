#pragma once

namespace Settings
{
	template <class T>
	class Setting
	{
	public:
		using value_type = T;

		Setting(
			std::string_view a_group,
			std::string_view a_key,
			value_type a_default) noexcept :
			_group(a_group),
			_key(a_key),
			_value(a_default)
		{}

		[[nodiscard]] auto group() const noexcept -> std::string_view { return this->_group; }
		[[nodiscard]] auto key() const noexcept -> std::string_view { return this->_key; }

		template <class Self>
		[[nodiscard]] auto&& get(this Self&& a_self) noexcept
		{
			return std::forward<Self>(a_self)._value;
		}

		template <class Self>
		[[nodiscard]] auto&& operator*(this Self&& a_self) noexcept
		{
			return std::forward<Self>(a_self).get();
		}

	private:
		std::string_view _group;
		std::string_view _key;
		value_type _value;
	};

	using bSetting = Setting<bool>;
	using iSetting = Setting<std::int64_t>;
	using fSetting = Setting<double>;
	using sSetting = Setting<std::string>;

#define MAKE_SETTING(a_type, a_group, a_key, a_default) \
	inline auto a_key = a_type(a_group##sv, #a_key##sv, a_default)

	// Master switch for the per-frame lens fill.
	MAKE_SETTING(bSetting, "TrueScopesVR", fillEnabled, true);
	// Fill cadence: 1 = every frame, 2 = every other frame, ... RT 0x62 persists
	// between frames, so low cadence just lowers the lens refresh rate.
	MAKE_SETTING(iSetting, "TrueScopesVR", fillEveryNFrames, std::int64_t(1));
	// Write 2 (always-on) into the iScopeEnabled:VR value cell after game load.
	// Optional: with the edge-triggered state hooks, the vanilla default (1, eye-gated)
	// works correctly — this just keeps the widget/fill on at all times.
	MAKE_SETTING(bSetting, "TrueScopesVR", forceAlwaysOn, false);
	// What fills the lens while scoped: 1 = copy of the main frame (phase-1 placeholder),
	// 2 = real mono world render from the scope camera (falls back to 1 on init failure
	// or a render fault).
	MAKE_SETTING(iSetting, "TrueScopesVR", lensMode, std::int64_t(2));
	// Scope render field of view in degrees (phase 2a fixed value; weapon zoomData wiring
	// comes later).
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeFovDegrees, 15.0);
	// Frustum near/far planes for the scope camera (SetCameraFOV params 3/4). near==far
	// produces a NaN projection and an eternally black render — the v0.2.x black-lens
	// root cause. 15 = the engine's default near; far generous for scoped sightlines.
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeNearClip, 15.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeFarClip, 250000.0);
	// Camera position in weapon-local space (applied to PrimaryWeaponScopeCamera's local
	// translate each render). Push the camera to the objective (front) end of the scope
	// tube so the render looks out of the glass, not at the tube interior. Y = forward
	// along the barrel in weapon space. Tune live: the TOML reloads on every scope-in.
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeCamOffsetX, 0.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeCamOffsetY, 15.0);
	MAKE_SETTING(fSetting, "TrueScopesVR", scopeCamOffsetZ, 0.0);
	// Draw the engine's sun BSDFLightDir pass into our scope light accumulation before
	// the resolve (v0.2.26). Requires the resolve accum-bind hooks; falls back to the
	// v0.2.25 look (ambient + local lights only) when off or unavailable.
	MAKE_SETTING(bSetting, "TrueScopesVR", sunEnabled, true);
	// Scale on the sun's light color for the scope render only (1.0 = engine value).
	// Doubles as a diagnostic: if lowering this does not dim the lens, the overbright
	// artifact is not coming through the light color path.
	MAKE_SETTING(fSetting, "TrueScopesVR", sunBrightnessScale, 1.0);
	// Let the sun pass write the specular accumulation target. Its spec output is
	// currently garbage (world-pos reconstruction constants — under investigation);
	// off = sun contributes diffuse only and spec stays cleared. Turn on to re-test.
	MAKE_SETTING(bSetting, "TrueScopesVR", sunSpecEnabled, false);
	// Suppress the vanilla scope-in world-blackout imagespace modifier (ScopeMenu's
	// full-strength zoomData imod). Core to the world+scope experience.
	MAKE_SETTING(bSetting, "TrueScopesVR", disableScopeBlackout, true);
	// Also suppress the eye-approach dimming fade (cosmetic; vanilla feel if left on).
	MAKE_SETTING(bSetting, "TrueScopesVR", disableApproachFade, false);

#undef MAKE_SETTING

	inline void load()
	{
		toml::table config;
		try {
			config = toml::parse_file("Data/F4SE/Plugins/TrueScopesVR.toml"sv);
		} catch (const std::exception& e) {
			logger::warn(FMT_STRING("TrueScopesVR.toml not loaded ({}), using defaults"), e.what());
			return;
		}

#define LOAD(a_setting)                                                              \
	if (const auto tweak = config[a_setting.group()][a_setting.key()]; tweak) {      \
		if (const auto value = tweak.as<decltype(a_setting)::value_type>(); value) { \
			*a_setting = value->get();                                               \
		}                                                                            \
	}

		LOAD(fillEnabled);
		LOAD(fillEveryNFrames);
		LOAD(forceAlwaysOn);
		LOAD(lensMode);
		LOAD(scopeFovDegrees);
		LOAD(scopeNearClip);
		LOAD(scopeFarClip);
		LOAD(scopeCamOffsetX);
		LOAD(scopeCamOffsetY);
		LOAD(scopeCamOffsetZ);
		LOAD(sunEnabled);
		LOAD(sunBrightnessScale);
		LOAD(sunSpecEnabled);
		LOAD(disableScopeBlackout);
		LOAD(disableApproachFade);

#undef LOAD

		logger::info(
			FMT_STRING("settings: fillEnabled={} fillEveryNFrames={} forceAlwaysOn={} lensMode={} scopeFovDegrees={} sunEnabled={} disableScopeBlackout={} disableApproachFade={}"),
			*fillEnabled, *fillEveryNFrames, *forceAlwaysOn, *lensMode, *scopeFovDegrees, *sunEnabled, *disableScopeBlackout, *disableApproachFade);
	}
}
