#define main engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <cassert>
int main(){
 const int sr=96000;Wav original;original.sampleRate=sr;original.channels=1;original.bits=24;
 for(int f=0;f<sr;++f){double t=(double)f/sr;original.samples.push_back((float)(.10*std::sin(2*3.141592653589793*713*t)+.035*std::sin(2*3.141592653589793*1279*t)+.02*std::sin(2*3.141592653589793*311*t)));}
 auto master=original;long a=sr/2-288,b=sr/2+288;for(long f=a;f<b;++f)master.samples[f]*=.05f;
 std::vector<Wav>sources{master,original,original};std::vector<double>delay(3,0);std::vector<bool>yes{false,true,true};
 auto r=detectDropoutRepair(sources,0,sr/2-96,sr/2+96,delay,yes,sr);
 std::cout<<"synthetic accepted "<<r.accepted<<" first "<<r.first<<" last "<<r.last<<"\n";assert(r.accepted);assert(r.first<=a&&r.last>=b);
 for(long f=a;f<b;++f)assert(std::abs(r.samples[f-r.renderFirst]-original.samples[f])<1e-5);
 sources[0]=original;assert(!detectDropoutRepair(sources,0,sr/2-96,sr/2+96,delay,yes,sr).accepted);
 sources[0]=master;sources[2].samples.assign(sr,0);assert(!detectDropoutRepair(sources,0,sr/2-96,sr/2+96,delay,yes,sr).accepted);
 sources[2]=original;yes[2]=false;assert(!detectDropoutRepair(sources,0,sr/2-96,sr/2+96,delay,yes,sr).accepted);
 yes[2]=true;sources={original,original,original};for(long f=a;f<b;++f)for(auto& w:sources)w.samples[f]*=.05f;
 assert(!detectDropoutRepair(sources,0,sr/2-96,sr/2+96,delay,yes,sr).accepted);
 sources={master,original,original};for(auto& x:sources[2].samples)x=-x;
 assert(!detectDropoutRepair(sources,0,sr/2-96,sr/2+96,delay,yes,sr).accepted);
 sources={master,original,original};for(auto& x:sources[1].samples)x*=.75f;for(auto& x:sources[2].samples)x*=1.2f;
 assert(detectDropoutRepair(sources,0,sr/2-96,sr/2+96,delay,yes,sr).accepted);
 std::cout<<"PASS dropout, healthy Master, missing control, one witness, common silence, opposite control, unequal levels\n";
}
