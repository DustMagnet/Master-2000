#define main engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <cassert>
int main(){
 const int sr=96000;const long first=4800,last=4992,click=4896;
 std::vector<Wav>s(5);for(auto&w:s){w.sampleRate=sr;w.channels=1;w.bits=24;w.samples.resize(9600);}
 for(long i=0;i<9600;++i){double t=(double)i/sr;float x=(float)(.13*sin(2*3.141592653589793*220*t)+.03*sin(2*3.141592653589793*370*t));s[0].samples[i]=x;s[1].samples[i]=x*.8f+.01f;s[2].samples[i]=x*4.f-.02f;s[3].samples[i]=.1f*sin(2*3.141592653589793*713*t);s[4].samples[i]=x*.9f;}
 std::vector<double>delay(5,0);std::vector<bool>eligible={false,true,true,false,true};
 assert(localImpulseTrials(s,0,first,last,delay,sr,eligible).empty());
 s[0].samples[click]+=.4f;s[4].samples[click-8]+=.4f;
 auto match=localTrendMatch(s,0,2,first,last,delay,sr);assert(match.usable && std::abs(match.gain-.25)<.001);
 auto trials=localImpulseTrials(s,0,first,last,delay,sr,eligible,nullptr,1);assert(!trials.empty());
 assert(trials[0].evidence.donorSource==1 && trials[0].evidence.controlSource==2);
 auto insufficient=eligible;insufficient[2]=false;insufficient[4]=false;
 assert(localImpulseTrials(s,0,first,last,delay,sr,insufficient).empty());
 // A shared musical transient is not removable noise, even with level changes.
 s[1].samples[click]+=.32f;s[2].samples[click]+=1.6f;
 assert(localImpulseTrials(s,0,first,last,delay,sr,eligible,nullptr,1).empty());
 std::cout<<"local review: independent witnesses, unequal levels, invalid copy, shared transient and insufficient support passed\n";
}
