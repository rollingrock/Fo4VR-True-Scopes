#include "CalibrationMath.h"
#include "CalibrationSession.h"
#include <cstdio>
#include <limits>
#include <cstdlib>
using namespace TrueScopes::Calibration;
static void check(bool pass) { if (!pass) { std::puts("FAILED"); std::exit(1); } }
int main()
{
    const float identity[9]{1,0,0,0,1,0,0,0,1};
    const float rotated[9]{0,-1,0,1,0,0,0,0,1};
    float out[3];
    check(Valid({}) && Identity({}));
    check(!Valid({0,0,0,0}) && !Valid({26,0,0,1}));
    check(!Valid({0,0,std::numeric_limits<float>::quiet_NaN(),1}));
    check(Delta({1,2,3,1}, identity, 1, identity, 1, out));
    check(out[0] == 1 && out[1] == 2 && out[2] == 3);
    check(Delta({1,2,3,1}, rotated, 1, identity, 1, out));
    check(out[0] == -2 && out[1] == 1 && out[2] == 3);
    check(Delta({1,2,3,1}, rotated, 2, rotated, 4, out));
    check(out[0] == .5f && out[1] == 1 && out[2] == 1.5f);
    check(!Delta({}, identity, 1, identity, 0, out));
    float tilt[9];
    Values angled{0,0,0,1,10,15};
    check(!Identity(angled) && Valid(angled) && !Valid({0,0,0,1,31,0}));
    check(Tilt({},rotated,identity,rotated,tilt));
    for(int i=0;i<9;++i)check(tilt[i]==rotated[i]);
    check(Tilt(angled,rotated,rotated,identity,tilt));
    for(int r=0;r<3;++r)for(int c=0;c<3;++c){
        float dot=0;for(int k=0;k<3;++k)dot+=tilt[r*3+k]*tilt[c*3+k];
        check(std::fabs(dot-(r==c?1.f:0.f))<1e-5f);
    }
    float independent[9];
    check(Tilt(angled,identity,identity,identity,independent));
    for(int i=0;i<9;++i)check(std::fabs(independent[i]-tilt[i])<1e-5f);
    for(int i=0;i<10000;++i){check(Tilt(angled,rotated,rotated,identity,tilt));
        for(int j=0;j<9;++j)check(std::fabs(independent[j]-tilt[j])<1e-5f);}
    for (int i = 0; i < 10000; ++i) {
        check(Delta({1,2,3,2}, rotated, 1, identity, 1, out));
        check(out[0] == -2 && out[1] == 1 && out[2] == 3);
    }
    std::puts("PASS: identity, limits, rotation, inverse parent scale and 10000 non-accumulating frames");
    Session session;
    State a{};
    a.ready=1; a.weapon=42; a.probe=1; a.modCount=2; a.mods[0]=100; a.mods[1]=200;
    std::memcpy(a.optic,"optic_a.nif",sizeof("optic_a.nif"));
    session.observe(a);
    const auto epoch = session.state().epoch;
    Values v;
    check(!session.render(42,a.optic,1,v)); // No saved adjustment: strict no-op.
    check(session.set(epoch,{1,2,3,1.5f}));
    check(session.render(42,a.optic,1,v) && v.size==1.5f);
    check(!session.render(43,a.optic,1,v) && !session.render(42,"other",1,v));
    check(!session.render(42,a.optic,2,v)); // New receiver probe cannot consume stale preview.
    a.probe=2; a.mods[0]=200; a.mods[1]=100; session.observe(a);
    check(session.state().epoch==epoch && session.render(42,a.optic,2,v));
    check(!session.set(epoch,{0,0,0,9}));
    a.mods[0]=300; session.observe(a); // New receiver, same gun and generic optic name.
    check(session.state().epoch!=epoch && !session.set(epoch,{1,0,0,1}));
    check(Identity(session.state().values));
    const auto receiverEpoch = session.state().epoch;
    session.observe({});
    check(!session.set(receiverEpoch,{1,0,0,1}) && !session.state().ready);
    session.observe(a);
    check(session.state().epoch!=receiverEpoch && Identity(session.state().values));
    a.modCount=25; session.observe(a); check(!session.state().ready);
    std::puts("PASS: default no-op, gun/optic/receiver isolation, OMOD order, stale writes, load/unequip and overflow");
}
