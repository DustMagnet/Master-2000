#define main master2000_engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <cassert>
int main(){
 Wav w;w.sampleRate=96000;w.channels=2;w.bits=24;
 for(int i=0;i<9600;++i){w.samples.push_back(float(.2*sin(i*.07)));w.samples.push_back(float(.1*cos(i*.11)));}
 auto original=w.samples;auto s=prepareChannels(w,true);assert(!s.applied && w.channels==2 && w.samples==original);
 for(size_t i=0;i<w.samples.size();i+=2)w.samples[i+1]=w.samples[i];
 prepareChannels(w,false);assert(w.channels==1 && w.samples.size()==original.size()/2);
 bool rejected=false;try{prepareChannels(w,true);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
 std::cout<<"Channel mode checks passed\n";
}
