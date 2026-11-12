#include "FallbackAnchorCodec.h"
#include <iostream>
#include <stdexcept>
using namespace TrueScopes;
using namespace FallbackAnchor;
void require(bool v,const char*s){if(!v)throw std::runtime_error(s);}
void near(V a,V b){for(int i=0;i<3;++i)require(std::fabs(a[i]-b[i])<.00015f,"position drift");}
void near(M a,M b){for(int i=0;i<9;++i)require(std::fabs(a[i]-b[i])<.00015f,"rotation drift");}
M turn(float angle,int axis){const float c=std::cos(angle),s=std::sin(angle);return axis==0?M{1,0,0,0,c,-s,0,s,c}:axis==1?M{c,0,s,0,1,0,-s,0,c}:M{c,-s,0,s,c,0,0,0,1};}
int main(){try{
    Pose w,p,baseline;baseline.position={0,7,5};baseline.scale=.1565f;
    baseline.rotation=turn(.1f,1);
    Anchor a;require(capture(w,p,baseline,a),"capture failed");
    Calibration::Values v;v.right=5.6805043f;v.forward=9.685499f;v.up=2.9794633f;v.size=1.0412608f;
    Pose start;require(fit(a,w,p,v,start),"initial fit failed");
    near(start.position,add(baseline.position,{v.right,v.forward,v.up}));
    near(start.rotation,baseline.rotation);
    require(std::fabs(start.scale-baseline.scale*v.size)<.00001f,"size changed on initial fit");
    // Moving weapon independently of both hands: exactly the bracing failure.
    w.position={0,3,-1};Pose fitted;require(fit(a,w,p,v,fitted),"translated fit failed");
    near(toWorld(p,fitted.position),add(start.position,w.position));
    // Primary UI parent can change hands, rotate/translate/scale independently.
    // The desired world picture must be identical for every parent choice.
    for(int axis=0;axis<3;++axis)for(int i=0;i<200;++i){
        const float t=float(i)*.01f;
        w.position={20*t,-13*t,2*t};w.rotation=turn(t,axis);w.scale=.7f+t*.2f;
        p.position={-10*t,15*t,-4*t};p.rotation=turn(-t*.8f,(axis+1)%3);p.scale=.8f+t*.1f;
        require(fit(a,w,p,v,fitted),"moving fit refused");
        near(toWorld(p,fitted.position),toWorld(w,add(a.position,{v.right,v.forward,v.up})));
        near(multiply(p.rotation,fitted.rotation),multiply(w.rotation,a.rotation));
        require(std::fabs(p.scale*fitted.scale-a.scale*w.scale*v.size)<.00015f,"scale not weapon-relative");
    }
    // Preview tilt works even without upstream haveFace/census rotation state.
    w={};p={};v.pitch=12;v.yaw=-9;
    require(fit(a,w,p,v,fitted),"tilt failed");
    near(fitted.position,start.position);require(fitted.rotation!=start.rotation,"tilt missing");
    const Pose saved=fitted;
    for(int i=0;i<10000;++i){require(fit(a,w,p,v,fitted),"repeated fit failed");near(fitted.position,saved.position);near(fitted.rotation,saved.rotation);}
    // Cancel/reset are numeric transactions; the unadjusted baseline does not change.
    Calibration::Values preview=v;preview.right+=2;
    require(fit(a,w,p,preview,fitted),"preview failed");
    require(fit(a,w,p,v,fitted),"cancel failed");near(fitted.position,saved.position);near(fitted.rotation,saved.rotation);
    require(fit(a,w,p,{},fitted),"reset failed");near(fitted.position,baseline.position);near(fitted.rotation,baseline.rotation);
    // Re-equip/restart: stable keys and full baseline survive JSON roundtrip.
    const std::string key=R"(["handmadeantimaterielrifle.esp:000f9a","fallback",["mod.esp:00089a"]])";
    Records records{{key,a}};auto j=encode(records);auto restored=decode(nlohmann::json::parse(j.dump()));
    require(restored.size()==1&&restored.contains(key),"identity lost");near(restored.at(key).position,a.position);near(restored.at(key).rotation,a.rotation);
    auto bad=j;bad["anchors"][key]["scale"]=-1;bool rejected=false;
    try{(void)decode(bad);}catch(...){rejected=true;}require(rejected,"corrupt cache accepted");
    bad=j;bad["anchors"][key]["rotation"][0]=2;rejected=false;
    try{(void)decode(bad);}catch(...){rejected=true;}require(rejected,"non-rigid cache accepted");
    Pose invalid=w;invalid.scale=0;require(!fit(a,invalid,p,v,fitted),"zero scale accepted");
    invalid=w;invalid.position[0]=NAN;require(!capture(invalid,p,baseline,a),"NaN accepted");
    std::cout<<"PASS: independent weapon/UI translation and pitch/yaw/roll, scale, re-parenting, nonaccumulating tilt, reset/cancel and persistent baseline roundtrip; invalid data refused. NOT headset acceptance.\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
