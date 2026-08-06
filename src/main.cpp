#include "Settings/Settings.h"
#include "TrueScopes/Hooks.h"
#include "TrueScopes/ScopeRender.h"

void InitializeLog()
{
	auto path = logger::log_directory();
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
	if (a_msg && a_msg->type == F4SE::MessagingInterface::kGameLoaded) {
		TrueScopes::Hooks::OnGameLoaded();
	}
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

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

	// One allocation for ALL hook stubs (14 bytes each) — never per-hook, see the
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

	logger::info("True Scopes VR loaded"sv);
	return true;
}
