#define main engine_main
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <cassert>
int main(){
 std::vector<float>input(24000);for(size_t i=0;i<input.size();++i)input[i]=.2f*std::sin(i*.017);
 workProgressEnabled=false;auto reference=resamplePlaybackSpeed(input,1,1.7);
 std::ostringstream log;auto* prior=std::cerr.rdbuf(log.rdbuf());
 workProgressEnabled=true;
 beginWorkPhase("RENDER_REPAIRS",100);reportWorkProgress(40,100,true);
 beginWorkPhase("LOCAL_REVIEW",100);reportWorkProgress(60,100,true);
 beginWorkPhase("SPEED_RESULT");auto presented=resamplePlaybackSpeed(input,1,1.7);
 std::cerr.rdbuf(prior);workProgressEnabled=false;
 assert(reference==presented);
 auto text=log.str();
 assert(text.find("WORK_PHASE RENDER_REPAIRS\nWORK_PROGRESS RENDER_REPAIRS 0 100")!=std::string::npos);
 assert(text.find("WORK_PHASE LOCAL_REVIEW\nWORK_PROGRESS LOCAL_REVIEW 0 100")!=std::string::npos);
 assert(text.find("WORK_PROGRESS LOCAL_REVIEW 60 100")!=std::string::npos);
 assert(text.find("WORK_PHASE SPEED_RESULT")!=std::string::npos);
 std::cout<<"PASS: phase resets, measurable counts and identical audio with telemetry enabled\n";
}
