#define main engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <cassert>
int main() {
    const int sr=96000; const long seg=4800, end=4992;
    std::vector<Wav> src(3);
    for(auto& w:src) {w.sampleRate=sr;w.channels=1;w.bits=24;w.samples.resize(9600);}
    for(int i=0;i<9600;++i) {
        double t=(double)i/sr;
        double x=.13*sin(2*3.141592653589793*220*t)+.03*sin(2*3.141592653589793*370*t);
        src[0].samples[i]=(float)x;
        src[1].samples[i]=(float)(x*.8+.01);
        src[2].samples[i]=.01f;
    }
    std::vector<double> delay(3,0);
    assert(!detectImpulseConsensus(src,0,seg,end,delay,sr).confirmed);
    src[0].samples[4896]+=.4f;
    auto e=detectImpulseConsensus(src,0,seg,end,delay,sr);
    // A flat/empty third transfer may look click-free, but it is not a strong
    // second witness for the surrounding musical curve.
    assert(e.impulsePatternConfirmed && !e.confirmed);
    for(int i=0;i<9600;++i) {
        double t=(double)i/sr;
        double x=.13*sin(2*3.141592653589793*220*t)+.03*sin(2*3.141592653589793*370*t);
        src[2].samples[i]=(float)(x*.9-.005);
    }
    e=detectImpulseConsensus(src,0,seg,end,delay,sr);
    assert(e.confirmed && e.donorSource==1);
    assert(std::abs(e.trendGain-1.25)<.001);
    assert(std::abs(e.trendDc+.0125)<.001);
    src[1].samples[4896]+=.4f;
    assert(!detectImpulseConsensus(src,0,seg,end,delay,sr).confirmed);
    src[1].samples.assign(9600,.02f);
    e=detectImpulseConsensus(src,0,seg,end,delay,sr);
    assert(e.impulsePatternConfirmed && !e.confirmed);
    assert(!localTrendMatch(src,0,1,0,192,delay,sr).usable);

    // Two clean donors may each match Master inside the normal gain range
    // while differing by more than 2.85x from each other. The witness-to-
    // witness check must use the composed range instead of rejecting valid
    // evidence solely because one transfer is much quieter than the other.
    std::vector<Wav> levelSrc(3);
    for(auto& w:levelSrc) {
        w.sampleRate=sr;w.channels=1;w.bits=24;w.samples.resize(9600);
    }
    for(int i=0;i<9600;++i) {
        const double t=(double)i/sr;
        const double x=.13*sin(2*3.141592653589793*220*t)+
            .03*sin(2*3.141592653589793*370*t);
        levelSrc[0].samples[i]=(float)x;
        levelSrc[1].samples[i]=(float)(.53*x+.004);
        levelSrc[2].samples[i]=(float)(2.0*x-.006);
    }
    levelSrc[0].samples[4896]+=.4f;
    const auto levelEvidence=detectImpulseConsensus(
        levelSrc,0,seg,end,delay,sr);
    assert(levelEvidence.confirmed);
    assert(levelEvidence.supportingWitnesses==2);

    std::vector<TimeMapAnchor> map;
    for(int i=0;i<9;++i) {
        TimeMapAnchor a;
        a.masterFrame=i*sr/4;
        a.donorDelay=(i/3)*-760.0; // deliberately stepped periodic lock
        a.corr=.98;
        a.verified=true;
        map.push_back(a);
    }
    stabilizeTimeMap(map,sr,0);
    double largestStep=0.0;
    for(size_t i=1;i<map.size();++i)
        largestStep=std::max(largestStep,
            std::abs(map[i].donorDelay-map[i-1].donorDelay));
    assert(largestStep<760.0);
    assert(mappedConfidenceAt(map,sr,sr)>.72);

    AffineDelayModel transfer;
    transfer.intercept=1200.0;
    transfer.slope=.032;
    transfer.valid=true;
    std::vector<TimeMapAnchor> fastTransfer;
    for(int i=0;i<17;++i) {
        TimeMapAnchor a;
        a.masterFrame=i*sr/4;
        a.donorDelay=transfer.delayAt(a.masterFrame)+((i%3)-1)*80.0;
        a.corr=.96;
        a.verified=true;
        fastTransfer.push_back(a);
    }
    stabilizeTimeMap(fastTransfer,sr,0,&transfer);
    const double publishedSlope=
        (fastTransfer.back().donorDelay-fastTransfer.front().donorDelay)/
        (fastTransfer.back().masterFrame-fastTransfer.front().masterFrame);
    assert(std::abs(publishedSlope-.032)<.002);
    assert(mappedConfidenceAt(fastTransfer,2*sr,sr)>.72);

    // A plausible onset is not allowed to override the programme path unless
    // its own multi-window trajectory is nearly as strong as the winner. The
    // observed x2h failure had 0/7 onset probes (score -2) versus 7/7 on the
    // whole-record basin (score about .94).
    assert(!onsetBasinMayOverride(-2.0,.940165,true,.72));
    assert(!onsetBasinMayOverride(.78,.940165,true,.72));
    assert(onsetBasinMayOverride(.92,.940165,true,.72));
    assert(!onsetBasinMayOverride(.94,.940165,false,.72));

    // When onset and programme continuity agree, the onset may still correct
    // an absolute periodic basin without changing the fitted speed slope.
    AffineDelayModel wrongBasin;
    wrongBasin.valid=true;
    wrongBasin.intercept=5452.0;
    wrongBasin.slope=-0.000836341;
    const long attackFrame=0;
    const auto onsetLock=anchorAffineBasinToOnset(
        wrongBasin,attackFrame,14863.0,true,sr*25/1000);
    assert(onsetLock.applied);
    assert(std::abs(onsetLock.interceptOffset-9411.0)<1e-9);
    assert(std::abs(onsetLock.model.delayAt(attackFrame)-14863.0)<1e-9);
    assert(onsetLock.model.slope==wrongBasin.slope);
    const auto weakOnset=anchorAffineBasinToOnset(
        wrongBasin,attackFrame,14863.0,false,sr*25/1000);
    assert(!weakOnset.applied && weakOnset.model.intercept==wrongBasin.intercept);

    // A shifted affine basin is diagnostic-only until four dense local points
    // independently verify the corrected path.
    std::vector<TimeMapAnchor> onsetAnchors;
    for(int i=0;i<6;++i) {
        TimeMapAnchor a;
        a.masterFrame=i*sr/4;
        a.donorDelay=onsetLock.model.delayAt(a.masterFrame)+(i%2?40.0:-40.0);
        a.corr=.95;
        a.verified=true;
        a.repairSafe=true;
        onsetAnchors.push_back(a);
    }
    const long unlock=gateDisputedBasinRepairs(
        onsetAnchors,onsetLock.model,sr,0);
    assert(unlock==3*sr/4);
    for(int i=0;i<3;++i) assert(!onsetAnchors[(size_t)i].repairSafe);
    for(int i=3;i<6;++i) assert(onsetAnchors[(size_t)i].repairSafe);

    auto brokenAnchors=onsetAnchors;
    for(auto& a:brokenAnchors) a.repairSafe=true;
    brokenAnchors[2].repairSafe=false;
    assert(gateDisputedBasinRepairs(
        brokenAnchors,onsetLock.model,sr,0)==-1);
    for(const auto& a:brokenAnchors) assert(!a.repairSafe);

    std::vector<TimeMapAnchor> impossible(2);
    impossible[0]={0,-443712.0,.80,true,true};
    impossible[1]={sr/4,82755.0,.99,true,true};
    assert(mappedConfidenceAt(impossible,sr/8,sr)==0.0);
    assert(mappedDelayAt(impossible,sr/16,sr)==-443712.0);
    assert(mappedDelayAt(impossible,3*sr/16,sr)==82755.0);

    // A stable early map may safely bracket frame zero by carrying the affine
    // speed slope backwards. It remains only timing authority; downstream
    // local waveform and two-witness gates still decide every repair.
    AffineDelayModel earlyModel;
    earlyModel.intercept=11105.0;
    earlyModel.slope=-0.00018;
    earlyModel.valid=true;
    std::vector<TimeMapAnchor> earlyMap;
    for(int i=0;i<5;++i) {
        const long frame=(long)((1.10+.10*i)*sr);
        earlyMap.push_back({frame,earlyModel.delayAt(frame)+(i%2?2.0:-2.0),
                            .94,true,true});
    }
    const auto zeroAnchor=makeBackwardZeroAnchor(
        earlyMap,sr,&earlyModel,0.0);
    assert(zeroAnchor.valid && zeroAnchor.anchor.masterFrame==0);
    assert(zeroAnchor.anchor.verified && zeroAnchor.anchor.repairSafe);
    assert(std::abs(zeroAnchor.anchor.donorDelay-earlyModel.delayAt(0))<5.0);

    auto unstableEarly=earlyMap;
    unstableEarly[1].donorDelay+=sr/20;
    assert(!makeBackwardZeroAnchor(
        unstableEarly,sr,&earlyModel,0.0).valid);

    std::cout << "PASS: trend donor logic, level-ratio control, backward-zero bracket, onset veto/affine lock and discontinuity rejection\n";
}
