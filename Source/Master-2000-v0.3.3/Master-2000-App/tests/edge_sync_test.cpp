#define main master2000_engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <cassert>
int main(){
 const int sr=12000;std::vector<Wav> s(3);
 unsigned rng=123;
 for(auto& w:s){w.sampleRate=sr;w.channels=1;w.bits=24;w.samples.resize(sr);}
 for(int i=0;i<sr;++i){rng=rng*1664525+1013904223;float x=((rng>>8)%10000)/20000.f-.25f;for(auto& w:s)w.samples[i]=x;}
 auto initial=[](){std::vector<std::vector<TimeMapAnchor>> m(3);for(int k=1;k<3;++k)for(long f:{3000L,9000L}){TimeMapAnchor a;a.masterFrame=f;a.donorDelay=0;a.corr=1;a.verified=a.repairSafe=true;m[k].push_back(a);}return m;};
 auto m=initial();assert(extendEdgeTimeMaps(s,m,{true,true,true},sr)>0);
 assert(mappedConfidenceAt(m[1],0,sr)>.97);assert(mappedConfidenceAt(m[2],sr-1,sr)>.97);
 m=initial();assert(extendEdgeTimeMaps(s,m,{true,true,false},sr)==0);
 // A historical extrapolated zero anchor must not authorize edge repairs.
 m=initial();for(int k=1;k<3;++k){TimeMapAnchor a;a.masterFrame=0;a.corr=.70;a.verified=a.repairSafe=true;m[k].insert(m[k].begin(),a);}
 for(auto& w:s)for(int i=0;i<2000;++i)w.samples[i]=0;
 extendEdgeTimeMaps(s,m,{true,true,true},sr);assert(mappedConfidenceAt(m[1],0,sr)==0);
 for(auto& w:s)for(int i=0;i<2000;++i)w.samples[i]=0;
 m=initial();extendEdgeTimeMaps(s,m,{true,true,true},sr);assert(mappedConfidenceAt(m[1],0,sr)==0);
 for(int i=0;i<sr;++i)s[2].samples[i]=-s[1].samples[i];
 m=initial();assert(extendEdgeTimeMaps(s,m,{true,true,true},sr)==0);
 for(auto& w:s)for(int i=0;i<sr;++i)w.samples[i]=.2f*std::sin(i*6.283185307179586/8);
 m=initial();assert(extendEdgeTimeMaps(s,m,{true,true,true},sr)==0);
 std::cout<<"PASS: measured bidirectional edges, silence stop, one witness and disagreement rejection\n";
}
