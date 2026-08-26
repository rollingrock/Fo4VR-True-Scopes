#pragma once

#define NOMMNOSOUND

#include "RE/Fallout.h"
#include "F4SE/F4SE.h"

#pragma warning(disable: 4100)

#pragma warning(push)
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <toml++/toml.h>
#pragma warning(pop)

namespace logger = F4SE::log;

using namespace std::literals;

namespace pstl
{
	using namespace F4SE::stl;

	// Unlike the place-in-red template this does not call F4SE::AllocTrampoline
	// per hook. On F4SEVR (no branch-pool interface) AllocTrampoline falls back to
	// Trampoline::create, and create/set_trampoline release() the previous buffer -
	// VirtualFreeing every previously written stub. With more than one hook that
	// orphans earlier call sites into freed memory. Allocate the trampoline once
	// in F4SEPlugin_Load with capacity for all hooks before calling this.
	template <class T>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = F4SE::GetTrampoline();
		T::func = trampoline.write_call<5>(a_src, T::thunk);
	}

}

#define DLLEXPORT __declspec(dllexport)

#include "Version.h"
