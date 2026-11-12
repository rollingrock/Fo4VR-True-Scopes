#include "StickCalibration.h"
#include <cstdlib>
#include <cstdio>
#include <limits>
using namespace scope_controls;
using TrueScopes::Calibration::Values;
static void check(bool v){if(!v){std::puts("FAILED");std::exit(1);}}
static Values integrate(int hz){
    Motion m; Values v{},out{};
    m.update(v,{},1.0f/hz,false,out);
    for(int i=0;i<hz;++i){check(m.update(v,{1,-1,1,-1},1.0f/hz,false,out));v=out;}
    return v;
}
int main(){
    Motion m; Values v{},out{};
    check(!m.update(v,{1,0,0,0},.01f,true,out) && !m.armed());
    check(!m.update(v,{},.01f,true,out) && m.armed());
    check(!m.update(v,{.19f,-.19f,.19f,-.19f},.01f,true,out));
    check(m.update(v,{1,0,0,0},.1f,true,out) && out.right>0 && out.up==0 && out.forward==0 && out.size==1);
    check(m.update(v,{0,1,0,0},.1f,true,out) && out.up>0);
    check(m.update(v,{0,0,1,0},.1f,true,out) && out.size>1 && out.forward==0);
    check(m.update(v,{0,0,0,1},.1f,true,out) && out.forward>0 && out.size==1);
    check(!m.update(v,{1,0,0,0},.2f,true,out) && !m.armed());
    check(!m.update(v,{1,0,0,0},.01f,true,out));
    m.update(v,{},.01f,true,out);
    check(!m.update(v,{std::numeric_limits<float>::quiet_NaN(),0,0,0},.01f,true,out) && !m.armed());
    const auto a=integrate(72),b=integrate(120);
    check(std::fabs(a.right-b.right)<.0001f && std::fabs(a.size-b.size)<.0001f);
    check(std::fabs(a.right-2)<.0001f && std::fabs(a.forward+2)<.0001f && std::fabs(a.size-1.5f)<.0001f);
    m.reset(); m.update(v,{},.01f,true,out);
    check(m.update({24.999f,0,0,3.999f},{1,0,1,0},.1f,false,out));
    check(out.right==25 && out.size==4);
    check(!m.update(v,{0,0,1,1,true},.05f,false,out) && !m.armed());
    check(!m.update(v,{0,0,0,0,true},.05f,false,out) && m.armed());
    check(m.update(v,{1,1,1,1,true},.05f,false,out));
    check(out.pitch>0 && out.yaw<0 && out.size==1 && out.right==0 && out.up==0 && out.forward==0);
    // A sustained modifier (or explicit Tilt selector) must stay active over
    // multiple frames; no size/depth leak while rotating.
    Values accumulated{};
    for(int i=0;i<120;++i){check(m.update(accumulated,{0,0,1,1,true},1.f/120,false,out));accumulated=out;}
    check(std::fabs(accumulated.pitch-8.f)<.001f && std::fabs(accumulated.yaw+8.f)<.001f);
    check(accumulated.forward==0 && accumulated.size==1);
    check(!m.update(v,{0,0,1,1,false},.05f,false,out) && !m.armed());
    std::puts("PASS: mappings, deadzone, neutral arming, stall/nonfinite reset, 72/120Hz equivalence and clamps");
}
