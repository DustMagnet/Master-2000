#define main master2000_engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    constexpr int sr=96000;
    const long detectedStart=(long)std::llround(9.086000*sr);
    const long detectedEnd=(long)std::llround(9.090000*sr);
    const long missedLeadingEdge=(long)std::llround(9.085725*sr);
    const auto bounds=expandedRepairWindow(
        detectedStart,detectedEnd,20L*sr,sr);

    assert(repairBoundaryGuardFrames(sr)==48);
    assert(bounds.start<=missedLeadingEdge);
    assert(bounds.end>=detectedEnd+48);

    std::vector<std::string> gateReason{
        "ELIGIBLE_SCORE","ELIGIBLE_SCORE","ELIGIBLE_SCORE"};
    assert(mayAbsorbAdjacentUncertainCell(
        'B',"B_QUALITY_DIFFERENCE_KEEP_MASTER",3.88705698,
        gateReason,1));
    assert(!mayAbsorbAdjacentUncertainCell(
        'B',"B_QUALITY_DIFFERENCE_KEEP_MASTER",2.49,
        gateReason,1));
    assert(!mayAbsorbAdjacentUncertainCell(
        'C',"C_FASTPATH_MASTER",8.0,gateReason,1));
    assert(!mayAbsorbAdjacentUncertainCell(
        'B',"B_POSTCHECK_CONSENSUS_LOST",8.0,gateReason,1));
    gateReason[1]="LOCAL_VERIFY_FAIL";
    assert(!mayAbsorbAdjacentUncertainCell(
        'B',"B_QUALITY_DIFFERENCE_KEEP_MASTER",8.0,
        gateReason,1));

    // v0.2.31 real-run regression at 8.560 s: a marginal 1.961x event must
    // never replace a broad 3.5 ms musical contour.  The same evidence may
    // still authorize a truly sample-localized core, while a strong broad
    // event retains the established automatic repair path.
    assert(!broadRepairEvidenceStrongEnough(false,1.96137731));
    assert(broadRepairEvidenceStrongEnough(true,1.96137731));
    assert(broadRepairEvidenceStrongEnough(false,2.25));

    // x2r: keep the 2 ms evidence cell, but narrow an isolated impulse to a
    // sample-scale donor core. Ordinary programme curvature must not trigger
    // localization, and the S-fade must have gentle endpoints.
    assert(smoothRepairFade(0.0)==0.0);
    assert(std::abs(smoothRepairFade(0.5)-0.5)<1e-12);
    assert(smoothRepairFade(1.0)==1.0);
    assert(smoothRepairFade(0.01)<0.00002);
    assert(1.0-smoothRepairFade(0.99)<0.00002);

    std::vector<Wav> sources(3);
    for(auto& source:sources) {
        source.sampleRate=sr;
        source.channels=1;
        source.bits=24;
        source.samples.resize(9600);
    }
    for(int i=0;i<9600;++i) {
        const double t=(double)i/sr;
        const double programme=0.13*std::sin(2.0*3.141592653589793*220.0*t)+
            0.03*std::sin(2.0*3.141592653589793*370.0*t);
        sources[0].samples[(size_t)i]=(float)programme;
        sources[1].samples[(size_t)i]=(float)(0.8*programme+0.01);
        sources[2].samples[(size_t)i]=(float)(0.9*programme-0.005);
    }
    std::vector<double> delays(3,0.0);
    const long cellStart=4800;
    const long cellEnd=cellStart+sr*2/1000;
    const auto cleanCore=localizeImpulseCore(
        sources,0,1,cellStart,cellEnd,9600,delays,0.0,1.25,sr);
    assert(!cleanCore.localized);

    const long clickFrame=4896;
    sources[0].samples[(size_t)clickFrame]+=0.4f;
    const auto clickCore=localizeImpulseCore(
        sources,0,1,cellStart,cellEnd,9600,delays,0.0,1.25,sr);
    assert(clickCore.localized);
    assert(clickCore.start<=clickFrame && clickCore.end>clickFrame);
    assert(clickCore.end-clickCore.start<cellEnd-cellStart);
    assert(clickCore.end-clickCore.start<=sr*3/2000);

    // Merged/absorbed evidence can span several 2 ms cells. One dominant
    // click in such an event should still receive the narrow treatment.
    const long longEventStart=4300;
    const long longEventEnd=5800;
    const auto longSingleCore=localizeImpulseCore(
        sources,0,1,longEventStart,longEventEnd,9600,delays,0.0,1.25,sr);
    assert(longSingleCore.localized);
    assert(longSingleCore.start<=clickFrame && longSingleCore.end>clickFrame);
    assert(longSingleCore.end-longSingleCore.start<=sr*3/2000);

    // Two separated, substantial impulses are not safe to collapse into one
    // core. Preserve the established guarded full-event renderer instead.
    const long secondClickFrame=5600;
    sources[0].samples[(size_t)secondClickFrame]+=0.35f;
    const auto multiClickCore=localizeImpulseCore(
        sources,0,1,longEventStart,longEventEnd,9600,delays,0.0,1.25,sr);
    assert(!multiClickCore.localized);
    const auto longFallback=expandedRepairWindow(
        longEventStart,longEventEnd,9600,sr);
    assert(multiClickCore.start==longFallback.start);
    assert(multiClickCore.end==longFallback.end);

    std::cout << "PASS: adjacent B absorption, boundary guard and sub-ms core\n";
}
