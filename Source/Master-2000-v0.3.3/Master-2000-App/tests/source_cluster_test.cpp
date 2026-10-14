#define main engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <algorithm>
#include <cassert>
#include <iostream>

static MasterSyncPreflight makePreflight(int count) {
    MasterSyncPreflight sync;
    sync.donor.assign(count,std::vector<DonorSyncPreflight>(count));
    sync.viableDonors.assign(count,0);
    sync.secondSupport.assign(count,0.0);
    sync.meanTopTwoSupport.assign(count,0.0);
    return sync;
}

static void connect(MasterSyncPreflight& sync,int a,int b,double support) {
    for(auto [from,to]:{std::pair{a,b},std::pair{b,a}}) {
        auto& relation=sync.donor[from][to];
        relation.viable=true;
        relation.support=support;
        relation.medianRobust=0.90+0.01*support;
        ++sync.viableDonors[from];
    }
}

int main() {
    // Once every candidate has the required two donors and identical graph
    // connectivity, source quality outranks a tiny support-score advantage.
    auto equalSync=makePreflight(3);
    connect(equalSync,0,1,0.90);connect(equalSync,0,2,0.90);connect(equalSync,1,2,0.99);
    equalSync.meanTopTwoSupport={0.90,0.945,0.945};
    equalSync.secondSupport={0.90,0.90,0.90};
    auto qualityChoice=chooseClusterAwareMaster(
        {10.0,1.0,0.0},{0.0,0.0,0.0},equalSync);
    assert(qualityChoice.source==0);

    auto sync=makePreflight(6);
    for(int a=0;a<5;++a)
        for(int b=a+1;b<5;++b)
            connect(sync,a,b,0.80+0.01*(a+b));
    // Source 6 is unrelated and therefore outside the compatible cluster.
    for(int i=0;i<5;++i) {
        sync.secondSupport[i]=0.82;
        sync.meanTopTwoSupport[i]=0.84;
    }
    std::vector<double> quality={1.0,1.1,1.2,1.3,1.4,99.0};
    std::vector<double> penalty(6,0.0);
    auto choice=chooseClusterAwareMaster(quality,penalty,sync);
    assert(choice.source==4);
    assert(choice.componentSize[4]==5);
    assert(choice.componentSize[5]==1);
    auto roles=classifySourceRoles(choice.source,quality,sync);
    assert(roles[4]==SourceRole::Master);
    assert(roles[5]==SourceRole::Rejected);
    assert(std::count(roles.begin(),roles.end(),SourceRole::Donor)==4);

    // When all six copies are compatible, only four become full donors; the
    // remaining viable copy is deliberately held as sparse reserve/control.
    sync=makePreflight(6);
    for(int a=0;a<6;++a)
        for(int b=a+1;b<6;++b)
            connect(sync,a,b,0.90-0.005*(a+b));
    for(int i=0;i<6;++i) {
        sync.secondSupport[i]=0.86;
        sync.meanTopTwoSupport[i]=0.88;
    }
    roles=classifySourceRoles(0,quality,sync);
    assert(roles[0]==SourceRole::Master);
    assert(std::count(roles.begin(),roles.end(),SourceRole::Donor)==4);
    assert(std::count(roles.begin(),roles.end(),SourceRole::Reserve)==1);
    assert(std::count(roles.begin(),roles.end(),SourceRole::Rejected)==0);
    std::cout << "PASS: largest compatible cluster and 4+1 donor/reserve roles\n";
}
