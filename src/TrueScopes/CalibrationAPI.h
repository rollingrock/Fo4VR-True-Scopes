#pragma once
#include <cstdint>

// Public POD-only ABI. No FRIK, CommonLib, STL objects, allocation, or node pointers cross DLLs.
// Read/Set are game-thread calls. Caller owns State storage. New versions use a new contract.
namespace TrueScopes::Calibration
{
    inline constexpr std::uint32_t kVersion = 2;
    struct Values {
        float right = 0.0f;
        float forward = 0.0f;
        float up = 0.0f;
        float size = 1.0f;
        float pitch = 0.0f; // degrees, weapon-local X
        float yaw = 0.0f;   // degrees, weapon-local Z
    };
    struct State {
        std::uint32_t bytes = sizeof(State);
        std::uint32_t ready = 0;
        std::uint64_t epoch = 0;
        std::uint32_t probe = 0;
        std::uint32_t weapon = 0;
        std::uint32_t modCount = 0;
        std::uint32_t mods[24]{};
        char optic[128]{};
        Values values{};
    };
    struct API {
        std::uint32_t bytes;
        std::uint32_t version;
        std::uint32_t (__cdecl* read)(State* state);
        std::uint32_t (__cdecl* set)(std::uint64_t epoch, const Values* values);
    };
    using GetAPI = const API* (__cdecl*)(std::uint32_t version, std::uint32_t bytes);
}
