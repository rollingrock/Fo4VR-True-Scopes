#pragma once

#define NOMMNOSOUND

#include "RE/Fallout.h"
#include "F4SE/F4SE.h"

#pragma warning(disable: 4100)

#pragma warning(push)
#include <spdlog/sinks/basic_file_sink.h>
#include <xbyak/xbyak.h>
#include <toml++/toml.h>
#pragma warning(pop)

namespace logger = F4SE::log;

using namespace std::literals;

namespace pstl
{
	using namespace F4SE::stl;

	void asm_replace(std::uintptr_t a_from, std::size_t a_size, std::uintptr_t a_to);

	template <class T>
	void asm_replace(std::uintptr_t a_from)
	{
		asm_replace(a_from, T::size, reinterpret_cast<std::uintptr_t>(T::func));
	}

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

	template <class F, size_t offset, class T>
	void write_vfunc()
	{
		REL::Relocation<std::uintptr_t> vtbl{ F::VTABLE[offset] };
		T::func = vtbl.write_vfunc(T::idx, T::thunk);
	}

	template <class F, class T>
	void write_vfunc()
	{
		write_vfunc<F, 0, T>();
	}

	inline std::string as_string(std::string_view a_view)
	{
		return { a_view.data(), a_view.size() };
	}
}

#define DLLEXPORT __declspec(dllexport)

#include "Version.h"
