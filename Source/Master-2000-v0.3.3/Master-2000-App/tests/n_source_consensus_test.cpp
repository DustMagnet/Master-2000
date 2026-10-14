#define main engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <algorithm>
#include <cassert>
#include <iostream>

static std::vector<Wav> makeSources(int count) {
    const int sr=96000;
    std::vector<Wav> sources((size_t)count);
    for(int s=0;s<count;++s) {
        auto& w=sources[(size_t)s];
        w.sampleRate=sr;
        w.channels=1;
        w.bits=24;
        w.samples.resize(9600);
        const double gain=0.72+0.11*s;
        const double dc=0.002*(s-2);
        for(int i=0;i<9600;++i) {
            const double t=(double)i/sr;
            const double x=.13*std::sin(2*3.141592653589793*220*t)+
                .03*std::sin(2*3.141592653589793*370*t);
            w.samples[(size_t)i]=(float)(gain*x+dc);
        }
    }
    return sources;
}

static bool contains(const std::vector<int>& values,int source) {
    return std::find(values.begin(),values.end(),source)!=values.end();
}

int main() {
    const int sr=96000;
    const long seg=4800,end=4992,click=4896;

    // Four good copies: all three donors support removal of the Master's click.
    auto four=makeSources(4);
    four[0].samples[(size_t)click]+=.40f;
    std::vector<double> delay4(4,0.0);
    std::vector<bool> eligible4(4,true);
    auto e=detectImpulseConsensus(four,0,seg,end,delay4,sr,&eligible4);
    assert(e.confirmed);
    assert(e.supportingWitnesses==3 && e.opposingWitnesses==0);

    // A globally poor source may not win a near-tied local fit by chance.
    // It may still win when the local fragment is decisively better.
    std::vector<LocalTrendMatch> donorTrends(4);
    for(int s=1;s<4;++s) {
        donorTrends[s].usable=true;
        donorTrends[s].corr=0.90;
    }
    donorTrends[1].fit=0.20;
    donorTrends[2].fit=0.22;
    donorTrends[3].fit=0.19;
    const std::vector<int> donorCandidates{1,2,3};
    const std::vector<double> globalQuality{0.0,9.0,8.0,1.0};
    bool guardChanged=false;
    assert(selectQualityAwareDonor(donorCandidates,donorTrends,
        &globalQuality,&guardChanged)==1);
    assert(guardChanged);
    donorTrends[3].fit=0.10;
    assert(selectQualityAwareDonor(donorCandidates,donorTrends,
        &globalQuality,&guardChanged)==3);
    assert(!guardChanged);

    // v0.2.31 real-run regression at 8.578 s: a suspicious Master impulse with
    // only one independent supporter is retained, but it must be visible as
    // Class B/manual review rather than silently reported as unchanged C.
    ImpulseConsensusEvidence oneSupport;
    oneSupport.supportingWitnesses=1;
    assert(unconfirmedImpulseReason(oneSupport)==
        "B_INSUFFICIENT_SOURCE_CONSENSUS_KEEP_MASTER");
    oneSupport.supportingWitnesses=2;
    oneSupport.opposingWitnesses=1;
    assert(unconfirmedImpulseReason(oneSupport)==
        "B_SPLIT_SOURCE_VOTE_KEEP_MASTER");

    // A quiet transfer carrying the same click must still oppose repair after
    // local level normalization; raw absolute amplitude would misclassify it
    // as a clean witness.
    four=makeSources(4);
    four[0].samples[(size_t)click]+=.40f;
    four[3].samples[(size_t)click]+=.40f;
    for(float& sample:four[3].samples) sample*=.25f;
    e=detectImpulseConsensus(four,0,seg,end,delay4,sr,&eligible4);
    assert(!e.confirmed);
    assert(e.supportingWitnesses==2 && e.opposingWitnesses==1);

    // The fourth copy has an unrelated local click. It abstains; the two clean
    // donors still form a strict majority with Master as the disputed source.
    four=makeSources(4);
    four[0].samples[(size_t)click]+=.40f;
    four[3].samples[(size_t)(click-26)]+=.55f;
    e=detectImpulseConsensus(four,0,seg,end,delay4,sr,&eligible4);
    assert(e.confirmed);
    assert(e.supportingWitnesses==2 && e.opposingWitnesses==0);
    assert(contains(e.abstainingSources,3));

    // An aligned same-sign event is not an abstention. Two clean donors versus
    // Master plus one agreeing witness is a 2-2 split, so Master is retained.
    four=makeSources(4);
    four[0].samples[(size_t)click]+=.40f;
    four[3].samples[(size_t)click]+=.40f;
    e=detectImpulseConsensus(four,0,seg,end,delay4,sr,&eligible4);
    assert(!e.confirmed);
    assert(e.supportingWitnesses==2 && e.opposingWitnesses==1);

    // Five copies: three clean donors beat Master plus one coincident witness.
    auto five=makeSources(5);
    five[0].samples[(size_t)click]+=.40f;
    five[4].samples[(size_t)click]+=.40f;
    std::vector<double> delay5(5,0.0);
    std::vector<bool> eligible5(5,true);
    auto e5=detectImpulseConsensus(five,0,seg,end,delay5,sr,&eligible5);
    assert(e5.confirmed);
    assert(e5.supportingWitnesses==3 && e5.opposingWitnesses==1);

    // A source rejected by synchronization is explicitly absent from voting.
    four=makeSources(4);
    four[0].samples[(size_t)click]+=.40f;
    eligible4={true,true,true,false};
    e=detectImpulseConsensus(four,0,seg,end,delay4,sr,&eligible4);
    assert(e.confirmed && contains(e.abstainingSources,3));

    // Six copies: the active core can use four donors while the sixth source
    // remains available as reserve. A coincident defect on one copy cannot
    // overturn four independent clean witnesses.
    auto six=makeSources(6);
    six[0].samples[(size_t)click]+=.40f;
    six[5].samples[(size_t)click]+=.40f;
    std::vector<double> delay6(6,0.0);
    std::vector<bool> eligible6(6,true);
    auto e6=detectImpulseConsensus(six,0,seg,end,delay6,sr,&eligible6);
    assert(e6.confirmed);
    assert(e6.supportingWitnesses==4 && e6.opposingWitnesses==1);

    std::cout << "PASS: level-matched adaptive 3-6 source consensus, split vote and abstention\n";
}
