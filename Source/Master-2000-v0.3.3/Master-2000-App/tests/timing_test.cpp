#define main master2000_engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <cassert>
int main(){
    TimingSignal a,b; a.step=b.step=1;
    a.values.resize(2000);b.values.resize(2000);
    unsigned rng=12345;
    for(int i=0;i<2000;++i){rng=1664525*rng+1013904223;
        a.values[i]=((rng>>8)%10000)/5000.0-1.0;}
    for(int i=13;i<2000;++i)b.values[i]=2.5*a.values[i-13]+0.3;
    auto m=matchTiming(a,b,200,400,0,40);
    assert(m.accepted && m.delay==13 && m.q>0.999);
    auto reverse=matchTiming(b,a,200,400,0,40);
    assert(reverse.accepted && reverse.delay==-13);
    std::fill(b.values.begin(),b.values.end(),0);
    assert(!matchTiming(a,b,200,400,0,40).accepted);
    for(int i=0;i<2000;++i)a.values[i]=b.values[i]=std::sin(i*6.283185307179586/20);
    assert(!matchTiming(a,b,200,400,0,40).accepted);

    // A periodic raw waveform has several equal phase peaks. Once the time
    // map supplies the right basin, phase search must keep the nearest peak
    // rather than jumping a complete musical cycle.
    Wav phaseRef,phaseDonor;
    phaseRef.sampleRate=phaseDonor.sampleRate=96000;
    phaseRef.channels=phaseDonor.channels=1;
    phaseRef.bits=phaseDonor.bits=24;
    phaseRef.samples.resize(12000);phaseDonor.samples.resize(12000);
    for(int i=0;i<12000;++i) {
        const float v=(float)std::sin(i*6.283185307179586/120.0);
        phaseRef.samples[(size_t)i]=v;
        phaseDonor.samples[(size_t)i]=v;
    }
    const auto phase=searchPhaseNearTrajectory(
        phaseRef,phaseDonor,2000,2304,0.0,96000);
    assert(std::labs(phase.delay)<=1 && phase.corr>.999);

    // v0.2.31: a donor outside the primary backward pair may follow the already
    // established path only when both independent bridge donors confirm it.
    assert(additionalBackwardBridgePass(
        true,10.0,0.0,5.0,0.80,0.75,96000));
    assert(!additionalBackwardBridgePass(
        true,10.0,0.0,5.0,0.80,0.54,96000));
    assert(!additionalBackwardBridgePass(
        true,500.0,0.0,5.0,0.80,0.75,96000));
    assert(!additionalBackwardBridgePass(
        true,10.0,0.0,500.0,0.80,0.75,96000));
    assert(!additionalBackwardBridgePass(
        false,10.0,0.0,5.0,0.80,0.75,96000));

    // A lone early ordinary-map anchor must remain diagnostic-only when the
    // dense backward chain stops later. Delay data is retained for display,
    // while repair authority begins exactly at the last proven point.
    std::vector<TimeMapAnchor> prefix{
        {96000,100.0,0.90,true,true},
        {192000,110.0,0.91,true,true},
        {230400,120.0,0.92,true,true},
        {240000,125.0,0.93,true,true}
    };
    assert(blockRepairBefore(prefix,230400)==2);
    assert(prefix[0].verified && !prefix[0].repairSafe);
    assert(prefix[1].verified && !prefix[1].repairSafe);
    assert(prefix[2].verified && prefix[2].repairSafe);
    assert(prefix[3].verified && prefix[3].repairSafe);
    assert(!denseBackwardMayReachZero(230400,115200,8,4));
    assert(!denseBackwardMayReachZero(115200,115200,3,4));
    assert(denseBackwardMayReachZero(115200,115200,4,4));

    std::cout<<"Timing helper: shift signs, gain/DC, silence, trajectory-prior phase, periodic ambiguity, all-donor backward witness gate and unsafe-prefix block passed\n";
}
