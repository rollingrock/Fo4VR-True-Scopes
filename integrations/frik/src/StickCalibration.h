#pragma once
#include "CalibrationMath.h"
#include <algorithm>
#include <cmath>

namespace scope_controls
{
    struct Sticks { float supportX=0, supportY=0, primaryX=0, primaryY=0; bool tilt=false; };
    class Motion
    {
    public:
        void reset() { _armed=false; }
        bool armed() const { return _armed; }
        bool update(const TrueScopes::Calibration::Values& start, Sticks input, float dt, bool fine,
                    TrueScopes::Calibration::Values& result)
        {
            result=start;
            const float a[]{input.supportX,input.supportY,input.primaryX,input.primaryY};
            for (float v:a) if (!std::isfinite(v)) { reset(); return false; }
            if (!std::isfinite(dt) || dt<=0 || dt>.10f) { reset(); return false; }
            if(input.tilt!=_tilt){_tilt=input.tilt;reset();}
            if (!_armed) {
                _armed=std::all_of(std::begin(a),std::end(a),[](float v){return std::fabs(v)<=.20f;});
                return false; // require one centered sample before consuming adjustments
            }
            const auto axis=[](float v) {
                v=std::clamp(v,-1.0f,1.0f);
                return std::fabs(v)<=.20f ? 0.0f : std::copysign((std::fabs(v)-.20f)/.80f,v);
            };
            const float speed=fine ? .25f : 2.0f;
            if(input.tilt){
                const float angular=fine ? 1.0f : 8.0f;
                result.pitch=std::clamp(start.pitch+axis(input.primaryY)*angular*dt,-30.0f,30.0f);
                result.yaw=std::clamp(start.yaw-axis(input.primaryX)*angular*dt,-30.0f,30.0f);
                return result.pitch!=start.pitch || result.yaw!=start.yaw;
            }
            result.right=std::clamp(start.right+axis(input.supportX)*speed*dt,-25.0f,25.0f);
            result.up=std::clamp(start.up+axis(input.supportY)*speed*dt,-25.0f,25.0f);
            result.forward=std::clamp(start.forward+axis(input.primaryY)*speed*dt,-25.0f,25.0f);
            result.size=std::clamp(start.size+axis(input.primaryX)*(fine ? .10f : .50f)*dt,.25f,4.0f);
            return result.right!=start.right || result.up!=start.up || result.forward!=start.forward || result.size!=start.size;
        }
    private:
        bool _armed=false;
        bool _tilt=false;
    };
}
