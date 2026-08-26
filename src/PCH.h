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

	// NOTE: unlike the place-in-red template this does NOT call F4SE::AllocTrampoline
	// per hook. On F4SEVR (no branch-pool interface) AllocTrampoline falls back to
	// Trampoline::create, and create/set_trampoline release() the PREVIOUS buffer —
	// VirtualFreeing every previously written stub. With more than one hook that
	// orphans earlier call sites into freed memory (v0.1.1 crash,
	// crash-2026-08-06-23-13-56). Allocate the trampoline ONCE in F4SEPlugin_Load
	// with capacity for all hooks before calling this.
	template <class T>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = F4SE::GetTrampoline();
		T::func = trampoline.write_call<5>(a_src, T::thunk);
	}

}

#define DLLEXPORT __declspec(dllexport)

#include "Version.h"
