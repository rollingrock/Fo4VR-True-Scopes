#include "Config.h"

using namespace common;

namespace
{
    const char* DEFAULT_SECTION = "FO4VRScopeCalibration";
}

namespace my_mod
{
    void Config::loadIniConfigInternal(const CSimpleIniA& ini)
    {
        panelX = static_cast<float>(ini.GetDoubleValue(DEFAULT_SECTION, "ControllerX", -16.0));
        panelY = static_cast<float>(ini.GetDoubleValue(DEFAULT_SECTION, "ControllerY", 4.0));
        panelZ = static_cast<float>(ini.GetDoubleValue(DEFAULT_SECTION, "ControllerZ", -5.0));
        if (!std::isfinite(panelX) || !std::isfinite(panelY) || !std::isfinite(panelZ)) {
            panelX=-16.0f; panelY=4.0f; panelZ=-5.0f;
        }
    }
}
