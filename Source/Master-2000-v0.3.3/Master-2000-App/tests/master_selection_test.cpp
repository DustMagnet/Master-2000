#define main engine_entry
#include "../Vinyl2000.Engine/Vinyl2000Engine.cpp"
#undef main
#include <cassert>
static Wav signal(){
 Wav w;w.sampleRate=16000;w.bits=24;w.channels=1;
 for(int i=0;i<16000*8;++i){double t=(double)i/16000;
 double env=0.5+0.2*std::sin(2.7*t)+0.2*std::sin(5.3*t);
 double x=0.12*std::sin(6.283185*(181*t+11*t*t))+0.09*std::sin(6.283185*(283*t+3*t*t));
 x+=0.06*std::sin(6.283185*(1103*t+17*t*t))+0.04*std::sin(6.283185*(1537*t+13*t*t));
 w.samples.push_back((float)(env*x));}return w;
}
int main(){
 auto a=signal();auto b=a,c=a;
 for(auto&v:b.samples)v*=0.2f;
 for(auto&v:c.samples)v*=2.0f;
 auto r=screenMasterCandidates({a,b,c});
 for(double p:r.penalty)assert(p==0);
 double qa=masterQualityScore(a),qb=masterQualityScore(b),qc=masterQualityScore(c);
 assert(std::abs(qa-qb)<0.01 && std::abs(qa-qc)<0.01);
 auto aa=assessSource(a),ab=assessSource(b);
 assert(std::abs(aa.qualityScore-ab.qualityScore)<0.01);
 assert(ab.programmeLevelDbfs<aa.programmeLevelDbfs-13.0);
 assert(aa.activeWindows>0 && aa.cleanWindowsPercent>=0.0 && aa.cleanWindowsPercent<=100.0);
 auto clipped=a;clipped.samples[100]=1.0f;
 assert(assessSource(clipped).clippedSamples==1);
 std::cout<<"gain only: no penalty, invariant quality\n";
 b=a;
 for(size_t i=0;i<a.frames();++i){double s=0;int n=0;for(long j=std::max<long>(0,(long)i-10);j<=std::min<long>((long)a.frames()-1,(long)i+10);++j){s+=a.samples[j];++n;}b.samples[i]=(float)(s/n);}

 for(int order=0;order<3;++order){std::vector<Wav> ws={a,c,a};ws[order]=b;
 r=screenMasterCandidates(ws);
 std::cout<<"muffled position "<<order<<": ";
 for(int i=0;i<3;++i)
     std::cout<<r.verified[i]<<"/"<<r.missing[i]<<" penalty="<<r.penalty[i]<<" ";
 std::cout<<"\n";
 assert(r.penalty[order]>0);
 std::vector<double> scores;for(int i=0;i<3;++i)scores.push_back(masterQualityScore(ws[i])-r.penalty[i]);
 assert(std::distance(scores.begin(),std::max_element(scores.begin(),scores.end()))!=order);
 }
 auto silence=a;std::fill(silence.samples.begin(),silence.samples.end(),0);
 r=screenMasterCandidates({silence,silence,silence});for(double p:r.penalty)assert(p==0);
 assert(masterQualityScore(silence)<-1e20);

 // x2f whole-record Master preflight: three compatible copies must give each
 // candidate two viable donors, while one silent/broken copy leaves no
 // candidate with the two independent witnesses required by Master-2000.
 auto syncOk=screenMasterSyncFeasibility({a,a,a});
 for(int n:syncOk.viableDonors)assert(n==2);
 auto syncBroken=screenMasterSyncFeasibility({a,a,silence});
 for(int n:syncBroken.viableDonors)assert(n<2);

 // x2g regression: a coherent 3.2% transfer-speed difference, including one
 // false periodic basin, must produce a resistant whole-record search model.
 std::vector<AffineProbe> sparse;
 for(int i=0;i<9;++i)
  sparse.push_back({i*16000L,700.0+0.032*i*16000.0,.95,i});
 sparse[4].donorDelay+=8000.0; // one false periodic basin
 auto resistant=fitAffineDelayModel(sparse,16000,9);
 assert(resistant.valid && resistant.inliers==8);
 assert(std::abs(resistant.slope-0.032)<1e-6);

 // Quality ranks only the candidates that can actually be supported by two
 // donors. A higher standalone quality score cannot win without that support.
 MasterSyncPreflight fabricated;
 fabricated.viableDonors={1,2,2};
 std::vector<double> quality={14.0,10.0,11.0};
 std::vector<double> penalty={0.0,0.0,0.0};
 assert(chooseSyncAwareMaster(quality,penalty,fabricated)==2);
 penalty[2]=20.0;
 assert(chooseSyncAwareMaster(quality,penalty,fabricated)==1);
 fabricated.viableDonors={1,1,1};
 assert(chooseSyncAwareMaster(quality,penalty,fabricated)==-1);
 std::cout<<"sync-aware Master preflight and affine speed model passed\n";

 // Broadband unrelated detail cannot supply two-source consensus.
 b=a;c=a;unsigned rng=3;
 for(size_t i=0;i<a.frames();++i){rng=1664525*rng+1013904223;b.samples[i]+=0.15f*((rng>>8)/16777216.0f-0.5f);rng=1664525*rng+1013904223;c.samples[i]+=0.15f*((rng>>8)/16777216.0f-0.5f);}
 r=screenMasterCandidates({a,b,c});assert(r.penalty[0]==0);
 std::cout<<"silence and unrelated noise guards passed\n";
}
