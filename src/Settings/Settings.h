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

#undef LOAD

		logger::info(
			FMT_STRING("settings: fillEnabled={} fillEveryNFrames={} forceAlwaysOn={}"),
			*fillEnabled, *fillEveryNFrames, *forceAlwaysOn);
	}
}
