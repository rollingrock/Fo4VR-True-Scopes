#pragma once

#include <array>

namespace optics_ui {

enum class Command { None, ScopePosition, ScopeTilt, Speed, ScopeHelp, Save, Reset, Cancel, ScopeBack };
using ButtonRows = std::array<std::array<Command, 4>, 2>;

inline constexpr ButtonRows scopeRows{{
    {Command::ScopePosition, Command::ScopeTilt, Command::Speed, Command::ScopeHelp},
    {Command::Save, Command::Reset, Command::Cancel, Command::None},
}};
inline constexpr ButtonRows helpRows{{
    {Command::None, Command::None, Command::None, Command::None},
    {Command::ScopeBack, Command::Cancel, Command::None, Command::None},
}};

inline constexpr const char* label(Command command)
{
    switch (command) {
    case Command::ScopePosition: return "Position";
    case Command::ScopeTilt: return "Tilt";
    case Command::Speed: return "Speed";
    case Command::ScopeHelp: return "Help";
    case Command::Save: return "Save";
    case Command::Reset: return "Reset";
    case Command::Cancel: return "Cancel";
    case Command::ScopeBack: return "Back";
    case Command::None: return "";
    }
    return "";
}

inline constexpr float scale = 1.6f;
inline constexpr float buttonScale = 1.4f;
inline constexpr float rowGap = 1.7f;
inline constexpr float verticalGap = 1.0f;

}
