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

	template <class T>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = F4SE::GetTrampoline();
		F4SE::AllocTrampoline(14);

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
