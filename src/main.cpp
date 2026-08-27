#include "DevBench/DevBench.h"
#include "DevBenchClient/DevBenchClient.h"
#include "Settings/Settings.h"
#include "TrueScopes/Hooks.h"
#include "TrueScopes/ScopeRender.h"

namespace
{
	void InitializeLog()
	{
		auto path = logger::log_directory();
		if (!path) {
			// Broken install (no Documents redirection?): run without a log file
			// rather than CTD before logging exists.
			return;
		}
		const auto gamepath = REL::Module::IsVR() ? "Fallout4VR/F4SE" : "Fallout4/F4SE";
		if (!path.value().generic_string().ends_with(gamepath)) {
			// handle bug where game directory is missing
			path = path.value().parent_path().append(gamepath);
		}

		*path /= fmt::format("{}.log"sv, "TrueScopesVR"sv);
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);

		const auto level = spdlog::level::trace;

		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(level);
		log->flush_on(level);

		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%Y-%m-%d %T.%e][%-16s:%-4#][%L]: %v"s);
	}

	void MessageHandler(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		// Register our tools into alandtse/devbench at kPostPostLoad, not kPostLoad.
		// F4SEVR's RegisterListener(sender = nullptr) snapshots the listener slots that
		// exist at that moment and Dispatch only walks the sender's own slot, so a plugin
		// loading after devbench can dispatch into an empty list. devbench re-registers at
		// kPostLoad once the plugin list is complete; kPostPostLoad is dispatched right
		// after kPostLoad for exactly this second-phase handshake and does not depend on
		// load order. A no-op if devbench is absent.
		if (a_msg->type == F4SE::MessagingInterface::kGameDataReady) {
			// Unequip event sink for the teardown latch. Idempotent - GameDataReady
			// can fire more than once across save loads.
			TrueScopes::Hooks::RegisterEquipSink();
		}
		if (a_msg->type == F4SE::MessagingInterface::kPostPostLoad) {
			// True Scopes replaces Better Scopes - both patch the same scope pipeline
			// and cannot coexist. The byte-verify hooks already fail soft on the
			// conflict; this names the cause.
			if (::GetModuleHandleW(L"BetterScopesVR.dll")) {
				logger::critical(
					"BetterScopesVR.dll is loaded - True Scopes REPLACES Better Scopes and they "
					"cannot coexist. Disable one of them. Expect scope hooks to have declined."sv);
			}
			DevBenchClient::Register();
		}
	}
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	// Pack MAJOR.MINOR.PATCH decimally (0.2.140 -> 2140) so Buffout/crash logs
	// name the real build instead of 0.
	a_info->version = Version::MAJOR * 1000000 + Version::MINOR * 1000 + Version::PATCH;

	if (!REL::Module::IsVR()) {
		logger::critical("True Scopes VR requires Fallout 4 VR - unsupported binary, plugin disabled itself"sv);
		return false;
	}

	if (a_f4se->IsEditor()) {
		logger::critical("Loaded in editor, marking as incompatible"sv);
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < (REL::Module::IsF4() ? F4SE::RUNTIME_LATEST : F4SE::RUNTIME_LATEST_VR)) {
		logger::critical(FMT_STRING("Unsupported runtime version {}"), ver.string());
		return false;
	}

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	InitializeLog();
	F4SE::Init(a_f4se, false);

	logger::info(FMT_STRING("True Scopes VR v{}.{}.{} {} {} is loading"), Version::MAJOR, Version::MINOR, Version::PATCH, __DATE__, __TIME__);
	const auto runtimeVer = REL::Module::get().version();
	logger::info(FMT_STRING("Fallout 4 v{}.{}.{}"), runtimeVer[0], runtimeVer[1], runtimeVer[2]);

	Settings::load();

	if (!*Settings::enabled) {
		logger::info(
			"True Scopes VR disabled by TOML (enabled=false) - no hooks installed, vanilla "
			"scopes untouched. The shipped ScopeMenu SWFs still apply (they only remove the "
			"hold-breath pill); delete them from Data/Interface for a fully vanilla install. "
			"Restart after re-enabling."sv);
		return true;
	}

	// One allocation for all hook stubs (14 bytes each) - never per-hook, see the
	// write_thunk_call note in PCH.h.
	F4SE::AllocTrampoline(256);

	if (!TrueScopes::Hooks::Install()) {
		logger::critical("hook install failed — plugin inactive"sv);
		return true;  // stay loaded so the log tells the story, but do nothing
	}

	F4SE::GetMessagingInterface()->RegisterListener(MessageHandler);

	if (!TrueScopes::ScopeRender::Init()) {
		logger::error("ScopeRender init failed — lens will use the copy fill"sv);
	}

	// Dev tooling. Failure here is never fatal — the mod works without it.
	DevBench::Init();

	logger::info("True Scopes VR loaded"sv);
	return true;
}
