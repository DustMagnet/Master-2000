#include "../src/AudioCore.h"
#include <cassert>
#include <iostream>
int main() {
 Wav w; w.sampleRate=48000; w.channels=2; w.bits=24;
 for(int i=0;i<4800;i++) { float x=(float)(.2*std::sin(2*3.141592653589793*1000*i/48000)); w.samples.push_back(x); w.samples.push_back(-x*.5f); }
 auto st=fuseStereoGrooveToMono(w,false);
 assert(w.channels==1 && w.frames()==4800 && st.applied && st.changedFrames==0);
 for(int i=0;i<4800;i++) assert(std::abs(w.samples[i]-.2*std::sin(2*3.141592653589793*1000*i/48000)/std::sqrt(2.0))<1e-6);
 assert(resamplePlaybackSpeed(w.samples,1,1)==w.samples);
 auto faster=resamplePlaybackSpeed(w.samples,1,78.0/45);
 assert(faster.size()==(size_t)std::floor(4800/(78.0/45)));
 int crossings=0; for(size_t i=101;i+100<faster.size();i++) if(faster[i-1]<0 && faster[i]>=0) crossings++;
 double hz=crossings*48000.0/(faster.size()-201); assert(std::abs(hz-1000*78/45)<30);
 auto slow=resamplePlaybackSpeed(w.samples,1,.5); assert(slow.size()==9600);
 writeWavPcm("/tmp/mono78-test.wav",48000,1,24,faster);
 auto read=readWav("/tmp/mono78-test.wav"); assert(read.channels==1&&read.bits==24&&read.frames()==faster.size());
 for(size_t i=0;i<faster.size();i++) assert(std::abs(read.samples[i]-faster[i])<1e-6);
 std::filesystem::remove("/tmp/mono78-test.wav");
 std::cout<<"PASS: mono polarity/gain, no repair, unity, speed length/pitch, PCM24 roundtrip\n";
}
