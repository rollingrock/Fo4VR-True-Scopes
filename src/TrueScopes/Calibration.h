#pragma once
#include "CalibrationAPI.h"
#include <string>
namespace TrueScopes::Calibration
{
    // Renderer consumes a copied snapshot, only for the same completed identity probe.
    bool ForRender(Values& out, std::string& fallbackKey);
}
