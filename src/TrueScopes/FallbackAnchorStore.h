#pragma once
#include "FallbackAnchorMath.h"
#include <string>
#include <optional>
namespace TrueScopes::FallbackAnchor {
// File I/O only on the calibration/menu game-thread Read call. Render thread
// reads/writes a bounded map; a new baseline is flushed on the following Read.
void service();
std::optional<Anchor> get(const std::string& key);
bool insert(const std::string& key,const Anchor& anchor);
}
