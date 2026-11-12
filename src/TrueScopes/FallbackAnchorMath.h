#pragma once
#include "CalibrationMath.h"
#include <array>
#include <algorithm>
namespace TrueScopes::FallbackAnchor {
using V=std::array<float,3>;
using M=std::array<float,9>;
inline constexpr M I{1,0,0,0,1,0,0,0,1};
struct Pose { V position{}; M rotation=I; float scale=1; };
// Baseline pose in WEAPON space, deliberately excluding personal calibration.
struct Anchor { V position{}; M rotation=I; float scale=1; };
inline M transpose(const M&m){M r{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)r[i*3+j]=m[j*3+i];return r;}
inline M multiply(const M&a,const M&b){M r{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)for(int k=0;k<3;++k)r[i*3+j]+=a[i*3+k]*b[k*3+j];return r;}
inline V rotate(const M&m,V p){V r{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)r[i]+=m[i*3+j]*p[j];return r;}
inline V add(V a,V b){for(int i=0;i<3;++i)a[i]+=b[i];return a;}
inline V sub(V a,V b){for(int i=0;i<3;++i)a[i]-=b[i];return a;}
inline V scaled(V a,float s){for(auto&v:a)v*=s;return a;}
inline bool rotationValid(const M&m){
    for(float x:m)if(!std::isfinite(x))return false;
    const auto unit=multiply(transpose(m),m);
    for(int i=0;i<9;++i)if(std::fabs(unit[i]-I[i])>.015f)return false;
    const float det=m[0]*(m[4]*m[8]-m[5]*m[7])-m[1]*(m[3]*m[8]-m[5]*m[6])+m[2]*(m[3]*m[7]-m[4]*m[6]);
    return std::fabs(det-1)<.02f;
}
inline bool valid(const Pose&p){
    for(float x:p.position)if(!std::isfinite(x))return false;
    return std::isfinite(p.scale)&&p.scale>.0001f&&p.scale<10000&&rotationValid(p.rotation);
}
inline bool valid(const Anchor&a){
    for(float x:a.position)if(!std::isfinite(x)||std::fabs(x)>150)return false;
    return std::isfinite(a.scale)&&a.scale>.0001f&&a.scale<8&&rotationValid(a.rotation);
}
inline V toWorld(const Pose&p,V local){return add(p.position,scaled(rotate(p.rotation,local),p.scale));}
inline V toLocal(const Pose&p,V world){return scaled(rotate(transpose(p.rotation),sub(world,p.position)),1/p.scale);}
inline bool capture(const Pose&weapon,const Pose&parent,const Pose&baseline,Anchor&out){
    if(!valid(weapon)||!valid(parent)||!valid(baseline))return false;
    Anchor a;
    a.position=toLocal(weapon,toWorld(parent,baseline.position));
    a.rotation=multiply(transpose(weapon.rotation),multiply(parent.rotation,baseline.rotation));
    a.scale=baseline.scale*parent.scale/weapon.scale;
    if(!valid(a))return false;
    out=a;return true;
}
inline bool fit(const Anchor&a,const Pose&weapon,const Pose&parent,const Calibration::Values&v,Pose&out){
    if(!valid(a)||!valid(weapon)||!valid(parent)||!Calibration::Valid(v))return false;
    Pose p;
    p.position=toLocal(parent,toWorld(weapon,add(a.position,{v.right,v.forward,v.up})));
    // Same centre-preserving yaw/pitch convention as the existing calibration.
    constexpr float rad=.017453292519943295f;
    const float cp=std::cos(v.pitch*rad),sp=std::sin(v.pitch*rad),cy=std::cos(v.yaw*rad),sy=std::sin(v.yaw*rad);
    const M tilt{cy,-sy*cp,sy*sp,sy,cy*cp,-cy*sp,0,sp,cp};
    p.rotation=multiply(transpose(parent.rotation),multiply(weapon.rotation,multiply(tilt,a.rotation)));
    p.scale=a.scale*weapon.scale/parent.scale*v.size;
    if(!valid(p)||p.scale>=8)return false;
    out=p;return true;
}
}
