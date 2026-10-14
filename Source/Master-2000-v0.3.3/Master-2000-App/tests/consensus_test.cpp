#define main master2000_engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <cassert>
int main(){
 std::vector<std::pair<long,TimingMatch>> p;
 for(int i=0;i<10;++i){TimingMatch m;m.delay=100+i*96;m.q=.9;p.push_back({i*96000L,m});}
 auto f=fitTimingConsensus(p,96000);assert(f.accepted && f.inliers.size()==10);
 p[5].second.delay+=1700;f=fitTimingConsensus(p,96000);assert(f.accepted && f.inliers.size()==9 && std::abs(f.intercept-100)<1e-6);
 p.resize(4);assert(!fitTimingConsensus(p,96000).accepted);
 p.clear();double times[]={.2,1.7,5.7,8.7,10.7,11.2,12.2,13.7};double delays[]={-1872,-1296,-576,240,912,1056,2784,1488};
 for(int i=0;i<8;++i){TimingMatch m;m.delay=delays[i];m.q=.85;p.push_back({long(times[i]*96000),m});}
 f=fitTimingConsensus(p,96000);assert(f.accepted && f.inliers.size()==7);
 for(int i=0;i<8;++i)p[i].second.delay=(i%2)*5000;
 assert(!fitTimingConsensus(p,96000).accepted);
 std::cout<<"Consensus checks passed\n";
}
