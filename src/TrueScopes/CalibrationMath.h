#pragma once
#include "CalibrationAPI.h"
#include <cmath>

namespace TrueScopes::Calibration
{
    inline bool Valid(const Values& v) noexcept
    {
        return std::isfinite(v.right) && std::isfinite(v.forward) && std::isfinite(v.up) &&
            std::isfinite(v.size) && std::fabs(v.right) <= 25.0f && std::fabs(v.forward) <= 25.0f &&
            std::fabs(v.up) <= 25.0f && v.size >= 0.25f && v.size <= 4.0f &&
            std::isfinite(v.pitch) && std::isfinite(v.yaw) && std::fabs(v.pitch)<=30.0f && std::fabs(v.yaw)<=30.0f;
    }
    inline bool Identity(const Values& v) noexcept
    {
        return v.right == 0.0f && v.forward == 0.0f && v.up == 0.0f && v.size == 1.0f && v.pitch==0 && v.yaw==0;
    }
    // Rotate the picture about its own centre in weapon axes, not world/hand
    // axes: L' = Rp^T Rw (Rz(yaw) Rx(pitch)) Rw^T Rp L.
    // baseline and output may alias. Zero tilt copies exactly, never accumulates.
    inline bool Tilt(const Values& v, const float* weapon, const float* parent,
                     const float* baseline, float* out) noexcept
    {
        if(!Valid(v)) return false;
        for(int i=0;i<9;++i) if(!std::isfinite(weapon[i]) || !std::isfinite(parent[i]) || !std::isfinite(baseline[i])) return false;
        if(v.pitch==0 && v.yaw==0){for(int i=0;i<9;++i)out[i]=baseline[i];return true;}
        constexpr float radians=.017453292519943295f;
        const float cp=std::cos(v.pitch*radians),sp=std::sin(v.pitch*radians);
        const float cy=std::cos(v.yaw*radians),sy=std::sin(v.yaw*radians);
        const float t[9]{cy,-sy*cp,sy*sp, sy,cy*cp,-cy*sp, 0,sp,cp};
        float relation[9]{},local[9]{},tilted[9]{},result[9]{};
        for(int r=0;r<3;++r)for(int c=0;c<3;++c)for(int k=0;k<3;++k)
            relation[r*3+c]+=parent[k*3+r]*weapon[k*3+c];
        for(int r=0;r<3;++r)for(int c=0;c<3;++c)for(int k=0;k<3;++k)
            local[r*3+c]+=relation[k*3+r]*baseline[k*3+c];
        for(int r=0;r<3;++r)for(int c=0;c<3;++c)for(int k=0;k<3;++k)
            tilted[r*3+c]+=t[r*3+k]*local[k*3+c];
        for(int r=0;r<3;++r)for(int c=0;c<3;++c)for(int k=0;k<3;++k)
            result[r*3+c]+=relation[r*3+k]*tilted[k*3+c];
        for(int i=0;i<9;++i)out[i]=result[i];
        return true;
    }
    // Both matrices are true row-major rotations. Convert weapon-local displacement
    // through world space into the widget parent's local frame. Never accumulate output.
    inline bool Delta(const Values& v, const float* weapon, float weaponScale,
        const float* parent, float parentScale, float* out) noexcept
    {
        if (!Valid(v) || !std::isfinite(weaponScale) || !std::isfinite(parentScale) ||
            weaponScale < 0.0001f || parentScale < 0.0001f) return false;
        for (int i = 0; i < 9; ++i)
            if (!std::isfinite(weapon[i]) || !std::isfinite(parent[i])) return false;
        const float local[3]{v.right, v.forward, v.up};
        float world[3]{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) world[r] += weapon[r * 3 + c] * local[c] * weaponScale;
        for (int r = 0; r < 3; ++r) {
            out[r] = 0;
            for (int c = 0; c < 3; ++c) out[r] += parent[c * 3 + r] * world[c] / parentScale;
            if (!std::isfinite(out[r])) return false;
        }
        return true;
    }
}
