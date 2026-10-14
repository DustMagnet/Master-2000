#define main master2000_engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    constexpr int sr=1000;
    std::vector<float> baseline(2000,0.0f);
    for(size_t i=0;i<baseline.size();++i)
        baseline[i]=(float)(0.10*std::sin(2.0*3.14159265358979323846*5.0*i/sr));

    auto output=baseline;
    auto clean=analyzeOutputQa(output,baseline,1,sr,1.0,{});
    assert(clean.passed);
    assert(clean.nonFiniteSamples==0);
    assert(clean.seamFlags==0);

    std::vector<RepairRecord> repair{{0.995,1.010,2,3.0,0.0}};
    output=baseline;
    output[995]=0.75f;
    auto seam=analyzeOutputQa(output,baseline,1,sr,1.0,repair);
    assert(!seam.passed);
    assert(seam.seamFlags>0);

    output=baseline;
    output[50]=std::numeric_limits<float>::quiet_NaN();
    auto invalid=analyzeOutputQa(output,baseline,1,sr,1.0,{});
    assert(!invalid.passed);
    assert(invalid.nonFiniteSamples==1);

    std::cout << "PASS: final output clipping/DC/non-finite/seam QA\n";
}
