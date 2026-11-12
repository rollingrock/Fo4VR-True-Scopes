#pragma once
#include "CalibrationMath.h"
#include <algorithm>
#include <cstring>

namespace TrueScopes::Calibration
{
    // Pure identity/preview logic. The owning runtime supplies synchronization.
    class Session
    {
    public:
        State state() const { return _state; }
        void observe(State next)
        {
            if (next.modCount > 24 || !next.weapon || !next.optic[0] || next.optic[127]) next.ready = 0;
            if (!next.ready) {
                if (_state.ready) { _state = State{}; _state.epoch = ++_epoch; }
                return;
            }
            std::sort(next.mods, next.mods + next.modCount);
            const bool same = _state.ready && next.weapon == _state.weapon &&
                std::strcmp(next.optic, _state.optic) == 0 && next.modCount == _state.modCount &&
                std::equal(next.mods, next.mods + next.modCount, _state.mods);
            next.epoch = same ? _state.epoch : ++_epoch;
            next.values = same ? _state.values : Values{};
            _state = next;
        }
        bool set(std::uint64_t epoch, const Values& values)
        {
            if (!_state.ready || _state.epoch != epoch || !Valid(values)) return false;
            _state.values = values;
            return true;
        }
        bool render(std::uint32_t weapon, const char* optic, std::uint32_t probe, Values& out) const
        {
            if (!_state.ready || weapon != _state.weapon || probe != _state.probe ||
                !optic || std::strcmp(optic, _state.optic) != 0 || Identity(_state.values)) return false;
            out = _state.values;
            return true;
        }
    private:
        State _state{};
        std::uint64_t _epoch = 0;
    };
}
