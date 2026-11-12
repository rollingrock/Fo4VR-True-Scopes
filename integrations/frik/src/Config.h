#pragma once

#include "ConfigBase.h"
#include "Resources.h"
#include "common/CommonUtils.h"

namespace my_mod
{
    static const auto BASE_MOD_PATH = BASE_PATH + "\\FO4VRScopeCalibration";
    static const auto INI_PATH = BASE_MOD_PATH + "\\FO4VRScopeCalibration.ini";

    class Config : public ConfigBase
    {
    public:
        explicit Config()
            : ConfigBase("FO4VRScopeCalibration", INI_PATH, IDR_CONFIG_INI)
        {}

        float panelX = -16.0f, panelY = 4.0f, panelZ = -5.0f;

    protected:
        virtual void loadIniConfigInternal(const CSimpleIniA& ini) override;
    };

    // Global singleton for easy access
    inline Config g_config;
}
