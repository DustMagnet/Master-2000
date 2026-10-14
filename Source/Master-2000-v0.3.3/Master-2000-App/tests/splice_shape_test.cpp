#define main engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <cassert>
int main(){
 const int sr=96000;std::vector<Wav>s(3);
 for(auto&w:s){w.sampleRate=sr;w.channels=1;w.bits=24;w.samples.resize(12000);}
 for(int i=0;i<12000;++i){float x=std::clamp(.00015f*(i-6000),-.7f,.7f);s[0].samples[i]=x;s[1].samples[i]=x*.8f+.01f;s[2].samples[i]=x*1.2f-.02f;}
 std::vector<double>d(3,0);std::vector<float>healthy=s[0].samples;
 assert(spliceShapeSupported(s,0,1,2,d,1.25,-.0125,healthy,5800,6100,sr));
 auto bad=healthy;
 for(int i=5920;i<5960;++i)bad[i]-=(float)(.02*std::sin(3.141592653589793*(i-5920)/40));
 assert(!spliceShapeSupported(s,0,1,2,d,1.25,-.0125,bad,5800,6100,sr));
 // Master-only detection is retained even with only one admitted donor.
 s[0].samples[6000]+=.2f;std::vector<bool>one={false,true,false};
 auto e=detectImpulseConsensus(s,0,5952,6144,d,sr,&one);
 assert(e.available && !e.confirmed && e.eligibleWitnesses==1 && e.zBySource[0]>5);
 // Local readmission requires good surrounding matches on both sides.
 assert(locallyVerifiedWitness(s,0,1,5952,6144,d,sr));
 for(int i=6200;i<7000;++i)s[2].samples[i]=.1f*std::sin(i*.13);
 assert(!locallyVerifiedWitness(s,0,2,5952,6144,d,sr));
 std::cout<<"PASS: smooth seam reversal, insufficient witnesses and two-sided local admission\n";
}
