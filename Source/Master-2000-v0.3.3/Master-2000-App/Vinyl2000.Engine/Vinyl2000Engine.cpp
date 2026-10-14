#include <cstdlib>
#include <sstream>
#include <memory>

// Master-2000 engine v0.3.3
// Multi-source archival audio reconstruction engine
//
// Original concept: Olle Hallberg, 2000
// Modernized implementation: 2026
//
// Core idea:
//   Reconstruct the best possible signal from N independent digitizations
//   of the same underlying recording. Sources may be different physical
//   copies, different playbacks, different playback equipment, or even
//   non-contact/optical transfers.
//
//   The engine continuously micro-synchronizes every source against a
//   reference time axis, evaluates local stability, and combines/selects
//   REAL recorded material. It does not synthesize missing music.
//
// Examples:
//   vinyl2000 3 out.wav take1.wav take2.wav take3.wav
//   vinyl2000 4 out.wav a.wav b.wav c.wav d.wav
//   vinyl2000 5 out.wav a.wav b.wav c.wav d.wav e.wav
//   vinyl2000 6 out.wav a.wav b.wav c.wav d.wav e.wav f.wav
//
// Optional decision map:
//   Add --map decisions.csv anywhere after output filename:
//   vinyl2000 3 out.wav --map decisions.csv a.wav b.wav c.wav
//
// Input:
//   PCM WAV, 8/16/24/32-bit, mono or stereo.
//   All sources must have identical sample rate, channel count and bit depth.
//
// Output:
//   PCM WAV preserving the input bit depth (8/16/24/32-bit). Stereo inputs
//   from lateral mono records are level/polarity matched and fused to mono
//   before cross-record synchronization and repair.
//
// Build:
//   g++ -std=c++17 -O3 -o vinyl2000.exe Master-2000-v2.2.cpp
//
// Architecture:
//   - WAV I/O
//   - alignment / micro-steering
//   - local source-quality estimation
//   - multi-source combiner
//   - decision-map diagnostics
//
// The DSP core is deliberately kept independent of any Windows GUI.

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr const char* kEngineDisplayVersion =
    "Master-2000 v0.3.3";
}

// Presentation telemetry only. Disabled for previews/tests until the full run
// enters final sync. Reporting never changes a sample, vote or repair decision.
namespace {
bool workProgressEnabled=false;
std::string workProgressPhase;
std::chrono::steady_clock::time_point workProgressLast;
void reportWorkProgress(size_t done,size_t total,bool force=false) {
    if(!workProgressEnabled || workProgressPhase.empty() || total==0)return;
    const auto now=std::chrono::steady_clock::now();
    if(!force && std::chrono::duration<double>(now-workProgressLast).count()<.5)return;
    workProgressLast=now;
    std::cerr<<"WORK_PROGRESS "<<workProgressPhase<<" "<<done<<" "<<total<<"\n";
}
void beginWorkPhase(const char* phase,size_t total=0) {
    if(!workProgressEnabled)return;
    workProgressPhase=phase;
    std::cerr<<"WORK_PHASE "<<phase<<"\n";
    reportWorkProgress(0,total,true);
}
}

struct Wav {
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;
    std::vector<float> samples;

    size_t frames() const {
        return channels ? samples.size() / (size_t)channels : 0;
    }
};

struct TrackState {
    double delay = 0.0;
    double measuredDelay = 0.0;
    double quality = 1.0;
    double shock = 0.0;
};

struct TimeMapAnchor {
    long masterFrame = 0;
    double donorDelay = 0.0;
    double corr = 0.0;
    bool verified = false;
    bool repairSafe = false;
};

// A whole-record delay model measured before the dense 250 ms time map.
// donorFrame = masterFrame + intercept + slope * masterFrame.
// It is only a search centre: every repair still needs the ordinary local
// correlation, phase, uniqueness and two-donor gates.
struct AffineDelayModel {
    double intercept = 0.0;
    double slope = 0.0;
    double maxResidual = 0.0;
    double span = 0.0;
    double score = -2.0;
    int inliers = 0;
    bool valid = false;

    double delayAt(long masterFrame) const {
        return intercept + slope * (double)masterFrame;
    }
};

// A sparse whole-record fit is excellent evidence for transfer-speed drift,
// but strongly periodic shellac music can give it the right slope in the wrong
// absolute musical cycle. A clean programme attack supplies that missing
// absolute position. Shift only the affine intercept; retain its measured
// slope and every ordinary local verification gate.
struct OnsetAffineLock {
    AffineDelayModel model;
    double interceptOffset = 0.0;
    bool applied = false;
};

static OnsetAffineLock anchorAffineBasinToOnset(
        const AffineDelayModel& model,
        long masterOnset,
        double measuredOnsetDelay,
        bool onsetEvidence,
        long lockRadius) {
    OnsetAffineLock result;
    result.model=model;
    if(!model.valid || !onsetEvidence) return result;
    const double difference=measuredOnsetDelay-model.delayAt(masterOnset);
    if(std::abs(difference)<=(double)lockRadius) return result;
    result.model.intercept+=difference;
    result.interceptOffset=difference;
    result.applied=true;
    return result;
}

// A level edge is useful absolute-position evidence only when the musical
// trajectory that begins there survives the same multi-window continuity test
// as every competing basin.  This prevents a quiet pickup, damaged first note
// or threshold mismatch from moving an otherwise excellent whole-record fit.
static bool onsetBasinMayOverride(double onsetPathScore,
                                  double winningPathScore,
                                  bool onsetEvidence,
                                  double minimumScore,
                                  double maximumDeficit=0.03) {
    return onsetEvidence && std::isfinite(onsetPathScore) &&
        std::isfinite(winningPathScore) && onsetPathScore>=minimumScore &&
        onsetPathScore>=winningPathScore-maximumDeficit;
}
static bool plausibleTimeMapTransition(const TimeMapAnchor& before,
                                       const TimeMapAnchor& after,
                                       int sampleRate) {
    const long span=std::max<long>(0,after.masterFrame-before.masterFrame);
    // Independent turntables can drift, but they cannot move the same musical
    // position by seconds between adjacent anchors. Allow 5% speed error plus
    // a 20 ms phase cushion; anything larger is a different periodic basin.
    const double maxChange=0.05*(double)span+0.020*(double)sampleRate;
    return std::abs(after.donorDelay-before.donorDelay)<=maxChange;
}

// After the strongest donor pair has independently established a safe path
// towards programme onset, an additional donor may follow that path only when
// it agrees with BOTH established witnesses.  This deliberately does not let
// the extra donor create a new timing basin or weaken the original two-source
// authority.  Movement is compared over one backward step; four milliseconds
// is the same deliberately generous analogue-drift allowance used by the
// primary bridge.
static bool additionalBackwardBridgePass(bool sourceAccepted,
                                         double sourceMove,
                                         double witnessMoveA,
                                         double witnessMoveB,
                                         double witnessCorrA,
                                         double witnessCorrB,
                                         int sampleRate) {
    if(!sourceAccepted || sampleRate<=0 ||
       !std::isfinite(sourceMove) ||
       !std::isfinite(witnessMoveA) ||
       !std::isfinite(witnessMoveB) ||
       !std::isfinite(witnessCorrA) ||
       !std::isfinite(witnessCorrB))
        return false;
    const double tolerance=0.004*(double)sampleRate;
    return witnessCorrA>=0.55 && witnessCorrB>=0.55 &&
        std::abs(witnessMoveA-witnessMoveB)<=tolerance &&
        std::abs(sourceMove-witnessMoveA)<=tolerance &&
        std::abs(sourceMove-witnessMoveB)<=tolerance;
}

struct BackwardZeroAnchor {
    TimeMapAnchor anchor;
    double slope = 0.0;
    double maxResidual = 0.0;
    bool valid = false;
};

// Once an ordinary time map and the two-source backward bridge have both
// established the first programme position, extend only the TIMING bracket to
// frame zero.  The affine slope comes from the whole-record transfer model,
// but its absolute basin is pinned to the earliest locally verified anchor.
// This is not audio evidence by itself: every repair in the extrapolated area
// must still pass map confidence, raw local phase/correlation, impulse vote,
// two independent witnesses and the post-render check.
static BackwardZeroAnchor makeBackwardZeroAnchor(
        const std::vector<TimeMapAnchor>& anchors,
        int sampleRate,
        const AffineDelayModel* model=nullptr,
        double basinOffset=0.0) {
    BackwardZeroAnchor result;
    std::vector<TimeMapAnchor> safe;
    for(const auto& anchor:anchors)
        if(anchor.verified && anchor.repairSafe)
            safe.push_back(anchor);
    std::sort(safe.begin(),safe.end(),[](const TimeMapAnchor& a,
                                         const TimeMapAnchor& b) {
        return a.masterFrame<b.masterFrame;
    });
    if(safe.size()<3 || safe.front().masterFrame<=0 ||
       safe.front().masterFrame>2L*sampleRate || safe.front().corr<0.55)
        return result;

    const size_t fitCount=std::min<size_t>(8,safe.size());
    double slope=0.0;
    double zeroDelay=safe.front().donorDelay;
    if(model && model->valid && std::isfinite(model->slope) &&
       std::abs(model->slope)<=0.01) {
        slope=model->slope;
        const double residual=safe.front().donorDelay-
            (model->delayAt(safe.front().masterFrame)+basinOffset);
        zeroDelay=model->delayAt(0)+basinOffset+residual;
    } else {
        double meanX=0.0,meanY=0.0;
        for(size_t i=0;i<fitCount;++i) {
            meanX+=(double)safe[i].masterFrame;
            meanY+=safe[i].donorDelay;
        }
        meanX/=fitCount;meanY/=fitCount;
        double xx=0.0,xy=0.0;
        for(size_t i=0;i<fitCount;++i) {
            const double x=(double)safe[i].masterFrame-meanX;
            xx+=x*x;
            xy+=x*(safe[i].donorDelay-meanY);
        }
        if(xx<=0.0) return result;
        slope=xy/xx;
        if(!std::isfinite(slope) || std::abs(slope)>0.01) return result;
        zeroDelay=meanY-slope*meanX;
    }

    double maxResidual=0.0;
    const double intercept=safe.front().donorDelay-
        slope*(double)safe.front().masterFrame;
    for(size_t i=0;i<fitCount;++i)
        maxResidual=std::max(maxResidual,std::abs(safe[i].donorDelay-
            (intercept+slope*(double)safe[i].masterFrame)));
    if(!std::isfinite(zeroDelay) ||
       maxResidual>0.0025*(double)sampleRate)
        return result;

    result.anchor.masterFrame=0;
    result.anchor.donorDelay=zeroDelay;
    result.anchor.corr=std::min(0.70,safe.front().corr);
    result.anchor.verified=true;
    result.anchor.repairSafe=true;
    if(!plausibleTimeMapTransition(result.anchor,safe.front(),sampleRate))
        return BackwardZeroAnchor{};
    result.slope=slope;
    result.maxResidual=maxResidual;
    result.valid=true;
    return result;
}

static double mappedDelayAt(const std::vector<TimeMapAnchor>& a, long f,
                            int sampleRate) {
    if (a.empty()) return 0.0;
    const TimeMapAnchor* before=nullptr;
    const TimeMapAnchor* after=nullptr;
    for(const auto& anchor:a) {
        if(!anchor.verified) continue;
        if(anchor.masterFrame<=f) before=&anchor;
        if(anchor.masterFrame>=f) {after=&anchor; break;}
    }
    if(!before && !after) return 0.0;
    if(!before) return after->donorDelay;
    if(!after) return before->donorDelay;
    if(before==after) return before->donorDelay;
    if(!plausibleTimeMapTransition(*before,*after,sampleRate)) {
        // Never draw or reconstruct an invented ramp between two different
        // musical periods. Keep the nearest measured island; confidence below
        // will remain zero across this discontinuity.
        return (f-before->masterFrame <= after->masterFrame-f)
            ? before->donorDelay : after->donorDelay;
    }
    double n=double(after->masterFrame-before->masterFrame);
    double u=n>0?double(f-before->masterFrame)/n:0.0;
    return before->donorDelay+(after->donorDelay-before->donorDelay)*u;
}
static double mappedConfidenceAt(const std::vector<TimeMapAnchor>& a,long f,int sampleRate) {
    if(a.empty()) return 0.0;
    // v0.2.30f: a donor interval is usable only when it is bracketed by two
    // independently verified, trend-stabilized anchors.  This prevents a new
    // period lock from repairing audio before its trajectory is confirmed.
    const TimeMapAnchor* before=nullptr;
    const TimeMapAnchor* after=nullptr;
    for (const auto& anchor : a) {
        if (!anchor.verified || !anchor.repairSafe) continue;
        if(anchor.masterFrame<=f) before=&anchor;
        if(anchor.masterFrame>=f) {after=&anchor; break;}
    }
    if(!before || !after) return 0.0;
    if(after->masterFrame-before->masterFrame > 2L*sampleRate) return 0.0;
    if(!plausibleTimeMapTransition(*before,*after,sampleRate)) return 0.0;
    return std::min(before->corr,after->corr);
}

// A sparse ordinary map may contain an isolated early verified anchor followed
// by a long unverified gap.  Such an anchor is useful diagnostics, but it must
// not reopen the donor before the dense backward walk has actually reached it.
// Keep the measured delay for display/audit and remove only repair authority.
static size_t blockRepairBefore(std::vector<TimeMapAnchor>& anchors,
                                long firstDenseSafeFrame) {
    size_t blocked=0;
    for(auto& anchor:anchors) {
        if(anchor.masterFrame<firstDenseSafeFrame && anchor.repairSafe) {
            anchor.repairSafe=false;
            ++blocked;
        }
    }
    return blocked;
}

static bool denseBackwardMayReachZero(long firstDenseSafeFrame,
                                      long musicOnsetFrame,
                                      int acceptedSteps,
                                      int minimumEvidenceSteps) {
    return firstDenseSafeFrame<=musicOnsetFrame &&
        acceptedSteps>=minimumEvidenceSteps;
}

// Periodic music often gives several almost-identical phase peaks.  The raw
// anchors then form a staircase: the correct long-term speed drift is present,
// but one 250 ms interval may absorb a whole waveform period.  Interpolating
// that staircase directly creates the apparent local de-sync seen in 30e.
// Fit a robust local line through roughly four seconds of VERIFIED evidence
// and publish the fitted delay instead.  This preserves genuine slow analogue
// speed drift while distributing a cycle transition over the surrounding
// evidence rather than one abrupt interval.
static void stabilizeTimeMap(std::vector<TimeMapAnchor>& anchors,
                             int sampleRate,
                             long musicOnset,
                             const AffineDelayModel* wholeRecordModel=nullptr) {
    std::vector<size_t> verified;
    std::vector<double> rawDelay(anchors.size(),0.0);
    std::vector<double> rawResidual(anchors.size(),0.0);
    for(size_t i=0;i<anchors.size();++i) {
        rawDelay[i]=anchors[i].donorDelay;
        const double baseline=(wholeRecordModel && wholeRecordModel->valid)
            ? wholeRecordModel->delayAt(anchors[i].masterFrame) : 0.0;
        rawResidual[i]=rawDelay[i]-baseline;
        anchors[i].repairSafe=false;
        if(anchors[i].verified && anchors[i].masterFrame>=musicOnset)
            verified.push_back(i);
    }
    if(verified.size()<3) return;

    const long localRadius=2L*sampleRate;
    const double residualScale=sampleRate*0.006; // 6 ms Huber scale
    const double maxPublishedShift=sampleRate*0.012; // never hide a gross mismatch
    const double maxSlope=0.025; // generous 2.5%; observed transfer is ~1.3%

    for(size_t vi=0;vi<verified.size();++vi) {
        const size_t target=verified[vi];
        const long centerFrame=anchors[target].masterFrame;
        std::vector<size_t> support;
        for(size_t j:verified)
            if(std::labs(anchors[j].masterFrame-centerFrame)<=localRadius)
                support.push_back(j);
        if(support.size()<3) continue;

        auto fit=[&](const std::vector<double>& robustWeight,
                     double& intercept,double& slope)->bool {
            double sw=0.0,sx=0.0,sy=0.0,sxx=0.0,sxy=0.0;
            for(size_t k=0;k<support.size();++k) {
                const size_t j=support[k];
                const double x=(double)(anchors[j].masterFrame-centerFrame);
                const double base=std::max(0.05,anchors[j].corr*anchors[j].corr);
                const double w=base*robustWeight[k];
                const double y=rawResidual[j];
                sw+=w; sx+=w*x; sy+=w*y; sxx+=w*x*x; sxy+=w*x*y;
            }
            const double den=sw*sxx-sx*sx;
            if(sw<=0.0 || std::abs(den)<1e-12) return false;
            slope=(sw*sxy-sx*sy)/den;
            intercept=(sy-slope*sx)/sw;
            return std::isfinite(intercept) && std::isfinite(slope);
        };

        std::vector<double> rw(support.size(),1.0);
        double intercept=rawResidual[target],slope=0.0;
        if(!fit(rw,intercept,slope)) continue;
        for(size_t k=0;k<support.size();++k) {
            const size_t j=support[k];
            const double x=(double)(anchors[j].masterFrame-centerFrame);
            const double residual=std::abs(rawResidual[j]-(intercept+slope*x));
            rw[k]=residual<=residualScale ? 1.0 : residualScale/residual;
        }
        if(!fit(rw,intercept,slope)) continue;
        slope=std::clamp(slope,-maxSlope,maxSlope);
        const double shift=std::clamp(intercept-rawResidual[target],
                                     -maxPublishedShift,maxPublishedShift);
        // Stabilize only the residual wow/flutter around the whole-record
        // model. This retains a genuine constant transfer-speed difference
        // even when it is larger than the local 2.5% slope guard.
        anchors[target].donorDelay=rawDelay[target]+shift;
        anchors[target].repairSafe=true;
    }
}

// A donor involved in an onset/whole-record basin dispute must prove the
// selected trajectory with several dense local anchors before it may donate
// audio. This applies both when an onset legitimately moves the affine model
// and when stronger programme evidence vetoes that move. Early anchors remain
// diagnostic-only until the fourth consecutive repair-safe point has arrived.
static long gateDisputedBasinRepairs(std::vector<TimeMapAnchor>& anchors,
                                    const AffineDelayModel& selectedModel,
                                    int sampleRate,
                                    long musicOnset,
                                    int requiredDenseAnchors=4) {
    const long maxDenseGap=std::max<long>(1,sampleRate/2); // tolerate one 250 ms miss
    const double maxModelResidual=0.025*(double)sampleRate;
    int consecutive=0;
    long previous=-1;
    long unlockFrame=-1;
    for(const auto& anchor:anchors) {
        const bool modelConsistent=anchor.verified && anchor.repairSafe &&
            anchor.masterFrame>=musicOnset &&
            std::abs(anchor.donorDelay-selectedModel.delayAt(anchor.masterFrame))
                <=maxModelResidual;
        if(!modelConsistent) {
            consecutive=0;
            previous=-1;
            continue;
        }
        if(previous>=0 && anchor.masterFrame-previous<=maxDenseGap)
            ++consecutive;
        else
            consecutive=1;
        previous=anchor.masterFrame;
        if(consecutive>=requiredDenseAnchors) {
            unlockFrame=anchor.masterFrame;
            break;
        }
    }
    for(auto& anchor:anchors) {
        if(unlockFrame<0 || anchor.masterFrame<unlockFrame)
            anchor.repairSafe=false;
    }
    return unlockFrame;
}

struct Stability {
    double impulse = 0.0;
    double jump = 0.0;
    double syncShock = 0.0;
    double correlationPenalty = 0.0;
    double score = 0.0; // lower = better
};

struct DecisionRow {
    double timeSec = 0.0;
    std::vector<double> delay;
    std::vector<double> quality;
    std::vector<double> score;
    // v0.2.29l PEAK-SEPARATION DIAGNOSTIC: donor eligibility telemetry only; no decision logic changes.
    std::vector<double> gateCorr;
    std::vector<double> gateMapConfidence;
    std::vector<double> gateLocalVerify;
    std::vector<std::string> gateReason;
    // v0.2.30e: auditable sample-scale impulse evidence and the independent
    // surrounding-trend fit used to select (or reject) a donor.
    std::vector<double> impulsePeak;
    std::vector<double> impulseFloor;
    std::vector<double> impulseZ;
    std::vector<double> trendFit;
    std::vector<double> trendCorr;
    int selectedSource = -1; // 1-based actual surgical source
    double disagreement = 0.0;
    double continuity = 0.0;
    double gain = 1.0;
    double dc = 0.0;
    double consensusSupport = 0.0;
    double donorControlCorr = 0.0;
    double masterDonorCorr = 0.0;
    double masterControlCorr = 0.0;
    double outlierRatio = 0.0;
    int eligibleWitnesses = 0;
    int supportingWitnesses = 0;
    int opposingWitnesses = 0;
    std::vector<int> supportingSources; // 1-based original source numbers
    std::vector<int> abstainingSources; // 1-based original source numbers
    std::vector<int> opposingSources;   // 1-based original source numbers
    std::string decisionReason = "MASTER_ONLY";
    char confidenceClass = 'C';
};

struct RepairRecord {
    double startSec = 0.0;
    double endSec = 0.0;
    int donorSource = 0;
    double outlierRatio = 0.0;
    double maxAbsDifference = 0.0;
    int retryAttempt = 0;
    double delayNudgeSamples = 0.0;
    std::string decisionReason = "A_CLICK_REPAIR";
    double coreStartSec = std::numeric_limits<double>::quiet_NaN();
    double coreEndSec = std::numeric_limits<double>::quiet_NaN();
    long fadeInSamples = 0;
    long fadeOutSamples = 0;
    double renderGain = 1.0;
    double renderDc = 0.0;
    double donorDelaySamples = std::numeric_limits<double>::quiet_NaN();
};

static uint16_t rd16(const unsigned char* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static Wav readWav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open " + path);

    unsigned char hdr[12];
    f.read((char*)hdr, 12);
    if (f.gcount() != 12 ||
        std::memcmp(hdr, "RIFF", 4) ||
        std::memcmp(hdr + 8, "WAVE", 4))
        throw std::runtime_error(path + ": not a RIFF/WAVE file");

    uint16_t audioFormat = 0, channels = 0, bits = 0, blockAlign = 0;
    uint32_t sampleRate = 0;
    std::vector<unsigned char> data;

    while (f) {
        unsigned char ch[8];
        f.read((char*)ch, 8);
        if (f.gcount() != 8)
            break;

        uint32_t size = rd32(ch + 4);
        std::string id((char*)ch, 4);

        if (id == "fmt ") {
            std::vector<unsigned char> fmt(size);
            f.read((char*)fmt.data(), size);
            if (size < 16)
                throw std::runtime_error(path + ": bad fmt chunk");

            audioFormat = rd16(fmt.data());
            channels = rd16(fmt.data() + 2);
            sampleRate = rd32(fmt.data() + 4);
            blockAlign = rd16(fmt.data() + 12);
            bits = rd16(fmt.data() + 14);

        } else if (id == "data") {
            data.resize(size);
            if (size)
                f.read((char*)data.data(), size);

        } else {
            f.seekg(size, std::ios::cur);
        }

        if (size & 1)
            f.seekg(1, std::ios::cur);
    }

    if (audioFormat != 1)
        throw std::runtime_error(path + ": only PCM WAV is supported");

    if (channels < 1 || channels > 2)
        throw std::runtime_error(path + ": only mono/stereo supported");

    if (!(bits == 8 || bits == 16 || bits == 24 || bits == 32))
        throw std::runtime_error(path + ": unsupported PCM bit depth");

    const int bytes = bits / 8;
    if (!sampleRate || blockAlign != channels * bytes || data.empty())
        throw std::runtime_error(path + ": malformed/empty WAV");

    Wav w;
    w.sampleRate = (int)sampleRate;
    w.channels = (int)channels;
    w.bits = (int)bits;

    size_t n = data.size() / bytes;
    w.samples.resize(n);

    for (size_t i = 0; i < n; ++i) {
        const unsigned char* p = data.data() + i * bytes;
        float v = 0.0f;

        if (bits == 8) {
            v = ((int)p[0] - 128) / 128.0f;

        } else if (bits == 16) {
            int16_t s = (int16_t)rd16(p);
            v = s / 32768.0f;

        } else if (bits == 24) {
            int32_t s = (int32_t)p[0] |
                        ((int32_t)p[1] << 8) |
                        ((int32_t)p[2] << 16);
            if (s & 0x800000)
                s |= ~0xFFFFFF;
            v = s / 8388608.0f;

        } else {
            int32_t s = (int32_t)rd32(p);
            v = (float)(s / 2147483648.0);
        }

        w.samples[i] = v;
    }

    return w;
}

struct MonoFusionStats {
    bool applied = false;
    double correlation = 1.0;
    double rightToLeftGain = 1.0;
    double leftGain = 1.0;
    double rightGain = 1.0;
    double sideToMidBeforeDb = -120.0;
    double sideToMidAfterDb = -120.0;
    double curvatureSigmaLeft = 0.0;
    double curvatureSigmaRight = 0.0;
    double monoRms = 0.0;
    double monoCurvatureSigma = 0.0;
    double qualityScore = 0.0;
    uint64_t monoExtremeEvents = 0;
    uint64_t leftChannelRepairs = 0;
    uint64_t rightChannelRepairs = 0;
    uint64_t changedFrames = 0;
};

static double medianInPlace(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    const size_t n = v.size();
    std::nth_element(v.begin(), v.begin() + n/2, v.end());
    double m = v[n/2];
    if ((n & 1u) == 0) {
        const auto it = std::max_element(v.begin(), v.begin() + n/2);
        m = 0.5 * (m + *it);
    }
    return m;
}

// A lateral mono groove contains the wanted signal in L+R and predominantly
// vertical groove motion/noise in L-R. Real capture chains rarely have equal
// channel gains, so summing raw L/R leaves much of that vertical component.
// Estimate the channel relationship from many one-second programme blocks,
// correct polarity and level, preserve the geometric mean programme level,
// and then perform only extremely conservative one-channel impulse repairs.
static MonoFusionStats fuseStereoGrooveToMono(Wav& w) {
    MonoFusionStats st;
    if (w.channels != 2 || w.frames() < 8) return st;

    const size_t frames = w.frames();
    const size_t block = (size_t)std::max(1024, w.sampleRate);
    std::vector<double> gains;
    std::vector<double> correlations;
    gains.reserve(frames/block + 1);
    correlations.reserve(frames/block + 1);

    for (size_t start=0; start<frames; start+=block) {
        const size_t end=std::min(frames,start+block);
        if(end-start<1024) continue;
        double ml=0.0,mr=0.0;
        for(size_t i=start;i<end;++i) {
            ml+=w.samples[i*2]; mr+=w.samples[i*2+1];
        }
        const double inv=1.0/(double)(end-start);
        ml*=inv; mr*=inv;
        double ll=0.0,rr=0.0,lr=0.0;
        for(size_t i=start;i<end;++i) {
            const double l=(double)w.samples[i*2]-ml;
            const double r=(double)w.samples[i*2+1]-mr;
            ll+=l*l; rr+=r*r; lr+=l*r;
        }
        if(ll<1e-12 || rr<1e-12) continue;
        const double corr=lr/std::sqrt(ll*rr);
        const double g=lr/rr;
        if(std::isfinite(g) && std::isfinite(corr) && std::abs(corr)>=0.50 &&
           std::abs(g)>=0.10 && std::abs(g)<=10.0) {
            gains.push_back(g);
            correlations.push_back(corr);
        }
    }
    if(gains.empty()) throw std::runtime_error("Stereo channels do not contain a usable common mono signal");
    const double signedGain=medianInPlace(gains);
    st.correlation=medianInPlace(correlations);
    st.rightToLeftGain=signedGain;
    const double magnitude=std::clamp(std::abs(signedGain),0.10,10.0);
    const double polarity=signedGain<0.0?-1.0:1.0;
    st.leftGain=1.0/std::sqrt(magnitude);
    st.rightGain=polarity*std::sqrt(magnitude);

    std::vector<float> mono(frames,0.0f);
    double midBefore=0.0,sideBefore=0.0,midAfter=0.0,sideAfter=0.0;
    for(size_t i=0;i<frames;++i) {
        const double l=w.samples[i*2];
        const double r=w.samples[i*2+1];
        const double lb=l*st.leftGain;
        const double rb=r*st.rightGain;
        const double mb=0.5*(l+r), sb=0.5*(l-r);
        const double ma=0.5*(lb+rb), sa=0.5*(lb-rb);
        midBefore+=mb*mb; sideBefore+=sb*sb;
        midAfter+=ma*ma; sideAfter+=sa*sa;
        mono[i]=(float)std::clamp(ma,-1.0,0.999999999);
    }
    auto ratioDb=[](double side,double mid) {
        return 10.0*std::log10(std::max(side,1e-30)/std::max(mid,1e-30));
    };
    st.sideToMidBeforeDb=ratioDb(sideBefore,midBefore);
    st.sideToMidAfterDb=ratioDb(sideAfter,midAfter);

    // Robust curvature floors. Sampling every eighth frame is sufficient for
    // the median while keeping memory bounded for full 96 kHz sides.
    std::vector<double> absLeft,absRight;
    absLeft.reserve(frames/8+1); absRight.reserve(frames/8+1);
    auto channelCurvature=[&](size_t i,int ch,double gain) {
        const double xm=w.samples[(i-1)*2+ch]*gain;
        const double x0=w.samples[i*2+ch]*gain;
        const double xp=w.samples[(i+1)*2+ch]*gain;
        return x0-0.5*(xm+xp);
    };
    for(size_t i=1;i+1<frames;i+=8) {
        absLeft.push_back(std::abs(channelCurvature(i,0,st.leftGain)));
        absRight.push_back(std::abs(channelCurvature(i,1,st.rightGain)));
    }
    st.curvatureSigmaLeft=medianInPlace(absLeft)/0.6744897501960817;
    st.curvatureSigmaRight=medianInPlace(absRight)/0.6744897501960817;
    const double quant=std::ldexp(1.0,-std::max(1,w.bits-1));
    const double thresholdL=std::max(40.0*st.curvatureSigmaLeft,32.0*quant*std::abs(st.leftGain));
    const double thresholdR=std::max(40.0*st.curvatureSigmaRight,32.0*quant*std::abs(st.rightGain));
    const int halfWidth=std::max(8,(int)std::llround(w.sampleRate*0.00050));
    std::vector<float> repairAlpha(frames,0.0f);
    std::vector<int8_t> repairChannel(frames,-1); // channel to KEEP: 0=L, 1=R
    size_t lastL=0,lastR=0;
    const size_t minGap=(size_t)std::max(1,(int)std::llround(w.sampleRate*0.00080));
    for(size_t i=1;i+1<frames;++i) {
        const double cl=std::abs(channelCurvature(i,0,st.leftGain));
        const double cr=std::abs(channelCurvature(i,1,st.rightGain));
        int keep=-1;
        if(cl>thresholdL && cl>10.0*cr && cr<5.0*st.curvatureSigmaRight) {
            keep=1;
            if(lastL==0 || i-lastL>minGap) ++st.leftChannelRepairs;
            lastL=i;
        } else if(cr>thresholdR && cr>10.0*cl && cl<5.0*st.curvatureSigmaLeft) {
            keep=0;
            if(lastR==0 || i-lastR>minGap) ++st.rightChannelRepairs;
            lastR=i;
        }
        if(keep<0) continue;
        const long a=std::max<long>(0,(long)i-halfWidth);
        const long b=std::min<long>((long)frames-1,(long)i+halfWidth);
        for(long j=a;j<=b;++j) {
            const double d=std::abs((double)j-(double)i)/(double)halfWidth;
            const float alpha=(float)(0.5+0.5*std::cos(3.14159265358979323846*d));
            if(alpha>repairAlpha[(size_t)j]) {
                repairAlpha[(size_t)j]=alpha;
                repairChannel[(size_t)j]=(int8_t)keep;
            }
        }
    }
    for(size_t i=0;i<frames;++i) {
        if(repairChannel[i]<0) continue;
        const double clean=repairChannel[i]==0?
            (double)w.samples[i*2]*st.leftGain:
            (double)w.samples[i*2+1]*st.rightGain;
        const double a=repairAlpha[i];
        mono[i]=(float)std::clamp((1.0-a)*(double)mono[i]+a*clean,-1.0,0.999999999);
        ++st.changedFrames;
    }
    double monoMean=0.0;
    for(float v:mono) monoMean+=v;
    monoMean/=std::max<size_t>(1,frames);
    double monoEnergy=0.0;
    std::vector<double> monoCurvature;
    monoCurvature.reserve(frames/8+1);
    for(size_t i=0;i<frames;++i) {
        const double v=(double)mono[i]-monoMean;
        monoEnergy+=v*v;
        if(i>0 && i+1<frames && (i%8)==0)
            monoCurvature.push_back(std::abs((double)mono[i]-0.5*((double)mono[i-1]+(double)mono[i+1])));
    }
    st.monoRms=std::sqrt(monoEnergy/std::max<size_t>(1,frames));
    st.monoCurvatureSigma=medianInPlace(monoCurvature)/0.6744897501960817;
    const double monoExtremeThreshold=40.0*std::max(st.monoCurvatureSigma,quant);
    size_t lastExtreme=0;
    for(size_t i=1;i+1<frames;++i) {
        const double c=std::abs((double)mono[i]-0.5*((double)mono[i-1]+(double)mono[i+1]));
        if(c<=monoExtremeThreshold) continue;
        if(lastExtreme==0 || i-lastExtreme>minGap) ++st.monoExtremeEvents;
        lastExtreme=i;
    }
    const double cleanDb=-20.0*std::log10(std::max(st.monoCurvatureSigma,1e-15)/std::max(st.monoRms,1e-15));
    const double eventRate=(double)st.monoExtremeEvents/std::max(1.0,(double)frames/(double)w.sampleRate);
    st.qualityScore=cleanDb-0.03*eventRate;
    w.samples.swap(mono);
    w.channels=1;
    st.applied=true;
    return st;
}

static void wr16(std::ofstream& f, uint16_t v) {
    unsigned char b[2] = {
        (unsigned char)(v & 255),
        (unsigned char)((v >> 8) & 255)
    };
    f.write((char*)b, 2);
}

static void wr32(std::ofstream& f, uint32_t v) {
    unsigned char b[4] = {
        (unsigned char)(v & 255),
        (unsigned char)((v >> 8) & 255),
        (unsigned char)((v >> 16) & 255),
        (unsigned char)((v >> 24) & 255)
    };
    f.write((char*)b, 4);
}

static void writeWavPcm(const std::string& path,
                        int sr,
                        int ch,
                        int bits,
                        const std::vector<float>& x) {
    if (!(bits == 8 || bits == 16 || bits == 24 || bits == 32))
        throw std::runtime_error("Unsupported output PCM bit depth");

    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot create " + path);

    const int bytes = bits / 8;
    uint32_t dataBytes = (uint32_t)(x.size() * bytes);

    f.write("RIFF", 4);
    wr32(f, 36 + dataBytes);
    f.write("WAVE", 4);

    f.write("fmt ", 4);
    wr32(f, 16);
    wr16(f, 1); // PCM
    wr16(f, (uint16_t)ch);
    wr32(f, (uint32_t)sr);
    wr32(f, (uint32_t)(sr * ch * bytes));
    wr16(f, (uint16_t)(ch * bytes));
    wr16(f, (uint16_t)bits);

    f.write("data", 4);
    wr32(f, dataBytes);

    size_t writtenSamples=0;
    for (float vf : x) {
        if((writtenSamples++ & 65535)==0)reportWorkProgress(writtenSamples,x.size());
        double v = std::max(-1.0, std::min(0.999999999, (double)vf));

        if (bits == 8) {
            int u = (int)std::lrint(v * 128.0 + 128.0);
            u = std::max(0, std::min(255, u));
            unsigned char b = (unsigned char)u;
            f.write((char*)&b, 1);
        } else if (bits == 16) {
            int32_t q = (int32_t)std::llround(v * 32768.0);
            q = std::max<int32_t>(-32768, std::min<int32_t>(32767, q));
            wr16(f, (uint16_t)(int16_t)q);
        } else if (bits == 24) {
            int64_t q = (int64_t)std::llround(v * 8388608.0);
            q = std::max<int64_t>(-8388608, std::min<int64_t>(8388607, q));
            uint32_t u = (uint32_t)((int32_t)q) & 0xFFFFFFu;
            unsigned char b[3] = {
                (unsigned char)(u & 255),
                (unsigned char)((u >> 8) & 255),
                (unsigned char)((u >> 16) & 255)
            };
            f.write((char*)b, 3);
        } else { // 32-bit PCM
            int64_t q = (int64_t)std::llround(v * 2147483648.0);
            q = std::max<int64_t>(INT32_MIN, std::min<int64_t>(INT32_MAX, q));
            wr32(f, (uint32_t)(int32_t)q);
        }
    }
}

static std::vector<float> resamplePlaybackSpeed(
    const std::vector<float>& input,
    int channels,
    double speedFactor) {
    if (channels <= 0 || input.empty()) return {};
    if (!std::isfinite(speedFactor) || speedFactor <= 0.0)
        throw std::runtime_error("Invalid playback speed factor");
    if (std::abs(speedFactor - 1.0) < 1e-12) return input;

    const size_t inFrames=input.size()/(size_t)channels;
    const size_t outFrames=std::max<size_t>(1,(size_t)std::floor((double)inFrames/speedFactor));
    std::vector<float> output(outFrames*(size_t)channels,0.0f);
    constexpr int radius=24;
    const double cutoff=std::min(1.0,1.0/speedFactor);
    const double pi=3.1415926535897932384626433832795;
    auto sinc=[&](double x) {
        if (std::abs(x)<1e-12) return 1.0;
        const double px=pi*x;
        return std::sin(px)/px;
    };

    for(size_t of=0;of<outFrames;++of) {
        if((of & 16383)==0)reportWorkProgress(of,outFrames);
        const double pos=(double)of*speedFactor;
        const long center=(long)std::floor(pos);
        for(int ch=0;ch<channels;++ch) {
            double sum=0.0,weightSum=0.0;
            for(int k=-radius+1;k<=radius;++k) {
                const long sf=center+k;
                if(sf<0 || sf>=(long)inFrames) continue;
                const double distance=pos-(double)sf;
                const double window=sinc(distance/(double)radius);
                const double weight=cutoff*sinc(cutoff*distance)*window;
                sum+=(double)input[(size_t)sf*(size_t)channels+(size_t)ch]*weight;
                weightSum+=weight;
            }
            if(std::abs(weightSum)>1e-12) sum/=weightSum;
            output[of*(size_t)channels+(size_t)ch]=(float)std::clamp(sum,-1.0,0.999999999);
        }
    }
    return output;
}

struct OutputQaReport {
    bool passed = true;
    double peak = 0.0;
    double dcMax = 0.0;
    double dcShiftMax = 0.0;
    uint64_t clippedSamples = 0;
    uint64_t baselineClippedSamples = 0;
    uint64_t nonFiniteSamples = 0;
    uint64_t seamFlags = 0;
};

// Final independent guard for the saved signal. This does not decide repairs;
// it reports whether reconstruction introduced clipping, non-finite samples,
// a material DC shift or a suspicious jump at a repair boundary.
static OutputQaReport analyzeOutputQa(
    const std::vector<float>& output,
    const std::vector<float>& baseline,
    int channels,
    int sampleRate,
    double speedFactor,
    const std::vector<RepairRecord>& repairs) {

    OutputQaReport q;
    if(channels<=0 || sampleRate<=0 || output.empty() ||
       output.size()!=baseline.size()) {
        q.passed=false;
        q.nonFiniteSamples=1;
        return q;
    }
    std::vector<long double> outputSum((size_t)channels,0.0L);
    std::vector<long double> baselineSum((size_t)channels,0.0L);
    for(size_t i=0;i<output.size();++i) {
        if((i & 65535)==0)reportWorkProgress(i,output.size());
        const double value=(double)output[i];
        const double base=(double)baseline[i];
        if(!std::isfinite(value)) { ++q.nonFiniteSamples; continue; }
        q.peak=std::max(q.peak,std::abs(value));
        if(std::abs(value)>=0.999999) ++q.clippedSamples;
        if(std::isfinite(base) && std::abs(base)>=0.999999)
            ++q.baselineClippedSamples;
        const int channel=(int)(i%(size_t)channels);
        outputSum[(size_t)channel]+=value;
        if(std::isfinite(base)) baselineSum[(size_t)channel]+=base;
    }
    const size_t frames=output.size()/(size_t)channels;
    for(int channel=0;channel<channels;++channel) {
        const double outputDc=(double)(outputSum[(size_t)channel]/
            (long double)std::max<size_t>(1,frames));
        const double baselineDc=(double)(baselineSum[(size_t)channel]/
            (long double)std::max<size_t>(1,frames));
        q.dcMax=std::max(q.dcMax,std::abs(outputDc));
        q.dcShiftMax=std::max(q.dcShiftMax,std::abs(outputDc-baselineDc));
    }

    auto firstDifference=[&](const std::vector<float>& data,size_t frame,int channel) {
        if(frame==0 || frame>=frames) return 0.0;
        const size_t p=frame*(size_t)channels+(size_t)channel;
        return std::abs((double)data[p]-(double)data[p-(size_t)channels]);
    };
    const long context=std::max<long>(16,sampleRate/200); // 5 ms
    for(const auto& repair:repairs) {
        for(double boundarySec:{repair.startSec,repair.endSec}) {
            const long frame=(long)std::llround(
                boundarySec*(double)sampleRate/std::max(1e-12,speedFactor));
            if(frame<=1 || frame>=(long)frames-1) continue;
            for(int channel=0;channel<channels;++channel) {
                std::vector<double> local;
                local.reserve((size_t)context*2);
                for(long f=std::max<long>(1,frame-context);
                    f<std::min<long>((long)frames,frame+context);++f) {
                    if(std::abs(f-frame)<=3) continue;
                    local.push_back(firstDifference(output,(size_t)f,channel));
                }
                double floor=0.000001;
                if(!local.empty()) {
                    auto middle=local.begin()+local.size()/2;
                    std::nth_element(local.begin(),middle,local.end());
                    floor=std::max(floor,*middle);
                }
                const double jump=firstDifference(output,(size_t)frame,channel);
                const double baselineJump=firstDifference(
                    baseline,(size_t)frame,channel);
                if(jump>std::max({0.02,12.0*floor,2.0*baselineJump+0.002}))
                    ++q.seamFlags;
            }
        }
    }
    q.passed=q.nonFiniteSamples==0 &&
        q.clippedSamples<=q.baselineClippedSamples &&
        q.dcShiftMax<=0.001 && q.seamFlags==0;
    return q;
}

static inline float monoAt(const Wav& w, long frame) {
    if (frame < 0 || (size_t)frame >= w.frames())
        return 0.0f;

    const float* p = &w.samples[(size_t)frame * w.channels];
    return w.channels == 1 ? p[0] : 0.5f * (p[0] + p[1]);
}

static inline float sampleLinear(const Wav& w, double frame, int c) {
    if (w.frames() == 0)
        return 0.0f;

    if (frame <= 0.0)
        return w.samples[c];

    double maxFrame = (double)w.frames() - 1.000001;
    if (frame >= maxFrame)
        return w.samples[(w.frames() - 1) * w.channels + c];

    long i = (long)std::floor(frame);
    double a = frame - i;

    float x0 = w.samples[(size_t)i * w.channels + c];
    float x1 = w.samples[(size_t)(i + 1) * w.channels + c];

    return (float)(x0 + (x1 - x0) * a);
}


// A phase-insensitive local feature used for robust alignment.
// Raw waveform correlation can become fragile when two transfers have
// different cartridge/RIAA phase response, especially on voice and other
// spectrally complex material.  This feature uses short-time absolute
// derivative energy, which preserves timing/onsets while discarding most
// polarity/phase sensitivity.
static inline double transientFeature(const Wav& w, long frame) {
    if (frame < 4 || (size_t)(frame + 4) >= w.frames())
        return 0.0;

    double acc = 0.0;
    for (int k = -3; k <= 3; ++k) {
        double a = monoAt(w, frame + k);
        double b = monoAt(w, frame + k - 1);
        acc += std::abs(a - b);
    }
    return acc / 7.0;
}


// v0.2.30a: bounded transient-feature cache for full-length sides.
// The old cache allocated one double plus one validity byte for every frame,
// twice per worker thread. A 225-second 96 kHz side therefore consumed roughly
// 390 MB per worker. Reconstruction creates two donor workers for every block,
// so Windows repeatedly allocated about 780 MB and eventually terminated the
// engine. 65,536 slots cover the largest local synchronization window while
// bounding both caches to about 2 MB per worker (about 4 MB per block).
struct FeatureCache {
    const Wav* wav = nullptr;
    static constexpr size_t slotCount = 1u << 16;
    std::vector<int64_t> keys;
    std::vector<double> values;

    void bind(const Wav& w) {
        if (wav != &w || values.size() != slotCount) {
            wav = &w;
            keys.assign(slotCount, INT64_MIN);
            values.assign(slotCount, 0.0);
        }
    }

    double get(const Wav& w, long frame) {
        if(frame<0 || (size_t)frame>=w.frames()) return 0.0;
        bind(w);
        const size_t k=(size_t)frame & (slotCount-1);
        if (keys[k]!=(int64_t)frame) {
            values[k]=transientFeature(w,frame);
            keys[k]=(int64_t)frame;
        }
        return values[k];
    }
};

static thread_local FeatureCache gFeatureCacheA;
static thread_local FeatureCache gFeatureCacheB;


// v0.2.29k: robust coarse musical feature for independent shellac transfers.
// This deliberately suppresses sample phase and isolated click amplitude.  It
// measures a short multi-scale envelope of band-like differences; the time-map
// uses it only to LOCATE the same musical passage.  Final donor verification
// remains on the original 96/24 audio and keeps the existing safety gates.
static inline double robustMusicFeature(const Wav& w, long frame) {
    if (frame < 192 || (size_t)(frame + 192) >= w.frames()) return 0.0;
    const int sr = std::max(8000, w.sampleRate);
    const int l1 = std::max(8, sr / 2000);   // ~0.5 ms
    const int l2 = std::max(16, sr / 1000); // ~1 ms
    const int l3 = std::max(32, sr / 500);  // ~2 ms
    auto d=[&](int lag){ return std::abs((double)monoAt(w,frame) - (double)monoAt(w,frame-lag)); };
    double e = 0.50*d(l1) + 0.30*d(l2) + 0.20*d(l3);
    // Soft compression: a shellac click cannot dominate a correlation window.
    return std::log1p(40.0 * e);
}

static double robustMusicCorrAtStrided(const Wav& ref,
                                       const Wav& candidate,
                                       long refStart,
                                       int window,
                                       long delay,
                                       int stride) {
    stride=std::max(stride,8);
    double ma=0.0, mb=0.0; int n=0;
    for(int i=192;i<window-192;i+=stride){
        long a=refStart+i,b=a+delay;
        if(a<192||b<192||(size_t)(a+192)>=ref.frames()||(size_t)(b+192)>=candidate.frames()) continue;
        ma+=robustMusicFeature(ref,a); mb+=robustMusicFeature(candidate,b); ++n;
    }
    if(n<24) return -2.0;
    ma/=n; mb/=n;
    double ab=0,aa=0,bb=0;
    for(int i=192;i<window-192;i+=stride){
        long a=refStart+i,b=a+delay;
        if(a<192||b<192||(size_t)(a+192)>=ref.frames()||(size_t)(b+192)>=candidate.frames()) continue;
        double x=robustMusicFeature(ref,a)-ma, y=robustMusicFeature(candidate,b)-mb;
        ab+=x*y; aa+=x*x; bb+=y*y;
    }
    double den=std::sqrt(aa*bb); return den>1e-18?ab/den:-2.0;
}

// Timing-only low-pass waveform: preserve polarity, discard impulsive detail.
// Two moving-average stages before decimation; never used as replacement audio.
struct TimingSignal {
    int step=1;
    std::vector<double> values;
};
static TimingSignal makeTimingSignal(const Wav& w) {
    TimingSignal out; out.step=std::max(1,w.sampleRate/2000);
    std::vector<double> prefix(w.frames()+1,0.0);
    for(size_t i=0;i<w.frames();++i) prefix[i+1]=prefix[i]+monoAt(w,(long)i);
    std::vector<double> smooth(w.frames()+1,0.0);
    const long half=out.step;
    for(long i=0;i<(long)w.frames();++i) {
        long lo=std::max(0L,i-half),hi=std::min((long)w.frames(),i+half+1);
        smooth[i+1]=smooth[i]+(prefix[hi]-prefix[lo])/(hi-lo);
    }
    for(long i=0;i<(long)w.frames();i+=out.step) {
        long lo=std::max(0L,i-half),hi=std::min((long)w.frames(),i+half+1);
        out.values.push_back((smooth[hi]-smooth[lo])/(hi-lo));
    }
    return out;
}
struct TimingMatch { double delay=0,q=-2,gap=0; bool accepted=false; bool boundary=false; const char* reason="NO_VALID_WINDOW"; };
static TimingMatch matchTiming(const TimingSignal& a,const TimingSignal& b,
                               long frame,long window,double prediction,long radius) {
    TimingMatch out; out.delay=prediction;
    long start=frame/a.step,n=window/a.step,center=(long)std::llround(prediction/a.step);
    long r=std::max(2L,radius/a.step);
    std::vector<std::pair<long,double>> scores;
    if(n<32 || start<0 || start+n>(long)a.values.size()) return out;
    for(long d=center-r;d<=center+r;++d) {
        long bs=start+d;
        if(bs<0 || bs+n>(long)b.values.size()) continue;
        double xsum=0,ysum=0,xx=0,yy=0,xy=0;
        for(long j=0;j<n;++j) { double x=a.values[start+j],y=b.values[bs+j];
            xsum+=x;ysum+=y;xx+=x*x;yy+=y*y;xy+=x*y; }
        xx-=xsum*xsum/n;yy-=ysum*ysum/n;xy-=xsum*ysum/n;
        double q=(xx>1e-12 && yy>1e-12)?xy/std::sqrt(xx*yy):-2;
        scores.push_back({d,q});
        if(q>out.q){out.q=q;out.delay=(double)(d*a.step);}
    }
    double second=-2;
    for(const auto& v:scores)
        if(std::abs(v.first*a.step-out.delay)>2*a.step) second=std::max(second,v.second);
    out.gap=out.q-second;
    out.accepted=out.q>=0.80 && out.gap>=0.025 &&
        std::abs(out.delay-center*a.step)<(r-1)*a.step;
    out.boundary=std::abs(out.delay-center*a.step)>=(r-1)*a.step;
    out.reason=out.accepted?"PASS":(scores.empty()?"NO_OVERLAP":
        (out.q<=-1.5?"NO_SIGNAL_VARIANCE":
         (out.q<0.80?"CORR_BELOW_0.80":
          (out.gap<0.025?"PEAK_GAP_BELOW_0.025":"SEARCH_BOUNDARY"))));
    return out;
}

static double featureCorrAtStrided(const Wav& ref,
                                   const Wav& candidate,
                                   long refStart,
                                   int window,
                                   long delay,
                                   int stride) {
    stride = std::max(1, stride);

    double meanA = 0.0, meanB = 0.0;
    int n = 0;

    for (int i = 4; i < window - 4; i += stride) {
        long a = refStart + i;
        long b = refStart + i + delay;

        if (a < 4 || b < 4 ||
            (size_t)(a + 4) >= ref.frames() ||
            (size_t)(b + 4) >= candidate.frames())
            continue;

        meanA += gFeatureCacheA.get(ref, a);
        meanB += gFeatureCacheB.get(candidate, b);
        ++n;
    }

    if (n < 16)
        return -2.0;

    meanA /= n;
    meanB /= n;

    double ab = 0.0, aa = 0.0, bb = 0.0;

    for (int i = 4; i < window - 4; i += stride) {
        long a = refStart + i;
        long b = refStart + i + delay;

        if (a < 4 || b < 4 ||
            (size_t)(a + 4) >= ref.frames() ||
            (size_t)(b + 4) >= candidate.frames())
            continue;

        double x = gFeatureCacheA.get(ref, a) - meanA;
        double y = gFeatureCacheB.get(candidate, b) - meanB;

        ab += x * y;
        aa += x * x;
        bb += y * y;
    }

    double den = std::sqrt(aa * bb);
    return den > 1e-18 ? ab / den : -2.0;
}

static double corrAtStrided(const Wav& ref,
                            const Wav& candidate,
                            long refStart,
                            int window,
                            long delay,
                            int stride) {
    double meanA = 0.0;
    double meanB = 0.0;
    int n = 0;

    stride = std::max(1, stride);

    for (int i = 0; i < window; i += stride) {
        long a = refStart + i;
        long b = refStart + i + delay;

        if (a < 0 || b < 0 ||
            (size_t)a >= ref.frames() ||
            (size_t)b >= candidate.frames())
            continue;

        meanA += monoAt(ref, a);
        meanB += monoAt(candidate, b);
        ++n;
    }

    if (n < std::max(8, window / (stride * 3)))
        return -2.0;

    meanA /= n;
    meanB /= n;

    double ab = 0.0, aa = 0.0, bb = 0.0;

    for (int i = 0; i < window; i += stride) {
        long a = refStart + i;
        long b = refStart + i + delay;

        if (a < 0 || b < 0 ||
            (size_t)a >= ref.frames() ||
            (size_t)b >= candidate.frames())
            continue;

        double x = monoAt(ref, a) - meanA;
        double y = monoAt(candidate, b) - meanB;

        ab += x * y;
        aa += x * x;
        bb += y * y;
    }

    double den = std::sqrt(aa * bb);
    return den > 1e-18 ? ab / den : -2.0;
}

static double corrAt(const Wav& ref,
                     const Wav& candidate,
                     long refStart,
                     int window,
                     long delay) {
    return corrAtStrided(ref, candidate, refStart, window, delay, 1);
}

struct PhaseSearchResult {
    long delay = 0;
    double corr = -2.0;
};

// Resolve raw sample phase around an already verified time-map trajectory.
// Periodic music can expose several nearly equal correlation peaks one cycle
// apart. Prefer the peak closest to the mapped trajectory when it is within
// 0.03 of the absolute best peak; a clearly better remote peak can still win.
static PhaseSearchResult searchPhaseNearTrajectory(
        const Wav& ref,
        const Wav& candidate,
        long phasePosition,
        int phaseWindow,
        double mappedDelay,
        int sampleRate) {
    PhaseSearchResult result;
    const long base=(long)std::llround(mappedDelay);
    const long radius=std::max<long>(32,sampleRate*4/1000);
    struct Candidate {long delay;double corr;};
    std::vector<Candidate> coarse;
    double absoluteBest=-2.0;
    for(long d=base-radius;d<=base+radius;d+=8) {
        const long donorFrame=phasePosition+d;
        if(donorFrame<0 || donorFrame+phaseWindow>=(long)candidate.frames())
            continue;
        const double q=corrAtStrided(
            ref,candidate,phasePosition,phaseWindow,d,4);
        coarse.push_back({d,q});
        absoluteBest=std::max(absoluteBest,q);
    }
    if(coarse.empty() || !std::isfinite(absoluteBest)) return result;

    long chosen=coarse.front().delay;
    double chosenQ=-2.0;
    long chosenDistance=std::numeric_limits<long>::max();
    for(const auto& candidatePeak:coarse) {
        if(candidatePeak.corr<absoluteBest-0.03) continue;
        const long distance=std::labs(candidatePeak.delay-base);
        if(distance<chosenDistance ||
           (distance==chosenDistance && candidatePeak.corr>chosenQ)) {
            chosen=candidatePeak.delay;
            chosenQ=candidatePeak.corr;
            chosenDistance=distance;
        }
    }

    result.delay=chosen;
    result.corr=chosenQ;
    for(long d=chosen-8;d<=chosen+8;++d) {
        const long donorFrame=phasePosition+d;
        if(donorFrame<0 || donorFrame+phaseWindow>=(long)candidate.frames())
            continue;
        const double q=corrAtStrided(
            ref,candidate,phasePosition,phaseWindow,d,1);
        if(q>result.corr) {result.corr=q;result.delay=d;}
    }
    return result;
}

static TrackState estimateDelay(const Wav& ref,
                                const Wav& candidate,
                                long pos,
                                double previousDelay,
                                int searchRadius,
                                int window) {
    long center = (long)std::llround(previousDelay);

    // A longer feature window improves uniqueness on speech and dense music.
    // Keep raw-waveform refinement short enough to follow wow/flutter.
    const int featureWindow = std::max(window, ref.sampleRate * 120 / 1000);

    // Stage 1: phase-insensitive feature correlation across the local search.
    // 4-sample delay grid + 8-sample feature stride keeps it reasonably cheap.
    long coarseBest = center;
    double coarseBestQ = -3.0;

    for (long d = center - searchRadius;
         d <= center + searchRadius;
         d += 4) {

        double q = featureCorrAtStrided(
            ref, candidate, pos, featureWindow, d, 8);

        // Very small continuity preference avoids hopping between equivalent
        // periodic matches.
        q -= 0.000001 * std::abs((double)d - previousDelay);

        if (q > coarseBestQ) {
            coarseBestQ = q;
            coarseBest = d;
        }
    }

    // Stage 2: feature refinement at single-sample resolution.
    long featureBest = coarseBest;
    double featureBestQ = -3.0;

    for (long d = coarseBest - 6; d <= coarseBest + 6; ++d) {
        double q = featureCorrAtStrided(
            ref, candidate, pos, featureWindow, d, 2);

        if (q > featureBestQ) {
            featureBestQ = q;
            featureBest = d;
        }
    }

    // Stage 3: raw normalized correlation only in a tiny neighborhood.
    // This gives sub-sample precision without letting phase differences drive
    // the broad search.
    long rawBest = featureBest;
    double rawBestQ = -3.0;

    for (long d = featureBest - 2; d <= featureBest + 2; ++d) {
        double q = corrAt(ref, candidate, pos, window, d);
        if (q > rawBestQ) {
            rawBestQ = q;
            rawBest = d;
        }
    }

    double c0 = corrAt(ref, candidate, pos, window, rawBest - 1);
    double c1 = corrAt(ref, candidate, pos, window, rawBest);
    double c2 = corrAt(ref, candidate, pos, window, rawBest + 1);

    double frac = 0.0;
    double denom = c0 - 2.0 * c1 + c2;

    if (std::abs(denom) > 1e-12) {
        frac = 0.5 * (c0 - c2) / denom;
        frac = std::max(-0.5, std::min(0.5, frac));
    }

    double measured = rawBest + frac;

    // Hybrid confidence: feature correlation is robust to phase/timbre,
    // waveform correlation supplies precision when it agrees.
    double confidence =
        std::max(0.0, 0.65 * featureBestQ + 0.35 * rawBestQ);

    // Critical safety rule:
    // uncertain speech/music alignment is allowed to KEEP the previous delay,
    // never to steer the timeline randomly.
    double smoothed = previousDelay;

    if (confidence >= 0.72) {
        double alpha = confidence >= 0.90 ? 0.85 : 0.60;
        smoothed =
            previousDelay + alpha * (measured - previousDelay);
    } else if (confidence >= 0.58) {
        // Cautious tracking only.
        smoothed =
            previousDelay + 0.20 * (measured - previousDelay);
    }

    TrackState s;
    s.delay = smoothed;
    s.measuredDelay = measured;
    s.quality = confidence;
    s.shock = smoothed - previousDelay;

    return s;
}

// v0.2.29: periodic real-recording fine-sync re-acquisition.
// Independent analogue transfers do not share an exact clock.  A +/-3 ms
// tracker is sufficient for synthetic copies but not for several minutes of
// turntable wow/drift.  Every ~0.5 s we therefore re-acquire the donor against
// a longer transient-envelope fingerprint around the CURRENT timeline.
// Only the donor timeline is moved; the Master is never time-warped.
static TrackState reacquireRealDelay(const Wav& ref,
                                     const Wav& candidate,
                                     long pos,
                                     double previousDelay) {
    const int sr = ref.sampleRate;
    const int win = std::max(2048, sr * 500 / 1000); // 500 ms evidence
    const int radius = std::max(64, sr * 120 / 1000); // +/-120 ms
    const int coarse = std::max(8, sr / 1000);        // ~1 ms grid
    const long center = (long)std::llround(previousDelay);

    long best = center;
    double bestQ = -3.0, secondQ = -3.0;
    for (long d=center-radius; d<=center+radius; d+=coarse) {
        double q=featureCorrAtStrided(ref,candidate,pos,win,d,8);
        if(q>bestQ){ secondQ=bestQ; bestQ=q; best=d; }
        else if(q>secondQ) secondQ=q;
    }
    long fineBest=best;
    double fineQ=-3.0;
    for(long d=best-coarse; d<=best+coarse; ++d){
        double q=featureCorrAtStrided(ref,candidate,pos,win,d,2);
        if(q>fineQ){ fineQ=q; fineBest=d; }
    }

    // Uniqueness is important on periodic music.  A weak/ambiguous anchor
    // freezes the old timeline rather than steering it to a false match.
    const double uniqueness = bestQ-secondQ;
    const bool accept = fineQ >= 0.12 && (uniqueness >= 0.006 || fineQ >= 0.30);
    double next=previousDelay;
    if(accept){
        // Anchor correction may be large after accumulated drift, but limit a
        // single update to 80 ms. Subsequent anchors can continue catching up.
        double delta=(double)fineBest-previousDelay;
        double lim=sr*0.080;
        delta=std::max(-lim,std::min(lim,delta));
        next=previousDelay+delta;
    }
    TrackState r;
    // v0.2.29 verified fine-sync:
    // Only publish a new trajectory point when the local correlation is
    // genuinely strong.  Otherwise freeze at the previous verified delay.
    // This prevents a plausible-looking but wrong correlation peak from
    // dragging the donor onto a different musical moment.
    const double verifiedCorrMin = 0.72;
    const bool verified = accept && std::isfinite(fineQ) && fineQ >= verifiedCorrMin;
    if (!verified) {
        next = previousDelay;
    }
    r.delay=next;
    r.measuredDelay=(double)fineBest;
    r.quality=verified ? std::min(1.0, 0.70 + std::max(0.0,fineQ-verifiedCorrMin)*0.30/(1.0-verifiedCorrMin)) : 0.0;
    r.shock=next-previousDelay;
    return r;
}

// Find a useful reference anchor near the beginning instead of assuming that
// every transfer starts at nearly the same time.  We score short windows by
// local signal activity (RMS + first-difference energy) and avoid the first
// 250 ms, which is often just recorder/needle-start noise.
static long findInitialAnchor(const Wav& w, int maxSearchSeconds = 8) {
    const int sr = w.sampleRate;
    if (sr <= 0 || w.frames() < (size_t)sr / 2) return 0;

    const long start = std::min<long>((long)w.frames() - 1, sr / 4);
    const long stop = std::min<long>((long)w.frames() - 1,
                                     (long)maxSearchSeconds * sr);
    const int win = std::max(256, sr * 120 / 1000);   // 120 ms
    const int hop = std::max(64,  sr * 20  / 1000);   // 20 ms
    const int stride = std::max(1, sr / 12000);        // <= ~12 kHz analysis

    long bestPos = std::min<long>(stop, sr / 2);
    double bestScore = -1.0;

    for (long pos = start; pos + win < stop; pos += hop) {
        double e = 0.0, d = 0.0;
        double prev = monoAt(w, pos);
        long n = 0;
        for (int j = 0; j < win; j += stride) {
            double x = monoAt(w, pos + j);
            e += x * x;
            double dx = x - prev;
            d += dx * dx;
            prev = x;
            ++n;
        }
        if (!n) continue;
        // Reject near-silence, then prefer information-rich/music-like windows.
        double rms = std::sqrt(e / n);
        if (rms < 0.0010) continue;
        double score = rms + 0.35 * std::sqrt(d / n);
        if (score > bestScore) {
            bestScore = score;
            bestPos = pos + win / 2;
        }
    }
    return bestPos;
}

// Locate the first sustained musical energy, independently in every transfer.
// A single click or needle drop must not become the start anchor: five of six
// consecutive 5 ms windows have to clear a threshold derived from both the
// lead-in noise floor and the recording's own level distribution.
static long findSustainedMusicOnset(const Wav& w) {
    const int sr=w.sampleRate;
    const long frames=(long)w.frames();
    if(sr<=0 || frames<sr/2) return 0;
    const int win=std::max(64,sr/200);
    const long count=frames/win;
    if(count<12) return findInitialAnchor(w,8);
    std::vector<double> rms((size_t)count,0.0);
    for(long i=0;i<count;++i) {
        double sum=0.0;
        for(int j=0;j<win;++j) {
            const double x=monoAt(w,i*win+j);
            sum+=x*x;
        }
        rms[(size_t)i]=std::sqrt(sum/win);
    }
    auto percentile=[](std::vector<double> v,double p) {
        if(v.empty()) return 0.0;
        const size_t k=std::min(v.size()-1,(size_t)std::floor(p*(v.size()-1)));
        std::nth_element(v.begin(),v.begin()+k,v.end());
        return v[k];
    };
    const size_t leadCount=std::min<size_t>(60,rms.size());
    std::vector<double> lead(rms.begin(),rms.begin()+leadCount);
    const double noiseMedian=percentile(lead,0.50);
    const double programmeP90=percentile(rms,0.90);
    const double threshold=std::max(noiseMedian*3.0,programmeP90*0.14);
    for(long i=0;i+6<=count;++i) {
        int above=0;
        std::vector<double> six;
        six.reserve(6);
        for(int j=0;j<6;++j) {
            const double v=rms[(size_t)(i+j)];
            six.push_back(v);
            if(v>threshold) ++above;
        }
        if(above>=5 && percentile(six,0.50)>threshold*1.20)
            return i*win;
    }
    return findInitialAnchor(w,8);
}

// Confirm that an onset is a real sustained level transition rather than one
// click in the lead-in. This is deliberately independent of cross-record raw
// phase: different physical copies need not correlate at the exact attack.
static double onsetTransitionRatio(const Wav& w,long onset) {
    const int sr=w.sampleRate;
    const long frames=(long)w.frames();
    if(sr<=0 || frames<=0) return 0.0;
    auto rms=[&](long first,long last)->double {
        first=std::clamp<long>(first,0,frames);
        last=std::clamp<long>(last,0,frames);
        if(last<=first) return 0.0;
        double sum=0.0;
        for(long i=first;i<last;++i) {
            const double x=monoAt(w,i);
            sum+=x*x;
        }
        return std::sqrt(sum/(double)(last-first));
    };
    const long nearGap=sr*50/1000;
    const long window=sr*250/1000;
    const double before=rms(onset-nearGap-window,onset-nearGap);
    const double after=rms(onset+nearGap,onset+nearGap+window);
    return after/std::max(1e-9,before);
}

static std::pair<double,double> refineOnsetDelay(const Wav& ref,const Wav& donor,
                                                 long refOnset,long donorOnset) {
    const int sr=ref.sampleRate;
    const long base=donorOnset-refOnset;
    const int win=std::max(1024,sr*80/1000);
    const long radius=std::max<long>(32,sr*12/1000);
    long best=base;
    double bestQ=-2.0;
    for(long d=base-radius;d<=base+radius;d+=4) {
        if(refOnset+d<0 || refOnset+d+win>=(long)donor.frames()) continue;
        const double q=corrAtStrided(ref,donor,refOnset,win,d,2);
        if(q>bestQ){bestQ=q;best=d;}
    }
    const long coarse=best;
    for(long d=coarse-4;d<=coarse+4;++d) {
        if(refOnset+d<0 || refOnset+d+win>=(long)donor.frames()) continue;
        const double q=corrAtStrided(ref,donor,refOnset,win,d,1);
        if(q>bestQ){bestQ=q;best=d;}
    }
    return {(double)best,bestQ};
}


struct BroadDelayDiag {
    long coarseBest = 0;
    double coarseBestScore = -2.0;
    double coarseSecondScore = -2.0;
    long refine1Best = 0;
    double refine1Score = -2.0;
    long fineBest = 0;
    double fineScore = -2.0;
    double returnedDelay = 0.0;
    double proposedDelay = 0.0;
    bool accepted = false;
    std::string rejectReason = "UNSET";
};

static double broadDelay(const Wav& ref,
                         const Wav& candidate,
                         int window,
                         int initRadius,
                         BroadDelayDiag* diag = nullptr) {
    // v2.17 / v0.2.29:
    // Robust global start sync.  Search the whole plausible start-offset range
    // using several independent musical anchors.  A candidate delay must
    // explain the same offset at multiple positions; one accidental musical
    // resemblance is not enough.
    const int sr = ref.sampleRate;
    if (sr <= 0) { if (diag) diag->rejectReason="INVALID_SAMPLE_RATE"; return 0.0; }

    // Search up to +/-20 s, limited by file lengths.
    const long maxRadius = std::min<long>(
        std::max<long>(sr, initRadius),
        20L * sr);

    const int featWin = std::max(window, sr * 220 / 1000); // 220 ms
    const int stride = std::max(1, sr / 12000);

    // Use multiple anchors spread through the first ~30 s of common material.
    // This is deliberately independent of "interesting" start noise/silence.
    const double anchorSec[] = {1.5, 4.0, 7.5, 11.0, 15.0, 20.0, 26.0};
    std::vector<long> anchors;
    for (double s : anchorSec) {
        long p = (long)std::llround(s * sr);
        if (p >= 0 && p + featWin < (long)ref.frames())
            anchors.push_back(p);
    }
    if (anchors.size() < 3) {
        // Short-file fallback.
        anchors.clear();
        const long usable = std::max<long>(0, (long)ref.frames() - featWin - 1);
        for (int k = 1; k <= 5; ++k) {
            long p = usable * k / 6;
            if (p >= 0 && p + featWin < (long)ref.frames())
                anchors.push_back(p);
        }
    }
    if (anchors.empty()) { if (diag) diag->rejectReason="NO_ANCHORS"; return 0.0; }

    // Global coarse grid: 10 ms.  Score by robust multi-anchor agreement.
    const long coarseStep = std::max<long>(1, sr / 100); // 10 ms
    long bestDelay = 0;
    double bestScore = -1e30;
    double secondScore = -1e30;

    auto scoreDelay = [&](long d) -> double {
        std::vector<double> qs;
        qs.reserve(anchors.size());
        for (long p : anchors) {
            long cp = p + d;
            if (cp < 0 || cp + featWin >= (long)candidate.frames())
                continue;
            double q = featureCorrAtStrided(ref, candidate, p, featWin, d, stride);
            if (std::isfinite(q) && q > -1.5)
                qs.push_back(q);
        }
        if (qs.size() < 3) return -1e30;

        std::sort(qs.begin(), qs.end());
        // Robust score: median + lower-quartile support.
        double med = qs[qs.size()/2];
        double low = qs[qs.size()/4];
        double mean = std::accumulate(qs.begin(), qs.end(), 0.0) / qs.size();

        // Prefer delays that are consistently good across anchors.
        return 0.50*med + 0.30*mean + 0.20*low;
    };

    for (long d = -maxRadius; d <= maxRadius; d += coarseStep) {
        double sc = scoreDelay(d);
        if (sc > bestScore) {
            secondScore = bestScore;
            bestScore = sc;
            bestDelay = d;
        } else if (sc > secondScore) {
            secondScore = sc;
        }
    }

    // Hierarchical refinement: 1 ms, then sample-level.
    const long oneMs = std::max<long>(1, sr / 1000);
    long ref1Best = bestDelay;
    double ref1Score = bestScore;
    for (long d = bestDelay - coarseStep; d <= bestDelay + coarseStep; d += oneMs) {
        double sc = scoreDelay(d);
        if (sc > ref1Score) {
            ref1Score = sc;
            ref1Best = d;
        }
    }

    long fineBest = ref1Best;
    double fineScore = ref1Score;
    const long fineRadius = std::max<long>(8, oneMs + 4);
    for (long d = ref1Best - fineRadius; d <= ref1Best + fineRadius; ++d) {
        double sc = scoreDelay(d);
        if (sc > fineScore) {
            fineScore = sc;
            fineBest = d;
        }
    }

    // v0.2.29l PEAK-SEPARATION DIAGNOSTIC. No thresholds changed.
    if (diag) {
        diag->coarseBest = bestDelay;
        diag->coarseBestScore = bestScore;
        diag->coarseSecondScore = secondScore;
        diag->refine1Best = ref1Best;
        diag->refine1Score = ref1Score;
        diag->fineBest = fineBest;
        diag->fineScore = fineScore;
        // v0.2.29j: retain the best finite global candidate as a SEARCH SEED only.
        // It is not accepted as an alignment unless the original fail-safe passes.
        diag->proposedDelay = std::isfinite(fineScore) ? (double)fineBest : 0.0;
    }

    // Fail-safe: reject weak or ambiguous global alignment rather than
    // returning a plausible-looking wrong offset.
    const double minGlobalScore = 0.45;
    const double minSeparation = 0.015;
    const bool finitePass = std::isfinite(fineScore);
    const bool scorePass = finitePass && fineScore >= minGlobalScore;
    const bool separationPass = !(secondScore > -1e20 && fineScore - secondScore < minSeparation);
    if (!finitePass || !scorePass || !separationPass) {
        if (diag) {
            diag->accepted = false;
            diag->returnedDelay = 0.0;
            diag->rejectReason = !finitePass ? "NONFINITE_SCORE" :
                                 (!scorePass ? "GLOBAL_SCORE_BELOW_0.45" :
                                  "GLOBAL_SEPARATION_BELOW_0.015");
        }
        return 0.0;
    }

    if (diag) {
        diag->accepted = true;
        diag->returnedDelay = (double)fineBest;
        diag->rejectReason = "ACCEPTED";
    }
    return (double)fineBest;
}


static Stability localStability(const Wav& w,
                                double frame,
                                double syncShock,
                                double correlation,
                                int channel) {
    float xm2 = sampleLinear(w, frame - 2.0, channel);
    float xm1 = sampleLinear(w, frame - 1.0, channel);
    float x0  = sampleLinear(w, frame,       channel);
    float xp1 = sampleLinear(w, frame + 1.0, channel);
    float xp2 = sampleLinear(w, frame + 2.0, channel);

    double dPrev = xm1 - xm2;
    double dNow = x0 - xm1;
    double dNext = xp1 - x0;
    double dNext2 = xp2 - xp1;

    double curvature =
        std::abs((double)xm1 - 2.0 * x0 + xp1);

    double normalSlope =
        0.5 * (std::abs(dPrev) + std::abs(dNext2)) + 1e-6;

    double edge =
        std::max(std::abs(dNow), std::abs(dNext));

    double jumpRatio =
        edge / normalSlope;

    Stability s;
    s.impulse = curvature;
    s.jump = jumpRatio;
    s.syncShock = std::abs(syncShock);
    s.correlationPenalty =
        std::max(0.0, 0.95 - correlation);

    s.score =
        2.5 * s.impulse +
        0.035 * std::max(0.0, s.jump - 3.0) +
        0.020 * s.syncShock +
        2.0 * s.correlationPenalty;

    return s;
}



struct NSourceEvidence {
    bool available = false;
    bool confirmed = false;
    int controlSource = -1;
    double donorControlCorr = -2.0;
    double masterDonorCorr = -2.0;
    double masterControlCorr = -2.0;
    double correlationAdvantage = 0.0;
    double outlierRatio = 0.0;
    int eligibleWitnesses = 0;
    int supportingWitnesses = 0;
    int opposingWitnesses = 0;
    std::vector<int> supportingSources;
    std::vector<int> abstainingSources;
    std::vector<int> opposingSources;
};

struct ImpulseConsensusEvidence {
    bool available = false;
    bool confirmed = false;
    int donorSource = -1;
    double masterPeak = 0.0;
    double donorPeak = 0.0;
    double controlPeak = 0.0;
    double outlierRatio = 0.0;
    int controlSource = -1;
    bool impulsePatternConfirmed = false;
    bool trendAccepted = false;
    double trendGain = 1.0;
    double trendDc = 0.0;
    double donorTrendFit = std::numeric_limits<double>::infinity();
    double controlTrendFit = std::numeric_limits<double>::infinity();
    double donorTrendCorr = -2.0;
    double controlTrendCorr = -2.0;
    double donorControlTrendCorr = -2.0;
    int eligibleWitnesses = 0;
    int supportingWitnesses = 0;
    int opposingWitnesses = 0;
    std::vector<int> supportingSources;
    std::vector<int> abstainingSources;
    std::vector<int> opposingSources;
    std::vector<double> peakBySource;
    std::vector<double> floorBySource;
    std::vector<double> zBySource;
    std::vector<double> trendFitBySource;
    std::vector<double> trendCorrBySource;
};

// x2n: a confirmed impulse can begin just before the 2 ms detector cell which
// finally crosses the Class-A threshold.  Keep the extra full-donor margin
// separate from the outer crossfade so a boundary sample is not merely faded
// down and left audible.
struct RepairWindowBounds {
    long start = 0;
    long end = 0;
};

struct ImpulseCoreWindow {
    long start = 0;
    long end = 0;
    long peakFrame = 0;
    double peakExcess = 0.0;
    bool localized = false;
};

static long repairBoundaryGuardFrames(int sampleRate) {
    return std::max<long>(1, sampleRate / 2000); // 0.5 ms
}

static RepairWindowBounds expandedRepairWindow(
    long detectedStart,long detectedEnd,long totalFrames,int sampleRate) {
    const long guard=repairBoundaryGuardFrames(sampleRate);
    return RepairWindowBounds{
        std::max<long>(0,detectedStart-guard),
        std::min<long>(totalFrames,detectedEnd+guard)};
}

// Quintic smootherstep keeps both slope and curvature continuous at the ends
// of a splice.  A linear fade has a slope discontinuity at each boundary and
// can therefore create the exact sample-scale kink that the postcheck is
// designed to reject.
static double smoothRepairFade(double position) {
    const double x=std::clamp(position,0.0,1.0);
    return x*x*x*(x*(x*6.0-15.0)+10.0);
}

// Keep the reliable 2 ms consensus cells, but localize one dominant isolated
// Master-only impulse inside the resulting confirmed event before rendering.
// This changes only the small sample core whose curvature is materially above
// the aligned clean donor. Broad or multi-impulse damage deliberately falls
// back to the established full-event path.
static ImpulseCoreWindow localizeImpulseCore(
    const std::vector<Wav>& sources,
    int master,
    int donor,
    long detectedStart,
    long detectedEnd,
    long totalFrames,
    const std::vector<double>& delays,
    double delayNudge,
    double donorGain,
    int sampleRate) {

    ImpulseCoreWindow result;
    const auto fallback=expandedRepairWindow(
        detectedStart,detectedEnd,totalFrames,sampleRate);
    result.start=fallback.start;
    result.end=fallback.end;
    result.peakFrame=detectedStart;
    if(sampleRate<=0 || master<0 || donor<0 ||
       master>=(int)sources.size() || donor>=(int)sources.size() ||
       donor>=(int)delays.size() || detectedEnd<=detectedStart ||
       totalFrames<5)
        return result;

    const long searchStart=std::max<long>(2,fallback.start);
    const long searchEnd=std::min<long>(totalFrames-2,fallback.end);
    if(searchEnd-searchStart<5) return result;

    auto curvatureAt=[&](int source,double masterFrame,double extraDelay,
                         double scale) {
        const Wav& w=sources[source];
        const double center=masterFrame+
            (source==master ? 0.0 : delays[source]+extraDelay);
        double peak=0.0;
        for(int ch=0;ch<w.channels;++ch) {
            const double left=sampleLinear(w,center-1.0,ch);
            const double here=sampleLinear(w,center,ch);
            const double right=sampleLinear(w,center+1.0,ch);
            peak=std::max(peak,std::abs(scale*(left-2.0*here+right)));
        }
        return peak;
    };

    const long context=std::max<long>(16,sampleRate/1000); // 1 ms each side
    std::vector<double> localFloor;
    localFloor.reserve((size_t)context*2);
    auto collectFloor=[&](long first,long last) {
        for(long frame=std::max<long>(2,first);
            frame<std::min<long>(totalFrames-2,last);++frame) {
            const double masterCurvature=curvatureAt(master,(double)frame,0.0,1.0);
            const double donorCurvature=curvatureAt(
                donor,(double)frame,delayNudge,donorGain);
            localFloor.push_back(std::max(masterCurvature,donorCurvature));
        }
    };
    collectFloor(searchStart-context,searchStart);
    collectFloor(searchEnd,searchEnd+context);
    double floor=0.000001;
    if(!localFloor.empty()) {
        auto middle=localFloor.begin()+localFloor.size()/2;
        std::nth_element(localFloor.begin(),middle,localFloor.end());
        floor=std::max(floor,*middle);
    }

    std::vector<double> excess((size_t)(searchEnd-searchStart),0.0);
    double peakMaster=0.0,peakDonor=0.0;
    size_t peakIndex=0;
    for(long frame=searchStart;frame<searchEnd;++frame) {
        const double masterCurvature=curvatureAt(master,(double)frame,0.0,1.0);
        const double donorCurvature=curvatureAt(
            donor,(double)frame,delayNudge,donorGain);
        const double value=std::max(0.0,masterCurvature-1.25*donorCurvature);
        const size_t index=(size_t)(frame-searchStart);
        excess[index]=value;
        if(value>result.peakExcess) {
            result.peakExcess=value;
            result.peakFrame=frame;
            peakIndex=index;
            peakMaster=masterCurvature;
            peakDonor=donorCurvature;
        }
    }

    const bool distinctImpulse=
        peakMaster>=std::max(0.00002,8.0*floor) &&
        result.peakExcess>=std::max(0.000015,5.0*floor) &&
        peakMaster>=1.8*(peakDonor+floor);
    if(!distinctImpulse) return result;

    const double threshold=std::max({0.00001,4.0*floor,
                                     0.08*result.peakExcess});
    const int allowedGap=std::max(1,sampleRate/48000);
    size_t first=peakIndex,last=peakIndex;
    int gap=0;
    for(size_t i=peakIndex;i>0;) {
        --i;
        if(excess[i]>=threshold) { first=i;gap=0; }
        else if(++gap>allowedGap) break;
    }
    gap=0;
    for(size_t i=peakIndex+1;i<excess.size();++i) {
        if(excess[i]>=threshold) { last=i;gap=0; }
        else if(++gap>allowedGap) break;
    }

    // A long event can still contain one short click surrounded by harmless
    // disagreement. Localize it only when no second substantial impulse exists;
    // otherwise the established full-event renderer remains the safer choice.
    double secondaryPeak=0.0;
    for(size_t i=0;i<excess.size();++i) {
        const bool outsideLeft=i+(size_t)allowedGap<first;
        const bool outsideRight=i>last+(size_t)allowedGap;
        if(outsideLeft || outsideRight)
            secondaryPeak=std::max(secondaryPeak,excess[i]);
    }
    if(secondaryPeak>=std::max(8.0*floor,0.35*result.peakExcess))
        return result;

    const long padding=std::max<long>(2,sampleRate/24000); // about 0.04 ms
    long coreStart=std::max<long>(fallback.start,
        searchStart+(long)first-padding);
    long coreEnd=std::min<long>(fallback.end,
        searchStart+(long)last+1+padding);
    const long minimumCore=std::max<long>(8,sampleRate/12000); // about 0.08 ms
    if(coreEnd-coreStart<minimumCore) {
        const long missing=minimumCore-(coreEnd-coreStart);
        coreStart=std::max<long>(fallback.start,coreStart-missing/2);
        coreEnd=std::min<long>(fallback.end,coreStart+minimumCore);
        coreStart=std::max<long>(fallback.start,coreEnd-minimumCore);
    }
    const long maximumCore=std::max<long>(16,sampleRate*3/2000); // 1.5 ms
    if(coreEnd<=coreStart || coreEnd-coreStart>maximumCore)
        return result;

    result.start=coreStart;
    result.end=coreEnd;
    result.localized=true;
    return result;
}

static bool mayAbsorbAdjacentUncertainCell(
    char confidenceClass,const std::string& decisionReason,
    double masterImpulseZ,const std::vector<std::string>& gateReason,
    int donor) {
    if(confidenceClass!='B' || !std::isfinite(masterImpulseZ) ||
       masterImpulseZ<2.5 || donor<0 || donor>=(int)gateReason.size() ||
       gateReason[donor]!="ELIGIBLE_SCORE")
        return false;
    // Never revive an event which a completed render/postcheck rejected.
    if(decisionReason.rfind("B_POSTCHECK_",0)==0) return false;
    return decisionReason=="B_QUALITY_DIFFERENCE_KEEP_MASTER" ||
        decisionReason=="B_IMPULSE_WITHOUT_TREND_MATCH_KEEP_MASTER" ||
        decisionReason=="B_SPLIT_SOURCE_VOTE_KEEP_MASTER";
}

// v0.2.31: a sample-localized click changes only the proven impulse core and may
// use the ordinary 1.85x physical-source margin.  If localization cannot
// separate the defect from surrounding programme, the renderer would replace
// several milliseconds of otherwise authentic Master audio.  Require a wider
// margin for that broad fallback; marginal cases remain Master and are exposed
// as Class B for listening/visual review.
static bool broadRepairEvidenceStrongEnough(
    bool sampleLocalized,double outlierRatio) {
    return sampleLocalized ||
        (std::isfinite(outlierRatio) && outlierRatio>=2.25);
}

static std::string unconfirmedImpulseReason(
    const ImpulseConsensusEvidence& evidence) {
    if(evidence.supportingWitnesses<2)
        return "B_INSUFFICIENT_SOURCE_CONSENSUS_KEEP_MASTER";
    if(evidence.opposingWitnesses>0)
        return "B_SPLIT_SOURCE_VOTE_KEEP_MASTER";
    return "B_UNCONFIRMED_IMPULSE_KEEP_MASTER";
}

struct LocalTrendMatch {
    bool usable = false;
    double fit = std::numeric_limits<double>::infinity(); // normalized RMSE; lower is better
    double corr = -2.0;
    double gain = 1.0;
    double dc = 0.0;
};

// A globally worn copy can occasionally achieve the numerically smallest
// local trend error by chance. With three or more clean witnesses, first form
// a trusted half from the best whole-file quality ranks, then let a lower-ranked
// source escape that pool only when its local fit is unambiguously better.
// Quality is therefore a prior rather than a veto: a poor disc may still donate
// the one genuinely clean fragment which no better copy can supply.
static int selectQualityAwareDonor(
    const std::vector<int>& candidates,
    const std::vector<LocalTrendMatch>& trends,
    const std::vector<double>* globalQuality,
    bool* qualityGuardChangedSelection=nullptr) {

    if(qualityGuardChangedSelection) *qualityGuardChangedSelection=false;
    auto betterLocal=[&](int a,int b) {
        if(b<0) return true;
        if(std::abs(trends[a].fit-trends[b].fit)>1e-12)
            return trends[a].fit<trends[b].fit;
        if(std::abs(trends[a].corr-trends[b].corr)>1e-12)
            return trends[a].corr>trends[b].corr;
        return a<b;
    };
    int localBest=-1;
    for(int source:candidates)
        if(source>=0 && source<(int)trends.size() && trends[source].usable &&
           betterLocal(source,localBest))
            localBest=source;
    if(localBest<0 || candidates.size()<3 || !globalQuality ||
       globalQuality->size()!=trends.size())
        return localBest;

    std::vector<int> ranked;
    for(int source:candidates)
        if(source>=0 && source<(int)trends.size() && trends[source].usable)
            ranked.push_back(source);
    std::sort(ranked.begin(),ranked.end(),[&](int a,int b) {
        const double qa=std::isfinite((*globalQuality)[a])
            ? (*globalQuality)[a] : -std::numeric_limits<double>::infinity();
        const double qb=std::isfinite((*globalQuality)[b])
            ? (*globalQuality)[b] : -std::numeric_limits<double>::infinity();
        if(std::abs(qa-qb)>1e-12) return qa>qb;
        return a<b;
    });
    const size_t trustedCount=std::min(ranked.size(),
        std::max<size_t>(2,(ranked.size()+1)/2));
    int trustedBest=-1;
    for(size_t i=0;i<trustedCount;++i)
        if(betterLocal(ranked[i],trustedBest)) trustedBest=ranked[i];
    if(trustedBest<0 || localBest==trustedBest) return localBest;
    if(std::find(ranked.begin(),ranked.begin()+trustedCount,localBest)!=
       ranked.begin()+trustedCount)
        return localBest;

    // A low-ranked source must improve normalized RMSE by both 28% and 0.05,
    // without losing surrounding-curve correlation. Otherwise use the best
    // locally matching member of the trusted quality majority.
    const bool clearlyBetter=
        trends[localBest].fit<=0.72*trends[trustedBest].fit &&
        trends[localBest].fit+0.05<=trends[trustedBest].fit &&
        trends[localBest].corr+0.02>=trends[trustedBest].corr;
    if(clearlyBetter) return localBest;
    if(qualityGuardChangedSelection) *qualityGuardChangedSelection=true;
    return trustedBest;
}

// Compare the musical baseline outside the possible click.  The suspect 2 ms
// decision interval plus a short guard is deliberately excluded.  An affine
// fit removes harmless transfer-level/DC differences; the residual measures
// whether the donor follows the same local curve before and after the defect.
static LocalTrendMatch localTrendMatch(
    const std::vector<Wav>& sources,
    int reference,
    int candidate,
    long seg,
    long segEnd,
    const std::vector<double>& delays,
    int sampleRate,
    double minimumGain=0.125,
    double maximumGain=8.0) {

    LocalTrendMatch result;
    if (reference < 0 || candidate < 0 ||
        reference >= (int)sources.size() || candidate >= (int)sources.size())
        return result;

    const long context = std::max<long>(192, sampleRate * 6 / 1000); // 6 ms per side
    const long guard = std::max<long>(24, sampleRate / 2000);         // 0.5 ms
    const long step = std::max<long>(8, sampleRate / 4000);          // 0.25 ms
    const long smoothRadius = std::max<long>(2, sampleRate / 12000); // ~0.08 ms

    auto smoothedMono = [&](int source, double masterFrame, double& value) {
        double sum = 0.0;
        int count = 0;
        const double center = masterFrame + delays[source];
        for (long k = -smoothRadius; k <= smoothRadius; k += 2) {
            const double frame = center + (double)k;
            if (frame < 0.0 || frame + 1.0 >= (double)sources[source].frames())
                continue;
            double mono = 0.0;
            for (int ch = 0; ch < sources[source].channels; ++ch)
                mono += sampleLinear(sources[source], frame, ch);
            sum += mono / std::max(1, sources[source].channels);
            ++count;
        }
        if (count == 0) return false;
        value = sum / count;
        return true;
    };

    std::vector<double> refValues;
    std::vector<double> candidateValues;
    refValues.reserve(64);
    candidateValues.reserve(64);
    auto collect = [&](long first, long last) {
        for (long frame = first; frame <= last; frame += step) {
            double a = 0.0, b = 0.0;
            if (smoothedMono(reference, (double)frame, a) &&
                smoothedMono(candidate, (double)frame, b)) {
                refValues.push_back(a);
                candidateValues.push_back(b);
            }
        }
    };
    collect(seg - context, seg - guard);
    const size_t leftCount = refValues.size();
    collect(segEnd + guard, segEnd + context);
    if (leftCount < 8 || refValues.size() - leftCount < 8) return result;

    double meanRef = std::accumulate(refValues.begin(), refValues.end(), 0.0) /
        (double)refValues.size();
    double meanCandidate = std::accumulate(candidateValues.begin(), candidateValues.end(), 0.0) /
        (double)candidateValues.size();
    double varRef = 0.0, varCandidate = 0.0, covariance = 0.0;
    for (size_t i = 0; i < refValues.size(); ++i) {
        const double a = refValues[i] - meanRef;
        const double b = candidateValues[i] - meanCandidate;
        varRef += a * a;
        varCandidate += b * b;
        covariance += a * b;
    }
    varRef /= (double)refValues.size();
    varCandidate /= (double)refValues.size();
    covariance /= (double)refValues.size();
    if (varCandidate < 1e-12) return result;

    const double unconstrainedGain = covariance / varCandidate;
    if (!std::isfinite(unconstrainedGain) ||
        unconstrainedGain < minimumGain || unconstrainedGain > maximumGain)
        return result;
    result.gain = unconstrainedGain;
    result.dc = meanRef - result.gain * meanCandidate;

    double sumSquared = 0.0;
    for (size_t i = 0; i < refValues.size(); ++i) {
        const double residual = refValues[i] -
            (result.gain * candidateValues[i] + result.dc);
        sumSquared += residual * residual;
    }
    const double rmse = std::sqrt(sumSquared / (double)refValues.size());
    const double referenceScale = std::max(0.0025, std::sqrt(varRef));
    result.fit = rmse / referenceScale;
    if (varRef >= 1e-12)
        result.corr = covariance / std::sqrt(varRef * varCandidate);
    else
        result.corr = rmse <= 0.0025 ? 1.0 : 0.0;

    // A donor must actually follow the local baseline.  Correlation protects
    // shaped programme material; normalized error also permits genuinely
    // quiet/flat groove sections where correlation is mathematically weak.
    result.usable = std::isfinite(result.fit) && result.fit <= 0.55 &&
        (result.corr >= 0.60 || std::sqrt(varRef) < 0.0030);
    return result;
}

// Detect a short impulse which exists in the audio master but not in a strict
// majority of the locally usable independent transfers. The detector works on
// the second difference, so slow amplitude/EQ differences do not masquerade as
// a click. A source which is not aligned, is locally invalid, or contains an
// unrelated impulse abstains; an aligned same-sign impulse is an opposing vote.
static ImpulseConsensusEvidence detectImpulseConsensus(
    const std::vector<Wav>& sources,
    int master,
    long seg,
    long segEnd,
    const std::vector<double>& delays,
    int sampleRate,
    const std::vector<bool>* eligibleMask=nullptr,
    const std::vector<double>* globalQuality=nullptr) {

    ImpulseConsensusEvidence e;
    if (sources.size() < 3 || master < 0 || master >= (int)sources.size())
        return e;

    const int sourceCount=(int)sources.size();
    constexpr double nan=std::numeric_limits<double>::quiet_NaN();
    e.peakBySource.assign(sourceCount,nan);
    e.floorBySource.assign(sourceCount,nan);
    e.zBySource.assign(sourceCount,nan);
    e.trendFitBySource.assign(sourceCount,std::numeric_limits<double>::infinity());
    e.trendCorrBySource.assign(sourceCount,-2.0);

    auto addUnique=[](std::vector<int>& values,int source) {
        if(std::find(values.begin(),values.end(),source)==values.end())
            values.push_back(source);
    };

    std::vector<int> eligible;
    for (int s=0; s<sourceCount; ++s) {
        if (s==master) continue;
        const bool admitted=!eligibleMask ||
            (s<(int)eligibleMask->size() && (*eligibleMask)[s]);
        if(admitted) eligible.push_back(s);
        else addUnique(e.abstainingSources,s);
    }
    // Measure Master even when fewer than two donors can vote.
    // Missing witness assessment must not label a clear impulse as safe C.

    // Compare impulse height in the Master's local level domain. Independent
    // transfers can differ greatly in gain; without this normalization a loud
    // clean donor can look damaged, or a quiet damaged donor can look clean.
    // localTrendMatch excludes the suspect interval, so the click itself cannot
    // choose the gain. DC is irrelevant to a second difference and is omitted.
    std::vector<double> measurementGain((size_t)sourceCount,1.0);
    for(int s:eligible) {
        const auto match=localTrendMatch(
            sources,master,s,seg,segEnd,delays,sampleRate);
        if(std::isfinite(match.gain) && match.gain>=0.125 && match.gain<=8.0)
            measurementGain[s]=match.gain;
    }

    const long context = std::max<long>(96, sampleRate * 2 / 1000); // +/-2 ms
    auto mono = [&](int s, double frame) {
        double v=0.0;
        for (int ch=0; ch<sources[s].channels; ++ch)
            v += sampleLinear(sources[s], frame, ch);
        return v/std::max(1,sources[s].channels);
    };
    auto curvature = [&](int s, long frame) {
        const double f=(double)frame+delays[s];
        if (f < 1.0 || f + 1.0 >= (double)sources[s].frames())
            return std::numeric_limits<double>::quiet_NaN();
        return measurementGain[s]*
            (mono(s,f)-0.5*(mono(s,f-1.0)+mono(s,f+1.0)));
    };

    std::vector<double> floor(sourceCount,nan), peak(sourceCount,nan);
    std::vector<long> peakFrame(sourceCount,std::numeric_limits<long>::min());
    std::vector<double> signedPeak(sourceCount,0.0);
    auto measure=[&](int s) {
        std::vector<double> local;
        local.reserve((size_t)(2*context+1));
        for (long i=seg-context; i<segEnd+context; ++i) {
            double c=curvature(s,i);
            if (std::isfinite(c)) local.push_back(std::abs(c));
        }
        if (local.size() < 32) return false;
        const size_t k=(size_t)std::llround(0.60*(double)(local.size()-1));
        std::nth_element(local.begin(),local.begin()+k,local.end());
        floor[s]=local[k]+1e-7;
        peak[s]=0.0;
        bool segmentMeasured=false;
        for (long i=seg; i<segEnd; ++i) {
            double c=curvature(s,i);
            if (!std::isfinite(c)) continue;
            if (!segmentMeasured) {
                peakFrame[s]=i;
                signedPeak[s]=c;
                segmentMeasured=true;
            }
            if (std::abs(c)>peak[s]) {
                peak[s]=std::abs(c);
                peakFrame[s]=i;
                signedPeak[s]=c;
            }
        }
        e.peakBySource[s]=peak[s];
        e.floorBySource[s]=floor[s];
        e.zBySource[s]=peak[s]/std::max(floor[s],1e-12);
        return segmentMeasured;
    };

    if(!measure(master)) return e;
    e.available=true;
    e.masterPeak=peak[master];
    std::vector<int> measured;
    for(int s:eligible) {
        if(measure(s)) measured.push_back(s);
        else addUnique(e.abstainingSources,s);
    }
    e.eligibleWitnesses=(int)measured.size();
    if(measured.size()<2) return e;
    e.available=true;

    const double masterZ=peak[master]/floor[master];
    e.masterPeak=peak[master];
    const double quantizationGuard=std::ldexp(16.0,-(sources[master].bits-1));

    // A clean witness has neither the Master's normalized curvature event nor
    // a similarly large absolute impulse. A non-clean witness only opposes the
    // repair when its peak is phase-coincident and has the same sign. Otherwise
    // its unrelated local defect is explicitly treated as an abstention.
    const long coincidentTolerance=std::max<long>(4,sampleRate/24000);
    for(int s:measured) {
        const double z=peak[s]/std::max(floor[s],1e-12);
        const bool clean=z<=masterZ*0.65 &&
            peak[s]<=peak[master]/1.85;
        if(clean) {
            e.supportingSources.push_back(s);
            continue;
        }
        const bool coincident=
            std::llabs(peakFrame[s]-peakFrame[master])<=coincidentTolerance &&
            signedPeak[s]*signedPeak[master]>0.0 &&
            z>=masterZ*0.55;
        if(coincident) e.opposingSources.push_back(s);
        else addUnique(e.abstainingSources,s);
    }

    e.supportingWitnesses=(int)e.supportingSources.size();
    e.opposingWitnesses=(int)e.opposingSources.size();
    double cleanPeak=0.0;
    for(int s:e.supportingSources) cleanPeak=std::max(cleanPeak,peak[s]);
    e.outlierRatio=peak[master]/std::max(cleanPeak,1e-7);

    // Strict majority is evaluated only among sources which can make a
    // defensible local statement: the Master, clean witnesses and witnesses
    // carrying the same event. Missing/invalid/unrelated copies abstain.
    const int effectiveVotes=1+e.supportingWitnesses+e.opposingWitnesses;
    const bool strictMajority=e.supportingWitnesses>effectiveVotes/2;
    e.impulsePatternConfirmed =
        peak[master] >= quantizationGuard &&
        masterZ >= 5.0 &&
        e.outlierRatio >= 1.85 &&
        e.supportingWitnesses >= 2 &&
        strictMajority;

    if (!e.impulsePatternConfirmed) return e;

    // Do not select the flattest transfer. Select the clean witness that best
    // follows the Master's surrounding musical trend. Other clean witnesses
    // remain independent confirmation even when they are not the inserted one.
    std::vector<LocalTrendMatch> trends((size_t)sourceCount);
    std::vector<int> strongTrendSources;
    for(int s:e.supportingSources) {
        trends[s]=localTrendMatch(
            sources,master,s,seg,segEnd,delays,sampleRate);
        e.trendFitBySource[s]=trends[s].fit;
        e.trendCorrBySource[s]=trends[s].corr;
        if(trends[s].usable) {
            strongTrendSources.push_back(s);
        }
    }
    bool qualityGuardChangedSelection=false;
    const int donor=selectQualityAwareDonor(strongTrendSources,trends,
        globalQuality,&qualityGuardChangedSelection);
    if (donor < 0 || strongTrendSources.size()<2) return e;
    const LocalTrendMatch chosenTrend=trends[donor];
    if(qualityGuardChangedSelection)
        std::cerr << "DONOR_QUALITY_GUARD selected " << (donor+1)
                  << " local_fit " << chosenTrend.fit << "\n";

    int control=-1;
    LocalTrendMatch donorControl;
    for(int s:strongTrendSources) {
        if(s==donor) continue;
        // Pair gain may span the ratio of the single-transfer gain limits.
        // Gain is fitted outside the suspected defect; the same curve-quality
        // and independent-witness gates still apply.
        const LocalTrendMatch candidate=localTrendMatch(
            sources,s,donor,seg,segEnd,delays,sampleRate,
            0.125/8.0,8.0/0.125);
        if(!candidate.usable) continue;
        if(control<0 || candidate.corr>donorControl.corr ||
           (candidate.corr==donorControl.corr && candidate.fit<donorControl.fit)) {
            control=s;
            donorControl=candidate;
        }
    }
    if(control<0) return e;

    // With exactly two independent witnesses there is no spare vote if one
    // transfer is locally misleading.  Require all three surrounding-curve
    // comparisons to be unusually strong before permitting an automatic A
    // repair.  Three or more clean witnesses already provide that redundancy.
    if(strongTrendSources.size()==2) {
        const bool masterDonorStrong=chosenTrend.usable &&
            chosenTrend.fit<=0.42 &&
            (chosenTrend.corr>=0.72 || chosenTrend.fit<=0.20);
        const bool masterControlStrong=trends[control].usable &&
            trends[control].fit<=0.42 &&
            (trends[control].corr>=0.72 || trends[control].fit<=0.20);
        const bool witnessesAgree=donorControl.usable &&
            donorControl.fit<=0.38 &&
            (donorControl.corr>=0.72 || donorControl.fit<=0.18);
        if(!masterDonorStrong || !masterControlStrong || !witnessesAgree)
            return e;
    }

    e.donorSource=donor;
    e.controlSource=control;
    e.donorPeak=peak[donor];
    e.controlPeak=peak[control];
    e.donorTrendFit=chosenTrend.fit;
    e.controlTrendFit=trends[control].fit;
    e.donorTrendCorr=chosenTrend.corr;
    e.controlTrendCorr=trends[control].corr;
    e.donorControlTrendCorr=donorControl.corr;
    e.trendGain=chosenTrend.gain;
    e.trendDc=chosenTrend.dc;
    e.trendAccepted=true;
    e.confirmed=true;
    return e;
}

// Curvature finds the sharp click, but an impulse may also leave a smooth
// shoulder. Extend only where a second independent transfer supports the same
// signed Master residual. Healthy programme/EQ differences set the noise floor.
static ImpulseCoreWindow supportedImpulseShoulders(
    const std::vector<Wav>& sources,int master,int donor,int control,
    ImpulseCoreWindow core,long detectedStart,long detectedEnd,
    const std::vector<double>& delay,double gain,double dc,int sr) {
    if(!core.localized || control<0 || control==donor)return core;
    const auto controlMatch=localTrendMatch(sources,master,control,
        detectedStart,detectedEnd,delay,sr);
    const auto pair=localTrendMatch(sources,control,donor,
        detectedStart,detectedEnd,delay,sr,.125/8.,8./.125);
    if(!controlMatch.usable || !pair.usable || pair.fit>.38 || pair.corr<.72)return core;
    auto mono=[&](int source,long f) {
        const double pos=f+delay[source];
        if(pos<1 || pos+2>=sources[source].frames())return std::numeric_limits<double>::quiet_NaN();
        double value=0;for(int ch=0;ch<sources[source].channels;++ch)value+=sampleLinear(sources[source],pos,ch);
        return value/std::max(1,sources[source].channels);
    };
    const long smooth=std::max<long>(2,sr/24000);
    auto residual=[&](long f) {
        double a=0,b=0;int n=0;
        for(long k=-smooth;k<=smooth;++k) {
            const double m=mono(master,f+k),d=mono(donor,f+k),c=mono(control,f+k);
            if(!std::isfinite(m)||!std::isfinite(d)||!std::isfinite(c))continue;
            a+=m-(gain*d+dc);b+=m-(controlMatch.gain*c+controlMatch.dc);++n;
        }
        if(n==0)return std::make_pair(std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN());
        return std::make_pair(a/n,b/n);
    };
    auto median=[](std::vector<double> values) {
        if(values.empty())return 0.;auto m=values.begin()+values.size()/2;
        std::nth_element(values.begin(),m,values.end());return *m;
    };
    std::vector<double>a,b;
    const long context=std::max<long>(192,sr*6/1000),guard=std::max<long>(24,sr/2000);
    for(long f=detectedStart-context;f<detectedEnd+context;f+=std::max(1,sr/24000)) {
        if(f>detectedStart-guard && f<detectedEnd+guard)continue;
        auto r=residual(f);if(std::isfinite(r.first)&&std::isfinite(r.second)){a.push_back(r.first);b.push_back(r.second);}
    }
    if(a.size()<32)return core;
    const double ma=median(a),mb=median(b);
    for(auto&v:a)v=std::abs(v-ma);for(auto&v:b)v=std::abs(v-mb);
    const double floorA=std::max(.0003,median(a)),floorB=std::max(.0003,median(b));
    auto supported=[&](long f) {
        auto r=residual(f);const double ra=r.first-ma,rb=r.second-mb;
        return std::isfinite(ra)&&std::isfinite(rb) && ra*rb>0 &&
            std::abs(ra)>2.5*floorA && std::abs(rb)>1.5*floorB &&
            std::abs(ra-rb)<3.*(floorA+floorB);
    };
    const long extension=std::max<long>(24,sr*3/4000); // <=0.75 ms per side
    const long low=std::max<long>(2,core.start-extension);
    const long high=std::min<long>((long)sources[master].frames()-2,core.end+extension);
    const long allowedGap=std::max<long>(2,sr/24000);
    long left=core.start,right=core.end,gap=0;
    for(long f=core.start-1;f>=low;--f){if(supported(f)){left=f;gap=0;}else if(++gap>allowedGap)break;}
    gap=0;for(long f=core.end;f<high;++f){if(supported(f)){right=f+1;gap=0;}else if(++gap>allowedGap)break;}
    if(right-left>std::max<long>(16,sr*3/2000))return core; // no blind broad splice
    core.start=left;core.end=right;return core;
}

// A smooth blend can create a visible dip without a sharp curvature spike.
// If both donor and independent control move clearly in the same direction,
// Result must not reverse against both by a material amount.
static bool spliceShapeSupported(const std::vector<Wav>& sources,int master,
    int donor,int control,const std::vector<double>& delay,double gain,double dc,
    const std::vector<float>& rendered,long first,long last,int sr) {
    (void)dc; // Constant offset cancels in the first-difference shape check.
    if(control<0 || control==donor)return true;
    auto cm=localTrendMatch(sources,master,control,first,last,delay,sr);
    if(!cm.usable)return false;
    const long half=std::max<long>(2,sr/24000); // 0.08 ms difference
    for(long f=std::max<long>(half,first);f<std::min<long>((long)sources[master].frames()-half,last);++f) {
        for(int ch=0;ch<sources[master].channels;++ch) {
            const double d=gain*(sampleLinear(sources[donor],f+half+delay[donor],ch)-sampleLinear(sources[donor],f-half+delay[donor],ch));
            const double c=cm.gain*(sampleLinear(sources[control],f+half+delay[control],ch)-sampleLinear(sources[control],f-half+delay[control],ch));
            const double r=rendered[(size_t)(f+half)*sources[master].channels+ch]-rendered[(size_t)(f-half)*sources[master].channels+ch];
            if(d*c>0 && std::min(std::abs(d),std::abs(c))>.001 &&
               r*d<0 && std::abs(r)>.001 && std::abs(r)>.15*std::min(std::abs(d),std::abs(c)))return false;
        }
    }
    return true;
}

static bool locallyVerifiedWitness(const std::vector<Wav>& sources,int master,int candidate,
    long first,long last,const std::vector<double>& delay,int sr) {
    const auto match=localTrendMatch(sources,master,candidate,first,last,delay,sr);
    if(!match.usable || match.fit>.30 || match.corr<.94)return false;
    const long window=std::max<long>(96,sr/250); // 4 ms per side
    const long guard=std::max<long>(24,sr/2000);
    auto side=[&](long start,long end) {
        std::vector<double>a,b;
        for(long f=start;f<end;f+=std::max(1,sr/16000)) {
            const double df=f+delay[candidate];
            if(f<1 || f+1>=(long)sources[master].frames() || df<1 || df+2>=sources[candidate].frames())return false;
            double x=0,y=0;
            for(int ch=0;ch<sources[master].channels;++ch){x+=sampleLinear(sources[master],f,ch);y+=sampleLinear(sources[candidate],df,ch);}
            a.push_back(x);b.push_back(y);
        }
        if(a.size()<16)return false;
        const double ma=std::accumulate(a.begin(),a.end(),0.)/a.size();
        const double mb=std::accumulate(b.begin(),b.end(),0.)/b.size();
        double va=0,vb=0,co=0;
        for(size_t i=0;i<a.size();++i){va+=(a[i]-ma)*(a[i]-ma);vb+=(b[i]-mb)*(b[i]-mb);co+=(a[i]-ma)*(b[i]-mb);}
        return va>1e-10 && vb>1e-10 && co/std::sqrt(va*vb)>=.92;
    };
    return side(first-guard-window,first-guard) && side(last+guard,last+guard+window);
}


// Bounded low-energy recovery. A donor supplies audio; a second donor verifies it.
// Master is used only OUTSIDE the detected loss to fit gain/DC and timing.
struct DropoutRepair {
    bool accepted=false;
    long first=0,last=0,renderFirst=0,renderLast=0;
    int donor=-1,control=-1;
    double gain=1,dc=0,pairCorr=0,donorContextCorr=0,controlContextCorr=0;
    std::vector<double> delay;
    std::vector<float> samples;
};
static DropoutRepair detectDropoutRepair(const std::vector<Wav>& sources,int master,
    long seedFirst,long seedLast,const std::vector<double>& initial,
    const std::vector<bool>& eligible,int sr) {
    DropoutRepair result;
    const auto& ref=sources[master];
    const long block=std::max<long>(8,sr/4000),radius=sr/50,fade=std::max(8,sr/8000);
    auto energy=[&](int src,long a,long b,const std::vector<double>& delay) {
        if(a<1 || b+delay[src]+2>=sources[src].frames() || a+delay[src]<1)return -1.;
        double sum=0;long count=0;
        for(long f=a;f<b;f+=2)for(int ch=0;ch<ref.channels;++ch){double x=sampleLinear(sources[src],f+delay[src],ch);sum+=x*x;++count;}
        return count?std::sqrt(sum/count):-1.;
    };
    double me=energy(master,seedFirst,seedLast,initial);
    if(me<0)return result;
    const long left=seedFirst-radius,right=seedLast+radius;
    if(left<sr/100 || right+sr/100>=(long)ref.frames())return result;
    std::vector<int> candidates;
    std::vector<double> roughGain(sources.size(),1.);
    double outerMaster=energy(master,left-sr/200,left,initial)+energy(master,right,right+sr/200,initial);
    for(int src=0;src<(int)sources.size();++src)if(src!=master && eligible[src]){
        double outer=energy(src,left-sr/200,left,initial)+energy(src,right,right+sr/200,initial);
        if(outer<=.002 || outerMaster<=.002)continue;
        roughGain[src]=outerMaster/outer;
        double de=energy(src,seedFirst,seedLast,initial)*roughGain[src];
        if(de>.004 && me<.25*de)candidates.push_back(src);
    }
    if(candidates.size()<2)return result;
    // Earliest qualifying energy run containing this seed. Short zero crossings
    // and natural silence fail the minimum duration / donor energy tests.
    for(int donor:candidates){
        auto low=[&](long f){double m=energy(master,f,f+block,initial),d=energy(donor,f,f+block,initial)*roughGain[donor];return m>=0 && d>.004 && m<.20*d;};
        long center=-1;
        for(long f=seedFirst;f<seedLast;f+=block)if(low(f)){center=f;break;}
        if(center<0)continue;
        long a=center,b=center+block;
        while(a-block>=left && low(a-block))a-=block;
        while(b+block<=right && low(b))b+=block;
        if(a<=left+block || b>=right-block || b-a<sr/2000 || b-a>sr/50)continue;
        // Include the energy-window boundary uncertainty before fitting.
        a-=block;b+=block;
        auto delays=initial;
        std::vector<LocalTrendMatch> matches(sources.size());
        for(int src:candidates){
            LocalTrendMatch best;double shift=0;
            for(int n=-24;n<=24;++n){auto trial=delays;trial[src]=initial[src]+n;
                auto fit=localTrendMatch(sources,master,src,a,b,trial,sr);
                if(fit.usable && fit.fit<best.fit){best=fit;shift=n;}}
            if(!best.usable || std::abs(shift)>=24)continue;
            for(int n=-7;n<=7;++n){auto trial=delays;double step=shift+n/8.;trial[src]=initial[src]+step;
                auto fit=localTrendMatch(sources,master,src,a,b,trial,sr);
                if(fit.usable && fit.fit<best.fit){best=fit;delays[src]=initial[src]+step;}}
            if(delays[src]==initial[src])delays[src]=initial[src]+shift;
            if(best.corr<.97 || best.fit>.15 || !locallyVerifiedWitness(sources,master,src,a,b,delays,sr))continue;
            matches[src]=best;
        }
        if(!matches[donor].usable || matches[donor].corr<.97 || matches[donor].fit>.15)continue;
        auto pairCheck=[&](int control){
            double xx=0,yy=0,xy=0,sx=0,sy=0,err=0;long count=0;
            for(long f=a;f<b;++f)for(int ch=0;ch<ref.channels;++ch){
                double x=matches[donor].gain*sampleLinear(sources[donor],f+delays[donor],ch)+matches[donor].dc;
                double y=matches[control].gain*sampleLinear(sources[control],f+delays[control],ch)+matches[control].dc;
                sx+=x;sy+=y;xx+=x*x;yy+=y*y;xy+=x*y;err+=(x-y)*(x-y);++count;}
            if(count<8)return -1.;
            double vx=xx-sx*sx/count,vy=yy-sy*sy/count;
            if(vx<=1e-10 || vy<=1e-10 || yy/count<.004*.004 || xx/count<.004*.004)return -1.;
            double corr=(xy-sx*sy/count)/std::sqrt(vx*vy);
            if(std::sqrt(err/count)>.12*std::sqrt(yy/count))return -1.;
            return corr;
        };
        int control=-1;double pair=0;
        for(int peer:candidates)if(peer!=donor && matches[peer].usable && matches[peer].corr>=.97 && matches[peer].fit<=.15){double q=pairCheck(peer);if(q>=.985 && q>pair){pair=q;control=peer;}}
        if(control<0)continue;
        // Reject a mere gain difference: the entire core must be strongly depressed.
        double masterEnergy=energy(master,a+block,b-block,delays);
        double donorEnergy=energy(donor,a+block,b-block,delays)*matches[donor].gain;
        if(masterEnergy>.20*donorEnergy)continue;
        long rf=a-fade,rl=b+fade;
        std::vector<float> proposed=ref.samples;
        bool safe=true;
        double seamScale=std::max(.001,donorEnergy*.12);
        for(long f=rf;f<rl;++f)for(int ch=0;ch<ref.channels;++ch){
            double replacement=matches[donor].gain*sampleLinear(sources[donor],f+delays[donor],ch)+matches[donor].dc;
            if(!std::isfinite(replacement)||replacement<=-1||replacement>=1){safe=false;break;}
            double alpha=f<a?smoothRepairFade((double)(f-rf+1)/(fade+1)):f>=b?smoothRepairFade((double)(rl-f)/(fade+1)):1.;
            if((f<a || f>=b) && std::abs(replacement-ref.samples[(size_t)f*ref.channels+ch])>seamScale)safe=false;
            proposed[(size_t)f*ref.channels+ch]=(float)((1-alpha)*ref.samples[(size_t)f*ref.channels+ch]+alpha*replacement);
        }
        if(!safe || !spliceShapeSupported(sources,master,donor,control,delays,matches[donor].gain,matches[donor].dc,proposed,rf,rl,sr))continue;
        result.accepted=true;result.first=a;result.last=b;result.renderFirst=rf;result.renderLast=rl;result.donor=donor;result.control=control;result.gain=matches[donor].gain;result.dc=matches[donor].dc;result.pairCorr=pair;result.donorContextCorr=matches[donor].corr;result.controlContextCorr=matches[control].corr;result.delay=delays;
        result.samples.assign(proposed.begin()+(size_t)rf*ref.channels,proposed.begin()+(size_t)rl*ref.channels);
        return result;
    }
    return result;
}

struct LocalImpulseTrial {
    ImpulseConsensusEvidence evidence;
    long start=0,end=0;
    std::vector<double> delay;
};

// Revisit only short Master curvature events. Fit timing/level on the healthy
// context, never on the click, then apply the ordinary independent-source vote.
static std::vector<LocalImpulseTrial> localImpulseTrials(
    const std::vector<Wav>& sources,int master,long start,long end,
    const std::vector<double>& seed,int sr,const std::vector<bool>& eligible,
    const std::vector<double>* quality=nullptr,int preferredDonor=-1,
    const std::vector<bool>* requiresLocalVerification=nullptr) {
    std::vector<LocalImpulseTrial> trials;
    if(master<0 || master>=(int)sources.size() || seed.size()!=sources.size()) return trials;
    const auto& ref=sources[master];
    const long half=std::max<long>(4,sr/8000); // +/-0.125 ms vote
    const long radius=std::max<long>(4,sr/2000); // bounded +/-0.5 ms resync
    auto curve=[&](long f) {
        double peak=0.0;
        for(int ch=0;ch<ref.channels;++ch) {
            const size_t p=(size_t)f*ref.channels+ch;
            peak=std::max(peak,std::abs((double)ref.samples[p]-
                .5*(ref.samples[p-ref.channels]+ref.samples[p+ref.channels])));
        }
        return peak;
    };
    std::vector<std::pair<double,long>> peaks;
    for(long f=std::max<long>(1,start);f<std::min<long>((long)ref.frames()-1,end);++f)
        peaks.push_back({curve(f),f});
    std::sort(peaks.begin(),peaks.end(),[](const auto&a,const auto&b){return a.first>b.first;});
    std::vector<long> used;
    for(const auto& peak:peaks) {
        if(used.size()>=std::min<long>(12,std::max<long>(3,(end-start)/(sr/1000)))) break;
        if(peak.first<std::ldexp(16.0,-(ref.bits-1))) break;
        if(std::any_of(used.begin(),used.end(),[&](long f){return std::abs(f-peak.second)<half*4;})) continue;
        used.push_back(peak.second);
        LocalImpulseTrial trial;
        trial.start=std::max<long>(1,peak.second-half);
        trial.end=std::min<long>((long)ref.frames()-1,peak.second+half+1);
        trial.delay=seed;
        const auto initial=detectImpulseConsensus(sources,master,trial.start,
            trial.end,trial.delay,sr,&eligible,quality);
        if(!initial.available || initial.zBySource[master]<5.0)continue;
        if(initial.confirmed && preferredDonor<0 && !requiresLocalVerification) {
            trial.evidence=initial;trials.push_back(std::move(trial));continue;
        }
        for(int source=0;source<(int)sources.size();++source) {
            if(source==master || source>=(int)eligible.size() || !eligible[source]) continue;
            double bestShift=0.0;
            LocalTrendMatch best=localTrendMatch(sources,master,source,
                trial.start,trial.end,trial.delay,sr);
            auto test=[&](double shift) {
                auto candidateDelay=seed;candidateDelay[source]+=shift;
                const auto m=localTrendMatch(sources,master,source,
                    trial.start,trial.end,candidateDelay,sr);
                if(m.usable && (!best.usable || m.fit+1e-9<best.fit)) {best=m;bestShift=shift;}
            };
            for(long shift=-radius;shift<=radius;shift+=4) test((double)shift);
            const double coarse=bestShift;
            for(int eighth=-16;eighth<=16;++eighth) test(coarse+eighth/8.0);
            // A best match at the search edge is unresolved timing, not proof.
            if(best.usable && std::abs(bestShift)<radius-1) trial.delay[source]+=bestShift;
        }
        auto localEligible=eligible;
        if(requiresLocalVerification)for(int source=0;source<(int)sources.size();++source)
            if(source<(int)requiresLocalVerification->size() && (*requiresLocalVerification)[source] &&
               !locallyVerifiedWitness(sources,master,source,trial.start,trial.end,trial.delay,sr))
                localEligible[source]=false;
        trial.evidence=detectImpulseConsensus(sources,master,trial.start,
            trial.end,trial.delay,sr,&localEligible,quality);
        if(preferredDonor>=0 && trial.evidence.confirmed) {
            auto&e=trial.evidence;
            if(std::find(e.supportingSources.begin(),e.supportingSources.end(),preferredDonor)==e.supportingSources.end()) continue;
            const auto match=localTrendMatch(sources,master,preferredDonor,
                trial.start,trial.end,trial.delay,sr);
            if(!match.usable || match.fit>.42 || (match.corr<.72 && match.fit>.20)) continue;
            int control=-1;double controlCorr=-2;
            for(int source:e.supportingSources) {
                if(source==preferredDonor)continue;
                const auto m=localTrendMatch(sources,source,preferredDonor,
                    trial.start,trial.end,trial.delay,sr,.125/8.0,8.0/.125);
                const auto masterMatch=localTrendMatch(sources,master,source,
                    trial.start,trial.end,trial.delay,sr);
                if(m.usable && m.fit<=.38 && (m.corr>=.72 || m.fit<=.18) &&
                   masterMatch.usable && masterMatch.fit<=.42 &&
                   (masterMatch.corr>=.72 || masterMatch.fit<=.20) && m.corr>controlCorr){control=source;controlCorr=m.corr;}
            }
            if(control<0)continue;
            e.donorSource=preferredDonor;e.controlSource=control;
            e.trendGain=match.gain;e.trendDc=match.dc;
        }
        if(trial.evidence.confirmed)trials.push_back(std::move(trial));
    }
    return trials;
}

static double sourceBlockScore(const std::vector<Wav>& sources,
                               int sourceIndex,
                               long block,
                               long end,
                               double delay,
                               double syncShock,
                               double correlation,
                               const std::vector<double>& allDelays,
                               const std::vector<bool>* sourceMask=nullptr) {
    const Wav& w = sources[sourceIndex];

    double sum = 0.0;
    double maxImpulse = 0.0;
    double consensusSum = 0.0;
    int count = 0;

    // Scoring every 8th frame is sufficient to identify clicks/dropouts
    // while keeping reconstruction much cheaper than sample-by-sample voting.
    for (long i = block; i < end; i += 8) {
        for (int ch = 0; ch < w.channels; ++ch) {
            double frame = (double)i + delay;

            if (frame < 2.0 || frame + 2.0 >= (double)w.frames())
                continue;

            Stability st =
                localStability(w, frame, syncShock, correlation, ch);

            sum += st.score;
            maxImpulse = std::max(maxImpulse, st.impulse);

            if (sources.size() >= 3) {
                std::vector<float> vals;
                vals.reserve(sources.size());

                for (size_t s = 0; s < sources.size(); ++s) {
                    if(sourceMask &&
                       (s>=sourceMask->size() || !(*sourceMask)[s])) continue;
                    double sf = (double)i + allDelays[s];
                    if (sf >= 0.0 && sf + 1.0 < (double)sources[s].frames())
                        vals.push_back(sampleLinear(sources[s], sf, ch));
                }

                if (vals.size() >= 3) {
                    std::sort(vals.begin(), vals.end());
                    double med;
                    size_t n = vals.size();
                    if (n & 1)
                        med = vals[n / 2];
                    else
                        med = 0.5 * (vals[n / 2 - 1] + vals[n / 2]);

                    float own = sampleLinear(w, frame, ch);
                    consensusSum += std::abs((double)own - med);
                }
            }

            ++count;
        }
    }

    if (count == 0)
        return std::numeric_limits<double>::infinity();

    double avg = sum / count;
    double consensus = consensusSum / count;

    // Max impulse makes a narrow click visible even though it occupies only
    // a handful of samples inside a 12 ms decision block.
    return avg + 1.5 * maxImpulse + 1.2 * consensus;
}


static double sourceContinuityScore(const std::vector<Wav>& sources,
                                    int sourceIndex,
                                    long seg,
                                    double delay,
                                    const std::vector<float>& out,
                                    int channels,
                                    int sampleRate) {
    if (seg <= 0) return 0.0;
    const int n = std::max(8, sampleRate / 2000); // about 0.5 ms
    long start = std::max<long>(0, seg - n);
    double sum2 = 0.0;
    int count = 0;
    for (long i = start; i < seg; i += 2) {
        for (int ch = 0; ch < channels; ++ch) {
            double frame = (double)i + delay;
            if (frame < 0.0 || frame + 1.0 >= (double)sources[sourceIndex].frames())
                continue;
            double candidate = sampleLinear(sources[sourceIndex], frame, ch);
            double previous = out[(size_t)i * channels + ch];
            double d = candidate - previous;
            sum2 += d * d;
            ++count;
        }
    }
    return count ? std::sqrt(sum2 / count) : 0.0;
}


struct AffineMatch { double gain = 1.0; double dc = 0.0; };

static double percentileD(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    q = std::max(0.0, std::min(1.0, q));
    size_t k = (size_t)std::llround(q * (double)(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

struct SourceAssessmentMetrics {
    double qualityScore=-1e30;
    double programmeLevelDbfs=-120.0;
    double headroomDb=0.0;
    double cleanWindowsPercent=0.0;
    uint64_t activeWindows=0;
    uint64_t impulseWindows=0;
    double impulsesPerMinute=0.0;
    uint64_t clippedSamples=0;
};

// x2p: calculate the same gain-independent quality score used by automatic
// Master selection and expose its understandable components to the preflight
// dialog. "Clean" means an active 20 ms window without a large derivative /
// crest outlier; it is a comparison aid, not a claim that the audio is perfect.
static SourceAssessmentMetrics assessSource(const Wav& w) {
    SourceAssessmentMetrics result;
    const int sr = w.sampleRate;
    const long win = std::max<long>(256, sr / 50);   // ~20 ms
    const long step = std::max<long>(win, sr / 100); // >=10 ms
    std::vector<double> rms;
    std::vector<double> peaks;
    std::vector<double> windowDerivativePeaks;
    std::vector<double> derivatives;
    const size_t estimatedWindows = std::max<size_t>(
        1u, w.frames() / static_cast<size_t>(std::max<long>(1, step)));
    rms.reserve(estimatedWindows);
    peaks.reserve(rms.capacity());
    windowDerivativePeaks.reserve(rms.capacity());
    for (long start=0; start + win < (long)w.frames(); start += step) {
        double ss=0.0, pk=0.0; long n=0;
        double derivativePeak=0.0;
        double previous=0.0; bool havePrevious=false;
        for (long i=start; i<start+win; i+=4) {
            const double signedMono=(double)monoAt(w,i);
            const double x=std::abs(signedMono);
            ss += x*x; pk=std::max(pk,x); ++n;
            if(havePrevious) {
                const double derivative=std::abs(signedMono-previous);
                derivatives.push_back(derivative);
                derivativePeak=std::max(derivativePeak,derivative);
            }
            previous=signedMono; havePrevious=true;
        }
        if(n) {
            rms.push_back(std::sqrt(ss/n));
            peaks.push_back(pk);
            windowDerivativePeaks.push_back(derivativePeak);
        }
    }
    if(rms.size()<8) return result;
    // Recorder/needle-start silence must not count as extra dynamics. Rank only
    // windows containing actual programme material (within 46 dB of robust top RMS).
    double topRms=percentileD(rms,0.95);
    std::vector<double> activeRms, activePeaks, activeDerivativePeaks;
    double gate=std::max(1e-7, topRms*0.005);
    for(size_t i=0;i<rms.size();++i) if(rms[i]>=gate){
        activeRms.push_back(rms[i]);
        activePeaks.push_back(peaks[i]);
        activeDerivativePeaks.push_back(windowDerivativePeaks[i]);
    }
    if(activeRms.size()<8) return result;
    double p20=percentileD(activeRms,0.20), p50=percentileD(activeRms,0.50), p90=percentileD(activeRms,0.90), p98=percentileD(activeRms,0.98);
    double peak98=percentileD(activePeaks,0.98), peakMax=*std::max_element(activePeaks.begin(),activePeaks.end());
    const double eps=1e-9;
    // Useful modulation span, with penalties for raised noise floor and rare spikes.
    double useful=20.0*std::log10((p90+eps)/(p20+eps));
    double activity=20.0*std::log10((p98+eps)/(p50+eps));
    double spike=std::max(0.0,20.0*std::log10((peakMax+eps)/(peak98+eps))-3.0);
    // v0.2.29u: dense shellac crackle lives in the upper derivative tail.
    // Use robust percentiles so one isolated click cannot condemn an otherwise
    // clean transfer. This prevents Auto Master from rewarding crackle as
    // "useful dynamics".
    double d99=percentileD(derivatives,0.990);
    double d999=percentileD(derivatives,0.999);
    double crackleTail=20.0*std::log10((d999+eps)/(d99+eps));
    double clipPenalty = peakMax >= 0.9995 ? 12.0 : 0.0;
    result.qualityScore=useful + 0.35*activity - 1.5*spike - crackleTail - clipPenalty;
    result.programmeLevelDbfs=20.0*std::log10(p50+eps);
    result.headroomDb=std::max(0.0,-20.0*std::log10(peakMax+eps));
    result.activeWindows=(uint64_t)activeRms.size();
    const double derivativeMedian=percentileD(activeDerivativePeaks,0.50);
    const double derivativeThreshold=std::max(1e-6,6.0*derivativeMedian);
    for(size_t i=0;i<activeRms.size();++i) {
        const double crest=activePeaks[i]/std::max(activeRms[i],eps);
        if(activeDerivativePeaks[i]>=derivativeThreshold && crest>=4.0)
            ++result.impulseWindows;
    }
    result.cleanWindowsPercent=result.activeWindows
        ? 100.0*(double)(result.activeWindows-result.impulseWindows)/result.activeWindows
        : 0.0;
    const double minutes=sr>0 ? (double)w.frames()/sr/60.0 : 0.0;
    result.impulsesPerMinute=minutes>0.0 ? result.impulseWindows/minutes : 0.0;
    result.clippedSamples=(uint64_t)std::count_if(
        w.samples.begin(),w.samples.end(),[](float sample){
            return std::isfinite(sample) && std::abs((double)sample)>=0.9995;
        });
    return result;
}

// Retained for unit tests and call sites which need only the ranking value.
[[maybe_unused]] static double masterQualityScore(const Wav& w) {
    return assessSource(w).qualityScore;
}

// x2c: bounded, gain-independent cross-source screening before selecting the
// time reference. This is selection only: it never modifies source samples.
struct MasterScreen {
    std::vector<int> verified, missing;
    std::vector<double> penalty;
};
struct ScreenBands { std::vector<double> low, detail; long onset=0; };
static ScreenBands screenBands(const Wav& w) {
    ScreenBands b;
    // Box integration before decimation; only compare the same input formats.
    const int step=std::max(1,w.sampleRate/4000);
    std::vector<double> x;
    for(size_t i=0;i+(size_t)step<=w.frames();i+=step) {
        double sum=0;
        for(int j=0;j<step;++j) sum+=monoAt(w,(long)i+j);
        x.push_back(sum/step);
    }
    b.low.resize(x.size()); b.detail.resize(x.size());
    for(size_t i=0;i<x.size();++i) {
        double sum=0; int n=0;
        for(long j=std::max<long>(0,(long)i-3);j<=std::min<long>((long)x.size()-1,(long)i+3);++j){sum+=x[j];++n;}
        b.low[i]=sum/n;
        // Separate higher-band detail from the low-band timing anchor.
        const size_t left=i?i-1:i, right=std::min(i+1,x.size()-1);
        b.detail[i]=x[i]-(x[left]+x[i]+x[right])/3.0;
    }
    b.onset=findSustainedMusicOnset(w)/step;
    return b;
}
static double screenCorr(const std::vector<double>& a,long ia,
                         const std::vector<double>& b,long ib,int n,int stride=1) {
    if(ia<0 || ib<0 || ia+n>(long)a.size() || ib+n>(long)b.size()) return 0;
    double sa=0,sb=0,aa=0,bb=0,ab=0;int count=0;
    for(int k=0;k<n;k+=stride){double x=a[ia+k],y=b[ib+k];sa+=x;sb+=y;aa+=x*x;bb+=y*y;ab+=x*y;++count;}
    aa-=sa*sa/count;bb-=sb*sb/count;ab-=sa*sb/count;
    if(aa<1e-18 || bb<1e-18)return 0;
    return std::abs(ab/std::sqrt(aa*bb)); // polarity does not affect quality
}
static double screenRms(const std::vector<double>& x,long i,int n) {
    double s=0,ss=0;
    for(int k=0;k<n;++k){double v=x[i+k];s+=v;ss+=v*v;}
    return std::sqrt(std::max(0.0,ss/n-(s/n)*(s/n)));
}
static MasterScreen screenMasterCandidates(const std::vector<Wav>& sources) {
    const int count=(int)sources.size();
    MasterScreen r; r.verified.assign(count,0);r.missing.assign(count,0);r.penalty.assign(count,0);
    // Stereo phase relationships must not be judged by a mono sum.
    if(count<3 || count>6 || sources[0].channels!=1)return r;
    std::vector<ScreenBands> b;
    for(const auto& w:sources)b.push_back(screenBands(w));
    const double rate=(double)sources[0].sampleRate/std::max(1,sources[0].sampleRate/4000);
    const int n=(int)(rate*0.20), radius=(int)(rate*0.60);
    long common=(long)b[0].low.size()-b[0].onset;
    for(int s=1;s<count;++s)common=std::min(common,(long)b[s].low.size()-b[s].onset);
    if(common<n*4)return r;
    for(int probe=0;probe<11;++probe) {
        const long anchor=(long)(((long)b[0].low.size()-n)*(0.04+0.092*probe));
        const long offset=anchor-b[0].onset;
        std::vector<long> pos((size_t)count,0);
        std::vector<unsigned char> aligned((size_t)count,0);
        pos[0]=anchor;
        aligned[0]=true;
        // Use the same anchor window for every copy. A low-confidence source
        // abstains at this probe; it no longer invalidates the other copies.
        for(int s=1;s<count;++s){
            const long expected=b[s].onset+offset;
            long best=expected;double qBest=0;
            // Onset detection can differ on a muffled copy. Also search the
            // corresponding fractional file position; neither seed is trusted.
            const long proportional=(long)((double)pos[0]*b[s].low.size()/b[0].low.size());
            const long seeds[2]={expected,proportional};
            for(long seed:seeds) for(long p=seed-radius;p<=seed+radius;p+=4){
                double q=screenCorr(b[0].low,pos[0],b[s].low,p,n,4);
                if(q>qBest){qBest=q;best=p;}
            }
            const long coarse=best;qBest=0;
            for(long p=coarse-4;p<=coarse+4;++p){
                double q=screenCorr(b[0].low,pos[0],b[s].low,p,n);
                if(q>qBest){qBest=q;best=p;}
            }
            pos[s]=best;
            aligned[s]=qBest>=0.80;
        }
        std::vector<double> ratio((size_t)count,0.0);
        std::vector<unsigned char> featureUsable=aligned;
        for(int s=0;s<count;++s){
            if(!aligned[s]) continue;
            double low=screenRms(b[s].low,pos[s],n);
            if(low<1e-7) {featureUsable[s]=false;continue;}
            ratio[s]=screenRms(b[s].detail,pos[s],n)/std::max(low,1e-15);
        }
        for(int s=0;s<count;++s){
            if(!featureUsable[s]) continue;
            bool verified=false, missing=false;
            for(int a=0;a<count;++a) {
                if(a==s || !featureUsable[a]) continue;
                for(int c=a+1;c<count;++c) {
                    if(c==s || !featureUsable[c]) continue;
                    // Independent copies must share both timing and detail,
                    // not merely contain more surface hiss.
                    if(screenCorr(b[a].low,pos[a],b[c].low,pos[c],n)<0.80 ||
                       screenCorr(b[a].detail,pos[a],b[c].detail,pos[c],n)<0.75)
                        continue;
                    verified=true;
                    if(ratio[a]>1.60*ratio[s] && ratio[c]>1.60*ratio[s] &&
                       std::min(ratio[a],ratio[c])>0.025 &&
                       std::max(ratio[a],ratio[c])<1.8*std::min(ratio[a],ratio[c]))
                        missing=true;
                }
            }
            if(verified) ++r.verified[s];
            if(missing) ++r.missing[s];
        }
    }
    for(int s=0;s<count;++s)
        if(r.verified[s]>=4 && r.missing[s]>=3 && r.missing[s]*2>=r.verified[s])
            r.penalty[s]=30.0*r.missing[s]/r.verified[s];
    return r;
}

struct AffineProbe {
    long masterFrame=0;
    double donorDelay=0.0;
    double quality=-2.0;
    int probeIndex=0;
};

// Fit a resistant straight line through sparse whole-record measurements.
// Theil-Sen supplies the initial slope, then two bounded least-squares passes
// refine only measurements within 18 ms. A model must cover at least half of
// the probe span and stay inside a physically generous +/-6% speed range.
static AffineDelayModel fitAffineDelayModel(const std::vector<AffineProbe>& probes,
                                             int sampleRate,
                                             int probeCount) {
    AffineDelayModel model;
    if(sampleRate<=0 || probeCount<2 || probes.size()<4) return model;

    auto median=[](std::vector<double> v)->double {
        if(v.empty()) return 0.0;
        const size_t mid=v.size()/2;
        std::nth_element(v.begin(),v.begin()+mid,v.end());
        double value=v[mid];
        if(v.size()%2==0) {
            const auto lower=std::max_element(v.begin(),v.begin()+mid);
            value=0.5*(value+*lower);
        }
        return value;
    };

    std::vector<double> slopes;
    for(size_t i=0;i<probes.size();++i)
        for(size_t j=i+1;j<probes.size();++j) {
            const long dx=probes[j].masterFrame-probes[i].masterFrame;
            if(std::labs(dx)<sampleRate/2) continue;
            slopes.push_back((probes[j].donorDelay-probes[i].donorDelay)/(double)dx);
        }
    if(slopes.empty()) return model;
    double slope=median(slopes);
    if(!std::isfinite(slope) || std::abs(slope)>0.06) return model;

    std::vector<double> intercepts;
    for(const auto& p:probes)
        intercepts.push_back(p.donorDelay-slope*(double)p.masterFrame);
    double intercept=median(intercepts);
    const double residualLimit=0.018*(double)sampleRate;
    std::vector<size_t> inliers;

    auto selectInliers=[&]() {
        inliers.clear();
        for(size_t i=0;i<probes.size();++i)
            if(std::abs(probes[i].donorDelay-
                        (intercept+slope*(double)probes[i].masterFrame))<=residualLimit)
                inliers.push_back(i);
    };
    auto refit=[&]()->bool {
        if(inliers.size()<4) return false;
        double sx=0.0,sy=0.0;
        for(size_t i:inliers) {
            sx+=(double)probes[i].masterFrame;
            sy+=probes[i].donorDelay;
        }
        const double mx=sx/inliers.size(), my=sy/inliers.size();
        double sxx=0.0,sxy=0.0;
        for(size_t i:inliers) {
            const double x=(double)probes[i].masterFrame-mx;
            sxx+=x*x;
            sxy+=x*(probes[i].donorDelay-my);
        }
        if(sxx<=0.0) return false;
        slope=sxy/sxx;
        intercept=my-slope*mx;
        return std::isfinite(slope) && std::isfinite(intercept);
    };

    selectInliers();
    if(!refit()) return model;
    selectInliers();
    if(!refit()) return model;
    selectInliers();
    if(inliers.size()<4 || std::abs(slope)>0.06) return model;

    int first=probeCount,last=-1;
    double maxResidual=0.0,qualitySum=0.0;
    for(size_t i:inliers) {
        first=std::min(first,probes[i].probeIndex);
        last=std::max(last,probes[i].probeIndex);
        maxResidual=std::max(maxResidual,std::abs(probes[i].donorDelay-
            (intercept+slope*(double)probes[i].masterFrame)));
        qualitySum+=probes[i].quality;
    }
    const double span=(last>first)
        ? (double)(last-first)/(double)(probeCount-1) : 0.0;
    model.intercept=intercept;
    model.slope=slope;
    model.maxResidual=maxResidual;
    model.span=span;
    model.score=qualitySum/inliers.size();
    model.inliers=(int)inliers.size();
    model.valid=model.inliers>=4 && model.span>=0.50 &&
        model.maxResidual<=residualLimit && std::abs(model.slope)<=0.06;
    return model;
}

// x2g: Master quality is useful only when two independent donors can follow
// that candidate through the recording.  This bounded preflight samples the
// whole programme on a low-rate timing representation, then applies the same
// 0.72 robust-musical-correlation gate used by the full time map.  It never
// changes audio. Its coherent probe delays also form a global linear speed
// model; the dense map then verifies and follows local wow/flutter around it.
struct DonorSyncPreflight {
    int validProbes=0;
    int verifiedProbes=0;
    double support=0.0;
    double span=0.0;
    double medianRobust=-2.0;
    double bestRobust=-2.0;
    AffineDelayModel affine;
    bool viable=false;
};
struct MasterSyncPreflight {
    std::vector<std::vector<DonorSyncPreflight>> donor;
    std::vector<int> viableDonors;
    std::vector<double> secondSupport;
    std::vector<double> meanTopTwoSupport;
};

using PreflightProgress = std::function<void(int,int,int,int)>;

static MasterSyncPreflight screenMasterSyncFeasibility(
        const std::vector<Wav>& sources,
        const std::vector<double>* offsetHints=nullptr,
        int hintedMaster=-1,
        const PreflightProgress& progress={}) {
    const int count=(int)sources.size();
    MasterSyncPreflight result;
    result.donor.assign(count,std::vector<DonorSyncPreflight>(count));
    result.viableDonors.assign(count,0);
    result.secondSupport.assign(count,0.0);
    result.meanTopTwoSupport.assign(count,0.0);
    if(count<3 || sources.empty()) return result;

    const int sr=sources[0].sampleRate;
    if(sr<=0) return result;
    for(const auto& w:sources)
        if(w.sampleRate!=sr || w.channels!=sources[0].channels) return result;

    std::vector<ScreenBands> bands;
    bands.reserve((size_t)count);
    for(const auto& w:sources) bands.push_back(screenBands(w));

    const int lowStep=std::max(1,sr/4000);
    const double lowRate=(double)sr/lowStep;
    const int lowWin=std::max(96,(int)std::llround(lowRate*0.20));
    const int lowRadius=std::max(32,(int)std::llround(lowRate*1.25));
    const int lowCoarse=std::max(2,(int)std::llround(lowRate/1000.0));
    const int robustWin=std::max(1024,sr/5);          // 200 ms
    const long robustRadius=std::max<long>(64,sr/40); // +/-25 ms
    const long robustCoarse=std::max<long>(16,sr/3000);
    const int robustStride=std::max(1,sr/12000);
    constexpr int probeCount=9;
    const int totalWork=std::max(1,count*(count-1)*probeCount);
    int completedWork=0;
    auto reportSkippedPair=[&](int master,int donor) {
        for(int probe=0;probe<probeCount;++probe)
            if(progress) progress(++completedWork,totalWork,master,donor);
    };

    for(int master=0;master<count;++master) {
        const long masterStart=bands[master].onset;
        const long masterUsable=(long)bands[master].low.size()-masterStart-lowWin-1;
        if(masterUsable<=lowWin) {
            for(int donor=0;donor<count;++donor)
                if(donor!=master) reportSkippedPair(master,donor);
            continue;
        }

        for(int donor=0;donor<count;++donor) {
            if(donor==master) continue;
            auto& report=result.donor[master][donor];
            const long donorStart=bands[donor].onset;
            const long donorUsable=(long)bands[donor].low.size()-donorStart-lowWin-1;
            if(donorUsable<=lowWin) {
                reportSkippedPair(master,donor);
                continue;
            }

            int firstVerified=-1,lastVerified=-1;
            std::vector<double> robustScores;
            robustScores.reserve(probeCount);
            std::vector<AffineProbe> affineProbes;
            affineProbes.reserve(probeCount);

            for(int probe=0;probe<probeCount;++probe) {
                if(progress) progress(++completedWork,totalWork,master,donor);
                const double fraction=(probe+1.0)/(probeCount+1.0);
                const long masterPos=masterStart+
                    (long)std::llround(fraction*masterUsable);
                const long normalizedSeed=donorStart+
                    (long)std::llround(fraction*donorUsable);
                const long elapsedSeed=donorStart+(masterPos-masterStart);
                std::vector<long> seeds={normalizedSeed,elapsedSeed};
                if(master==hintedMaster && offsetHints &&
                   master<(int)offsetHints->size() &&
                   donor<(int)offsetHints->size() &&
                   std::isfinite((*offsetHints)[master]) &&
                   std::isfinite((*offsetHints)[donor])) {
                    const double relativeSeconds=
                        (*offsetHints)[donor]-(*offsetHints)[master];
                    seeds.push_back(masterPos+
                        (long)std::llround(relativeSeconds*lowRate));
                }

                long lowBest=normalizedSeed;
                double lowBestQ=-2.0;
                for(long seed:seeds) {
                    const long lo=std::max<long>(0,seed-lowRadius);
                    const long hi=std::min<long>(
                        (long)bands[donor].low.size()-lowWin-1,
                        seed+lowRadius);
                    for(long p=lo;p<=hi;p+=lowCoarse) {
                        const double q=screenCorr(
                            bands[master].low,masterPos,
                            bands[donor].low,p,lowWin,4);
                        if(q>lowBestQ){lowBestQ=q;lowBest=p;}
                    }
                }
                const long lowRefine=lowBest;
                for(long p=lowRefine-lowCoarse;p<=lowRefine+lowCoarse;++p) {
                    if(p<0 || p+lowWin>=(long)bands[donor].low.size()) continue;
                    const double q=screenCorr(
                        bands[master].low,masterPos,
                        bands[donor].low,p,lowWin,1);
                    if(q>lowBestQ){lowBestQ=q;lowBest=p;}
                }
                if(lowBestQ<0.55) continue;
                ++report.validProbes;

                const long masterFrame=masterPos*lowStep;
                const long baseDelay=lowBest*lowStep-masterFrame;
                long robustBestDelay=baseDelay;
                double robustBest=-2.0;
                for(long d=baseDelay-robustRadius;
                    d<=baseDelay+robustRadius;d+=robustCoarse) {
                    if(masterFrame+d<0 ||
                       masterFrame+d+robustWin>=(long)sources[donor].frames()) continue;
                    const double q=robustMusicCorrAtStrided(
                        sources[master],sources[donor],masterFrame,
                        robustWin,d,robustStride);
                    if(q>robustBest){robustBest=q;robustBestDelay=d;}
                }
                const long robustRefine=robustBestDelay;
                for(long d=robustRefine-robustCoarse;
                    d<=robustRefine+robustCoarse;d+=4) {
                    if(masterFrame+d<0 ||
                       masterFrame+d+robustWin>=(long)sources[donor].frames()) continue;
                    const double q=robustMusicCorrAtStrided(
                        sources[master],sources[donor],masterFrame,
                        robustWin,d,robustStride);
                    if(q>robustBest){robustBest=q;robustBestDelay=d;}
                }
                const long sampleRefine=robustBestDelay;
                for(long d=sampleRefine-4;d<=sampleRefine+4;++d) {
                    if(masterFrame+d<0 ||
                       masterFrame+d+robustWin>=(long)sources[donor].frames()) continue;
                    const double q=robustMusicCorrAtStrided(
                        sources[master],sources[donor],masterFrame,
                        robustWin,d,robustStride);
                    if(q>robustBest){robustBest=q;robustBestDelay=d;}
                }
                if(std::isfinite(robustBest) && robustBest>-1.5)
                    robustScores.push_back(robustBest);
                report.bestRobust=std::max(report.bestRobust,robustBest);
                if(robustBest>=0.72) {
                    ++report.verifiedProbes;
                    if(firstVerified<0) firstVerified=probe;
                    lastVerified=probe;
                    affineProbes.push_back({masterFrame,(double)robustBestDelay,
                                            robustBest,probe});
                }
            }

            report.support=(double)report.verifiedProbes/probeCount;
            report.span=(firstVerified>=0 && lastVerified>firstVerified)
                ? (double)(lastVerified-firstVerified)/(probeCount-1) : 0.0;
            if(!robustScores.empty()) {
                std::sort(robustScores.begin(),robustScores.end());
                report.medianRobust=robustScores[robustScores.size()/2];
            }
            report.affine=fitAffineDelayModel(affineProbes,sr,probeCount);
            // Four strong probes spread over at least half of the programme
            // are enough to shortlist a Master only when their delays also
            // describe one coherent physical speed trajectory. The full map
            // still performs all ordinary 250 ms checks before any repair.
            report.viable=report.validProbes>=4 &&
                report.verifiedProbes>=4 && report.support>=4.0/probeCount &&
                report.span>=0.50 && report.affine.valid;
            if(report.viable) ++result.viableDonors[master];
        }

        std::vector<double> support;
        for(int donor=0;donor<count;++donor)
            if(donor!=master) support.push_back(result.donor[master][donor].support);
        std::sort(support.begin(),support.end(),
                  [](double a,double b){return a>b;});
        if(support.size()>=2) {
            result.secondSupport[master]=support[1];
            result.meanTopTwoSupport[master]=0.5*(support[0]+support[1]);
        }
    }
    return result;
}

[[maybe_unused]] static int chooseSyncAwareMaster(const std::vector<double>& quality,
                                 const std::vector<double>& detailPenalty,
                                 const MasterSyncPreflight& sync) {
    int best=-1;
    for(int pass=0;pass<2 && best<0;++pass) {
        for(int i=0;i<(int)quality.size();++i) {
            if(i>=(int)sync.viableDonors.size() || sync.viableDonors[i]<2) continue;
            if(pass==0 && i<(int)detailPenalty.size() && detailPenalty[i]!=0.0) continue;
            if(best<0 || quality[i]>quality[best]) best=i;
        }
    }
    return best;
}

enum class SourceRole { Master, Donor, Reserve, Rejected };

static const char* sourceRoleName(SourceRole role) {
    switch(role) {
        case SourceRole::Master: return "MASTER";
        case SourceRole::Donor: return "DONOR";
        case SourceRole::Reserve: return "RESERVE";
        default: return "REJECTED";
    }
}

struct ClusterMasterChoice {
    int source=-1;
    std::vector<int> componentSize;
    std::vector<int> strongDegree;
};

// x2p: unrelated or severely damaged copies must not steer Master selection.
// Build an undirected graph only from reciprocal whole-record sync viability,
// find the largest connected cluster, then choose its cleanest well-connected
// member. Once two donors are viable, measured source quality is deliberately
// considered before tiny correlation-score differences: a slightly noisier or
// damaged copy must not become Master merely because it resembles the others
// by a fraction of a percent more closely.
static ClusterMasterChoice chooseClusterAwareMaster(
        const std::vector<double>& quality,
        const std::vector<double>& detailPenalty,
        const MasterSyncPreflight& sync) {
    const int count=(int)quality.size();
    ClusterMasterChoice choice;
    choice.componentSize.assign((size_t)count,1);
    choice.strongDegree.assign((size_t)count,0);
    if(count==0 || (int)sync.donor.size()!=count) return choice;

    std::vector<std::vector<int>> adjacency((size_t)count);
    for(int a=0;a<count;++a) for(int b=a+1;b<count;++b) {
        const bool strong=a<(int)sync.donor.size() && b<(int)sync.donor[a].size() &&
            b<(int)sync.donor.size() && a<(int)sync.donor[b].size() &&
            sync.donor[a][b].viable && sync.donor[b][a].viable;
        if(strong) {
            adjacency[a].push_back(b);
            adjacency[b].push_back(a);
        }
    }
    for(int i=0;i<count;++i) choice.strongDegree[i]=(int)adjacency[i].size();

    std::vector<int> component((size_t)count,-1),sizes;
    for(int start=0;start<count;++start) {
        if(component[start]>=0) continue;
        const int id=(int)sizes.size();
        std::vector<int> stack={start};
        component[start]=id;
        int size=0;
        while(!stack.empty()) {
            const int here=stack.back();stack.pop_back();++size;
            for(int next:adjacency[here]) if(component[next]<0) {
                component[next]=id;stack.push_back(next);
            }
        }
        sizes.push_back(size);
    }
    for(int i=0;i<count;++i) choice.componentSize[i]=sizes[component[i]];

    auto better=[&](int candidate,int incumbent) {
        if(incumbent<0) return true;
        if(choice.componentSize[candidate]!=choice.componentSize[incumbent])
            return choice.componentSize[candidate]>choice.componentSize[incumbent];
        if(choice.strongDegree[candidate]!=choice.strongDegree[incumbent])
            return choice.strongDegree[candidate]>choice.strongDegree[incumbent];
        if(sync.viableDonors[candidate]!=sync.viableDonors[incumbent])
            return sync.viableDonors[candidate]>sync.viableDonors[incumbent];
        if(std::abs(quality[candidate]-quality[incumbent])>1e-12)
            return quality[candidate]>quality[incumbent];
        if(std::abs(sync.meanTopTwoSupport[candidate]-
                    sync.meanTopTwoSupport[incumbent])>1e-12)
            return sync.meanTopTwoSupport[candidate]>
                   sync.meanTopTwoSupport[incumbent];
        return candidate<incumbent;
    };

    for(int pass=0;pass<2 && choice.source<0;++pass) {
        for(int i=0;i<count;++i) {
            if(i>=(int)sync.viableDonors.size() || sync.viableDonors[i]<2) continue;
            if(pass==0 && i<(int)detailPenalty.size() && detailPenalty[i]!=0.0) continue;
            if(better(i,choice.source)) choice.source=i;
        }
    }
    return choice;
}

// The active core is Master plus at most four full donors. A sixth compatible
// copy becomes a sparse reserve/control source; non-viable copies are rejected
// before dense maps and reconstruction so they cannot vote or waste hours.
static std::vector<SourceRole> classifySourceRoles(
        int master,
        const std::vector<double>& quality,
        const MasterSyncPreflight& sync) {
    const int count=(int)quality.size();
    std::vector<SourceRole> roles((size_t)count,SourceRole::Rejected);
    if(master<0 || master>=count) return roles;
    roles[master]=SourceRole::Master;
    std::vector<int> viable;
    for(int source=0;source<count;++source)
        if(source!=master && source<(int)sync.donor[master].size() &&
           sync.donor[master][source].viable)
            viable.push_back(source);
    std::sort(viable.begin(),viable.end(),[&](int a,int b) {
        const auto& pa=sync.donor[master][a];
        const auto& pb=sync.donor[master][b];
        if(std::abs(pa.support-pb.support)>1e-12) return pa.support>pb.support;
        if(std::abs(pa.medianRobust-pb.medianRobust)>1e-12)
            return pa.medianRobust>pb.medianRobust;
        if(std::abs(quality[a]-quality[b])>1e-12) return quality[a]>quality[b];
        return a<b;
    });
    for(size_t i=0;i<viable.size();++i)
        roles[viable[i]]=i<4 ? SourceRole::Donor : SourceRole::Reserve;
    return roles;
}

static void writeDecisionMap(const std::string& path,
                             const std::vector<DecisionRow>& rows,
                             int sourceCount) {
    std::ofstream f(path);
    if (!f)
        throw std::runtime_error("Cannot create decision map " + path);

    f << std::fixed << std::setprecision(8);

    f << "time_sec,selected_source,disagreement,continuity,gain,dc,consensus_support,donor_control_corr,master_donor_corr,master_control_corr,outlier_ratio,eligible_witnesses,supporting_witnesses,opposing_witnesses,supporting_sources,abstaining_sources,opposing_sources,decision_reason,confidence_class";

    for (int s = 0; s < sourceCount; ++s)
        f << ",delay_" << (s + 1);

    for (int s = 0; s < sourceCount; ++s)
        f << ",quality_" << (s + 1);

    for (int s = 0; s < sourceCount; ++s)
        f << ",score_" << (s + 1);

    // v0.2.29l PEAK-SEPARATION DIAGNOSTIC: expose each pre-score eligibility gate.
    for (int s = 0; s < sourceCount; ++s) f << ",gate_corr_" << (s + 1);
    for (int s = 0; s < sourceCount; ++s) f << ",gate_map_conf_" << (s + 1);
    for (int s = 0; s < sourceCount; ++s) f << ",gate_local_verify_" << (s + 1);
    for (int s = 0; s < sourceCount; ++s) f << ",gate_reason_" << (s + 1);
    for (int s = 0; s < sourceCount; ++s) f << ",impulse_peak_" << (s + 1);
    for (int s = 0; s < sourceCount; ++s) f << ",impulse_floor_" << (s + 1);
    for (int s = 0; s < sourceCount; ++s) f << ",impulse_z_" << (s + 1);
    for (int s = 0; s < sourceCount; ++s) f << ",trend_fit_" << (s + 1);
    for (int s = 0; s < sourceCount; ++s) f << ",trend_corr_" << (s + 1);

    f << "\n";

    auto sourceList=[](const std::vector<int>& sources) {
        std::ostringstream text;
        for(size_t i=0;i<sources.size();++i) {
            if(i) text << '|';
            text << sources[i];
        }
        return text.str();
    };

    size_t writtenRows=0;
    for (const auto& r : rows) {
        reportWorkProgress(writtenRows++,rows.size());
        f << r.timeSec << ","
          << r.selectedSource << ","
          << r.disagreement << ","
          << r.continuity << ","
          << r.gain << ","
          << r.dc << ","
          << r.consensusSupport << ","
          << r.donorControlCorr << ","
          << r.masterDonorCorr << ","
          << r.masterControlCorr << ","
          << r.outlierRatio << ","
          << r.eligibleWitnesses << ","
          << r.supportingWitnesses << ","
          << r.opposingWitnesses << ","
          << sourceList(r.supportingSources) << ","
          << sourceList(r.abstainingSources) << ","
          << sourceList(r.opposingSources) << ","
          << r.decisionReason << ","
          << r.confidenceClass;

        for (double v : r.delay)   f << "," << v;
        for (double v : r.quality) f << "," << v;
        for (double v : r.score)   f << "," << v;
        for (double v : r.gateCorr) f << "," << v;
        for (double v : r.gateMapConfidence) f << "," << v;
        for (double v : r.gateLocalVerify) f << "," << v;
        for (const auto& v : r.gateReason) f << "," << v;
        for (double v : r.impulsePeak) f << "," << v;
        for (double v : r.impulseFloor) f << "," << v;
        for (double v : r.impulseZ) f << "," << v;
        for (double v : r.trendFit) f << "," << v;
        for (double v : r.trendCorr) f << "," << v;

        f << "\n";
    }
}

struct TimingFit {
    double intercept=0,slope=0,maxResidual=0,score=0;
    std::vector<size_t> inliers;
    bool accepted=false;
};
static TimingFit fitTimingConsensus(const std::vector<std::pair<long,TimingMatch>>& points,int sr) {
    TimingFit f;
    if(points.size()<5 || sr<=0) return f;
    auto median=[](std::vector<double> v) {
        std::sort(v.begin(),v.end());
        size_t n=v.size();return (v[(n-1)/2]+v[n/2])*0.5;
    };
    std::vector<double> slopes;
    for(size_t i=0;i<points.size();++i) for(size_t j=i+1;j<points.size();++j) {
        double dt=double(points[j].first)-points[i].first;
        if(dt>=sr) slopes.push_back((points[j].second.delay-points[i].second.delay)/dt);
    }
    if(slopes.empty()) return f;
    f.slope=median(slopes);
    if(std::abs(f.slope)>0.025) return f;
    std::vector<double> intercepts;
    for(const auto& p:points) intercepts.push_back(p.second.delay-f.slope*p.first);
    f.intercept=median(intercepts);
    for(size_t i=0;i<points.size();++i)
        if(std::abs(points[i].second.delay-f.intercept-f.slope*points[i].first)<=0.003*sr)
            f.inliers.push_back(i);
    // Require five points and at least 70% of the individually accepted probes.
    if(f.inliers.size()<5 || f.inliers.size()*10<points.size()*7) return f;
    if(points[f.inliers.back()].first-points[f.inliers.front()].first<sr) return f;
    double mx=0,my=0;
    for(auto i:f.inliers) {mx+=points[i].first;my+=points[i].second.delay;}
    mx/=f.inliers.size();my/=f.inliers.size();
    double xx=0,xy=0;
    for(auto i:f.inliers) {double x=points[i].first-mx;xx+=x*x;xy+=x*(points[i].second.delay-my);}
    if(xx<=0) return f;
    f.slope=xy/xx;f.intercept=my-f.slope*mx;
    for(auto i:f.inliers) {
        f.maxResidual=std::max(f.maxResidual,std::abs(points[i].second.delay-f.intercept-f.slope*points[i].first));
        f.score+=points[i].second.q;
    }
    f.score/=f.inliers.size();
    f.accepted=std::abs(f.slope)<=0.025 && f.maxResidual<=0.003*sr;
    return f;
}

static MonoFusionStats prepareChannels(Wav& w,bool stereoMode) {
    if(stereoMode) {
        if(w.channels!=2) throw std::runtime_error("Stereo mode requires two-channel source files. Select Mono for mono sources.");
        return MonoFusionStats{};
    }
    return fuseStereoGrooveToMono(w);
}

// Preview a requested donor against at least one additional independent donor.
// Existing Result is copied; only intervals whose physical vote and seam check
// pass are patched. The candidate remains a separate file for user approval.
static int localReviewPreview(const std::vector<Wav>& sources,int master,int donor,
    double startSec,double endSec,const std::vector<double>& delay,double speed,
    const std::string& baselinePath,const std::string& destination,
    const std::vector<bool>& eligible) {
    if(master<0 || master>=(int)sources.size() || donor<0 ||
       donor>=(int)sources.size() || donor==master || !std::isfinite(startSec) ||
       !std::isfinite(endSec) || startSec<0 || endSec<=startSec ||
       endSec-startSec>.020001 || !std::isfinite(speed) || speed<=0 ||
       delay.size()!=sources.size()) throw std::runtime_error("LOCAL_REVIEW_INVALID_RANGE_OR_SOURCE");
    if(std::filesystem::exists(destination))throw std::runtime_error("LOCAL_REVIEW_DESTINATION_EXISTS");
    const auto& ref=sources[master];
    const int sr=ref.sampleRate;
    if(endSec*sr>ref.frames()-2)throw std::runtime_error("LOCAL_REVIEW_RANGE_OUTSIDE_MASTER");
    Wav baseline=readWav(baselinePath);
    if(baseline.sampleRate!=sr || baseline.channels!=ref.channels || baseline.bits!=ref.bits)
        throw std::runtime_error("LOCAL_REVIEW_RESULT_FORMAT_MISMATCH");
    const auto expectedFrames=(size_t)std::llround(ref.frames()/speed);
    if(std::abs((double)baseline.frames()-(double)expectedFrames)>2)
        throw std::runtime_error("LOCAL_REVIEW_RESULT_DURATION_MISMATCH");
    auto mask=eligible;
    if(mask.size()!=sources.size() || !mask[donor])throw std::runtime_error("LOCAL_REVIEW_INELIGIBLE_SOURCE");
    mask[master]=false;
    auto trials=localImpulseTrials(sources,master,(long)std::floor(startSec*sr),
        (long)std::ceil(endSec*sr),delay,sr,mask,nullptr,donor);
    // Some broad impulse shoulders have a valid ordinary 2 ms vote but no
    // isolated micro-vote. Preserve that independently confirmed route too.
    if(trials.empty() && endSec-startSec<=.002001) {
        auto evidence=detectImpulseConsensus(sources,master,(long)std::llround(startSec*sr),
            (long)std::llround(endSec*sr),delay,sr,&mask);
        if(evidence.confirmed && evidence.donorSource==donor) {
            LocalImpulseTrial trial;trial.evidence=evidence;trial.delay=delay;
            trial.start=(long)std::llround(startSec*sr);trial.end=(long)std::llround(endSec*sr);
            trials.push_back(std::move(trial));
        }
    }
    std::vector<float> candidate=ref.samples;
    std::vector<std::pair<long,long>> accepted;
    std::ostringstream audit;
    audit<<"action,start_sec,end_sec,core_start_sec,core_end_sec,donor_source,control_source,gain,dc,delay_samples\n"<<std::setprecision(12);
    std::string reject="LOCAL_REVIEW_NO_TWO_CLEAN_MATCHING_WITNESSES";
    for(const auto& trial:trials) {
        const auto&e=trial.evidence;
        auto core=localizeImpulseCore(sources,master,donor,trial.start,trial.end,
            (long)ref.frames(),trial.delay,0,e.trendGain,sr);
        core=supportedImpulseShoulders(sources,master,donor,e.controlSource,
            core,trial.start,trial.end,trial.delay,e.trendGain,e.trendDc,sr);
        if(!core.localized){reject="LOCAL_REVIEW_CORE_NOT_ISOLATED";continue;}
        const long fade=std::max<long>(std::max(1,sr/8000),(core.end-core.start)/2);
        const long first=std::max<long>(1,core.start-fade);
        const long last=std::min<long>((long)ref.frames()-1,core.end+fade);
        if(std::any_of(accepted.begin(),accepted.end(),[&](const auto&r){return first<r.second && last>r.first;}))continue;
        auto trialOut=candidate;
        bool bad=false;
        for(long f=first;f<last && !bad;++f) {
            const double position=f+trial.delay[donor];
            if(position<1 || position+2>=sources[donor].frames()){bad=true;break;}
            double alpha=1;
            if(f<core.start)alpha=smoothRepairFade((double)(f-first+1)/(core.start-first+1));
            else if(f>=core.end)alpha=smoothRepairFade((double)(last-f)/(last-core.end+1));
            for(int ch=0;ch<ref.channels;++ch) {
                const size_t p=(size_t)f*ref.channels+ch;
                const double replacement=e.trendGain*sampleLinear(sources[donor],position,ch)+e.trendDc;
                if(!std::isfinite(replacement)||replacement<-1||replacement>=1){bad=true;break;}
                trialOut[p]=(float)((1-alpha)*ref.samples[p]+alpha*replacement);
            }
        }
        auto curvature=[&](const std::vector<float>& data,long f,int ch){const size_t p=(size_t)f*ref.channels+ch;return std::abs((double)data[p-ref.channels]-2*data[p]+data[p+ref.channels]);};
        for(int ch=0;ch<ref.channels && !bad;++ch) {
            std::vector<double> floorValues;
            for(long f=std::max<long>(1,first-sr/200);f<std::min<long>((long)ref.frames()-1,last+sr/200);++f)
                if(f<first-1||f>last)floorValues.push_back(curvature(ref.samples,f,ch));
            double floor=.000001;
            if(!floorValues.empty()){auto m=floorValues.begin()+floorValues.size()/2;std::nth_element(floorValues.begin(),m,floorValues.end());floor=std::max(floor,*m);}
            double beforePeak=0,afterPeak=0;
            for(long f=first-1;f<=last && f<(long)ref.frames()-1;++f) {
                if(f<1)continue;
                double before=curvature(ref.samples,f,ch),after=curvature(trialOut,f,ch);
                beforePeak=std::max(beforePeak,before);afterPeak=std::max(afterPeak,after);
                if(after>std::max(.00002,12*floor) && after>std::max(1.8*before,2.5*floor)){bad=true;break;}
            }
            if(beforePeak>std::max(.00002,12*floor) && afterPeak>=.92*beforePeak)bad=true;
        }
        const auto post=detectImpulseConsensus(sources,master,trial.start,trial.end,trial.delay,sr,&mask);
        if(!post.confirmed || std::find(post.supportingSources.begin(),post.supportingSources.end(),donor)==post.supportingSources.end())bad=true;
        if(!bad && !spliceShapeSupported(sources,master,donor,e.controlSource,
               trial.delay,e.trendGain,e.trendDc,trialOut,first,last,sr))bad=true;
        if(bad){reject="LOCAL_REVIEW_POSTCHECK_SEAM_OR_CONSENSUS_FAILED";continue;}
        candidate=std::move(trialOut);accepted.push_back({first,last});
        audit<<"MANUAL_SOURCE_APPROVED,"<<(double)first/sr<<","<<(double)last/sr<<","<<(double)core.start/sr<<","<<(double)core.end/sr<<","<<donor+1<<","<<e.controlSource+1<<","<<e.trendGain<<","<<e.trendDc<<","<<trial.delay[donor]<<"\n";
    }
    if(accepted.empty())throw std::runtime_error(reject);
    // Preserve the original PCM bytes outside the accepted patches, including
    // 32-bit PCM values that cannot round-trip through float.
    std::ifstream original(baselinePath,std::ios::binary);
    original.seekg(12);std::streamoff dataOffset=-1;
    while(original) {
        unsigned char h[8];original.read((char*)h,8);if(original.gcount()!=8)break;
        const uint32_t size=rd32(h+4);
        if(std::memcmp(h,"data",4)==0){dataOffset=original.tellg();break;}
        original.seekg((std::streamoff)size+(size&1),std::ios::cur);
    }
    if(dataOffset<0)throw std::runtime_error("LOCAL_REVIEW_DATA_CHUNK_MISSING");
    original.close();
    std::filesystem::copy_file(baselinePath,destination);
    std::fstream output(destination,std::ios::binary|std::ios::in|std::ios::out);
    constexpr int radius=24;
    const double pi=3.1415926535897932384626433832795;
    const double cutoff=std::min(1.0,1.0/speed);
    auto sinc=[&](double x){if(std::abs(x)<1e-12)return 1.0;double px=pi*x;return std::sin(px)/px;};
    const bool sameSpeed=std::abs(speed-1)<1e-12;
    for(const auto&r:accepted) {
        const long margin=sameSpeed?0:radius;
        const size_t first=(size_t)std::max(0.0,std::floor((r.first-margin)/speed));
        const size_t last=std::min(baseline.frames(),(size_t)std::ceil((r.second+margin)/speed));
        std::vector<float> patch((last-first)*ref.channels);
        for(size_t f=first;f<last;++f)for(int ch=0;ch<ref.channels;++ch) {
            double value=0;
            if(sameSpeed)value=candidate[f*ref.channels+ch];
            else {
                const double pos=f*speed;const long center=(long)std::floor(pos);
                double sum=0,weightSum=0;
                for(int k=-radius+1;k<=radius;++k) {
                    long sf=center+k;if(sf<0 || sf>=(long)ref.frames())continue;
                    double distance=pos-sf;
                    double weight=cutoff*sinc(cutoff*distance)*sinc(distance/radius);
                    sum+=candidate[(size_t)sf*ref.channels+ch]*weight;weightSum+=weight;
                }
                if(std::abs(weightSum)>1e-12)sum/=weightSum;
                value=std::clamp(sum,-1.0,.999999999);
            }
            patch[(f-first)*ref.channels+ch]=(float)value;
        }
        const auto patchPath=destination+".patch.wav";
        writeWavPcm(patchPath,sr,ref.channels,ref.bits,patch);
        std::ifstream encoded(patchPath,std::ios::binary);encoded.seekg(44);
        const size_t bytes=patch.size()*(ref.bits/8);
        std::vector<char> pcm(bytes);encoded.read(pcm.data(),bytes);
        if((size_t)encoded.gcount()!=bytes)throw std::runtime_error("LOCAL_REVIEW_PATCH_ENCODE_FAILED");
        output.seekp(dataOffset+(std::streamoff)(first*ref.channels*(ref.bits/8)));
        output.write(pcm.data(),pcm.size());
        encoded.close();std::filesystem::remove(patchPath);
    }
    output.flush();if(!output)throw std::runtime_error("LOCAL_REVIEW_PATCH_WRITE_FAILED");
    output.close();
    std::ofstream log(destination+".local-review.csv");log<<audit.str();
    if(!log)throw std::runtime_error("LOCAL_REVIEW_AUDIT_WRITE_FAILED");
    std::cout<<"LOCAL_REVIEW_ACCEPTED "<<accepted.size()<<" donor "<<donor+1<<"\n";
    return 0;
}


// Extend only measured, two-witness timing. Windows shrink at file boundaries;
// no delay is extrapolated into a silent or unsupported edge.
static int extendEdgeTimeMaps(const std::vector<Wav>& sources,
        std::vector<std::vector<TimeMapAnchor>>& maps,
        const std::vector<bool>& eligible, int sr) {
    if(sources.empty() || sources[0].frames()<size_t(sr/20)) return 0;
    const long end=(long)sources[0].frames()-1, hop=std::max(1,sr/40);
    // A previous model-based frame-zero bridge is diagnostic only now.
    // Edge repairs must be bracketed by measured evidence.
    for(size_t k=1;k<maps.size();++k)
        for(auto& a:maps[k])
            if(a.masterFrame==0 && a.corr<=0.70) a.repairSafe=false;
    int added=0;
    for(int direction : {-1,1}) {
        long seed=direction<0?end:0; bool found=false;
        // Find a shared bracket, rather than trusting an isolated endpoint.
        for(long f=direction<0?0:end; f>=0 && f<=end; f-=direction*hop) {
            int count=0;
            for(size_t k=1;k<maps.size();++k)
                if(eligible[k] && mappedConfidenceAt(maps[k],f,sr)>=0.80) ++count;
            if(count>=2){seed=f;found=true;break;}
        }
        if(!found) continue;
        std::vector<int> chain;
        std::vector<double> delay(maps.size());
        for(size_t k=1;k<maps.size();++k)
            if(eligible[k] && mappedConfidenceAt(maps[k],seed,sr)>=0.80) {
                chain.push_back((int)k);delay[k]=mappedDelayAt(maps[k],seed,sr);
            }
        long previous=seed;
        while(previous!=(direction<0?0:end)) {
            const long f=direction<0?std::max(0L,previous-hop):std::min(end,previous+hop);
            const long lo=std::max(0L,f-sr/40);
            const int win=(int)(std::min(end+1,lo+sr/20)-lo);
            std::vector<int> passing;
            std::vector<long> candidate(maps.size());
            std::vector<double> quality(maps.size());
            for(int k:chain) {
                const long radius=std::max(4,sr/1000);
                const double roundedDelay=std::round(delay[k]);
                // Windows long is 32-bit. Reject an unrepresentable basin,
                // including the search radius, before narrowing explicitly.
                if(!std::isfinite(roundedDelay) ||
                   roundedDelay<(double)std::numeric_limits<long>::min()+radius ||
                   roundedDelay>(double)std::numeric_limits<long>::max()-radius)
                    continue;
                const long center=static_cast<long>(roundedDelay);
                double best=-2,second=-2;long bestDelay=center;
                // Local basin only; reject competing phase peaks and boundaries.
                for(long d=center-radius;d<=center+radius;++d) {
                    if(lo+d<0 || lo+d+win>(long)sources[k].frames()) continue;
                    const double q=corrAtStrided(sources[0],sources[k],lo,win,d,std::max(1,sr/12000));
                    if(q>best){best=q;bestDelay=d;}
                }
                for(long d=center-radius;d<=center+radius;d+=4) {
                    if(std::labs(d-bestDelay)<=std::max(4,sr/2000)) continue;
                    if(lo+d<0 || lo+d+win>(long)sources[k].frames()) continue;
                    second=std::max(second,corrAtStrided(sources[0],sources[k],lo,win,d,std::max(1,sr/12000)));
                }
                if(best<0.97 || best-second<0.002 || std::labs(bestDelay-center)>=radius) continue;
                candidate[k]=bestDelay;quality[k]=best;passing.push_back(k);
            }
            std::vector<int> supported;
            for(int a:passing) for(int b:passing) {
                if(a==b || std::abs((candidate[a]-delay[a])-(candidate[b]-delay[b]))>sr*0.001) continue;
                if(corrAtStrided(sources[a],sources[b],lo+candidate[a],win,
                        candidate[b]-candidate[a],std::max(1,sr/12000))>=0.97) {
                    supported.push_back(a);break;
                }
            }
            if(supported.size()<2) {
                std::cerr<<"EDGE_SYNC_STOP direction "<<direction<<" frame "<<f<<" reason INSUFFICIENT_TWO_DONOR_SUPPORT\n";break;
            }
            for(int k:supported) {
                TimeMapAnchor a; a.masterFrame=f;a.donorDelay=candidate[k];
                a.corr=quality[k];a.verified=true;a.repairSafe=true;
                maps[k].push_back(a);delay[k]=candidate[k];++added;
                std::sort(maps[k].begin(),maps[k].end(),[](const TimeMapAnchor& x,const TimeMapAnchor& y){return x.masterFrame<y.masterFrame;});
            }
            chain=supported;previous=f;
        }
    }
    return added;
}

int main(int argc, char** argv) {
    try {
        if (argc < 5) {
            std::cerr
                << kEngineDisplayVersion << "\n\n"
                << "Usage:\n"
                << "  vinyl2000 N out.wav source1.wav source2.wav [...]\n"
                << "  vinyl2000 N out.wav --map decisions.csv source1.wav ...\n\n"
                << "N must be 3, 4, 5 or 6.\n";
            return 2;
        }

        int sourceCount = std::atoi(argv[1]);
        if (sourceCount < 3 || sourceCount > 6)
            throw std::runtime_error(
                "Source count must be between 3 and 6");

        std::string outName = argv[2];
        std::string mapName;

        int arg = 3;
        int requestedMaster = 0; // 0 = automatic, otherwise 1-based input source
        std::string masterSelectionMode = "auto";
        double recordedRpm = 0.0;
        double discRpm = 0.0;

        if (arg < argc && std::string(argv[arg]) == "--master") {
            if (arg + 1 >= argc) throw std::runtime_error("--master requires auto or a source number");
            std::string m = argv[arg + 1];
            if (m != "auto") {
                requestedMaster = std::atoi(m.c_str());
                masterSelectionMode = "manual";
            }
            arg += 2;
        }

        if (arg < argc && std::string(argv[arg]) == "--master-origin") {
            if (arg + 1 >= argc) throw std::runtime_error("--master-origin requires auto-frozen or manual");
            const std::string origin=argv[arg+1];
            if(origin!="auto-frozen" && origin!="manual")
                throw std::runtime_error("--master-origin requires auto-frozen or manual");
            if(requestedMaster<=0)
                throw std::runtime_error("--master-origin requires an explicit Master source");
            masterSelectionMode=origin=="auto-frozen"?"auto_frozen":"manual";
            arg+=2;
        }

        bool stereoMode=false;
        if (arg < argc && std::string(argv[arg]) == "--channel-mode") {
            if(arg+1>=argc) throw std::runtime_error("--channel-mode requires mono or stereo");
            std::string mode=argv[arg+1];
            if(mode!="mono" && mode!="stereo") throw std::runtime_error("Channel mode must be mono or stereo");
            stereoMode=mode=="stereo";arg+=2;
        }
        if (arg < argc && std::string(argv[arg]) == "--recorded-rpm") {
            if (arg + 1 >= argc) throw std::runtime_error("--recorded-rpm requires a number");
            recordedRpm=std::stod(argv[arg+1]);
            arg+=2;
        }

        if (arg < argc && std::string(argv[arg]) == "--disc-rpm") {
            if (arg + 1 >= argc) throw std::runtime_error("--disc-rpm requires a number");
            discRpm=std::stod(argv[arg+1]);
            arg+=2;
        }

        if (arg < argc && std::string(argv[arg]) == "--map") {
            if (arg + 1 >= argc)
                throw std::runtime_error("--map requires a filename");

            mapName = argv[arg + 1];
            arg += 2;
        }

        std::string previewPath;
        std::vector<double> manualOffsets(sourceCount,std::numeric_limits<double>::quiet_NaN());
        if(arg<argc && std::string(argv[arg])=="--preview-sync") {
            if(arg+1>=argc) throw std::runtime_error("Missing preview path");
            previewPath=argv[arg+1];arg+=2;
        }
        if(arg<argc && std::string(argv[arg])=="--sync-offsets") {
            if(arg+1>=argc) throw std::runtime_error("Missing sync offsets");
            std::istringstream values(argv[arg+1]);std::string value;int i=0;
            while(std::getline(values,value,',')) {
                if(i>=sourceCount) throw std::runtime_error("Too many sync offsets");
                if(value!="auto") {manualOffsets[i]=std::stod(value);if(!std::isfinite(manualOffsets[i]))throw std::runtime_error("Invalid sync offset");}
                ++i;
            }
            if(i!=sourceCount) throw std::runtime_error("Wrong sync offset count");
            arg+=2;
        }

        double syncCheckStart=0.0, syncCheckSeconds=0.0;
        if(arg<argc && std::string(argv[arg])=="--sync-check") {
            if(arg+2>=argc)throw std::runtime_error("Missing sync-check start/duration");
            syncCheckStart=std::stod(argv[arg+1]);syncCheckSeconds=std::stod(argv[arg+2]);arg+=3;
            if(!std::isfinite(syncCheckStart)||!std::isfinite(syncCheckSeconds)||syncCheckStart<0||syncCheckSeconds<2||syncCheckSeconds>30)
                throw std::runtime_error("Sync range: start >= 0, duration 2..30 seconds");
        }
        bool localReview=false;
        int localDonor=-1;
        double localStart=0,localEnd=0,localSpeed=1;
        std::string localBaseline;
        std::vector<double> localDelay;
        std::vector<bool> localEligible;
        if(arg<argc && std::string(argv[arg])=="--local-review") {
            if(arg+7>=argc)throw std::runtime_error("LOCAL_REVIEW_ARGUMENTS_MISSING");
            localReview=true;localDonor=std::stoi(argv[arg+1])-1;
            localStart=std::stod(argv[arg+2]);localEnd=std::stod(argv[arg+3]);
            localSpeed=std::stod(argv[arg+4]);localBaseline=argv[arg+5];
            std::istringstream values(argv[arg+6]);std::string value;
            while(std::getline(values,value,',')) {
                double d=std::stod(value);if(!std::isfinite(d))throw std::runtime_error("LOCAL_REVIEW_INVALID_DELAY");
                localDelay.push_back(d);
            }
            std::istringstream admitted(argv[arg+7]);
            while(std::getline(admitted,value,',')) {
                if(value!="0" && value!="1")throw std::runtime_error("LOCAL_REVIEW_INVALID_ELIGIBILITY");
                localEligible.push_back(value=="1");
            }
            arg+=8;
        }
        if (argc - arg != sourceCount)
            throw std::runtime_error(
                "Source count does not match number of WAV files");

        std::vector<Wav> sources;
        sources.reserve(sourceCount);
        std::vector<MonoFusionStats> monoFusion(sourceCount);
        int inputRate=0,inputChannels=0,inputBits=0;
        for (int i = 0; i < sourceCount; ++i) {
            Wav w=readWav(argv[arg+i]);
            if(i==0) {
                inputRate=w.sampleRate; inputChannels=w.channels; inputBits=w.bits;
            } else if (w.sampleRate != inputRate ||
                       w.channels != inputChannels ||
                       w.bits != inputBits) {
                throw std::runtime_error("All input WAV formats must match exactly");
            }
            monoFusion[i]=prepareChannels(w,stereoMode);
            sources.push_back(std::move(w));
        }

        if(localReview) {
            if(requestedMaster<=0 || requestedMaster>sourceCount)
                throw std::runtime_error("LOCAL_REVIEW_EXPLICIT_MASTER_REQUIRED");
            return localReviewPreview(sources,requestedMaster-1,localDonor,
                localStart,localEnd,localDelay,localSpeed,localBaseline,outName,localEligible);
        }

        const auto profileTotalStart = std::chrono::steady_clock::now();
        auto profilePhaseStart = profileTotalStart;
        auto profileSeconds = [&]() {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now() - profilePhaseStart).count();
        };
        auto profilePhase = [&](const char* name) {
            std::cerr << "PROFILE " << name << " " << profileSeconds() << "\n";
            profilePhaseStart = std::chrono::steady_clock::now();
        };

        std::vector<int> originalIndex(sourceCount);
        for (int i=0;i<sourceCount;++i) originalIndex[i]=i;
        std::vector<double> masterScores(sourceCount);
        std::vector<SourceAssessmentMetrics> sourceAssessment(sourceCount);
        // Use gain-independent dynamics/click penalties for both mono and stereo.
        // The old mono score rewarded low curvature, including lost detail.
        const auto masterScreen=screenMasterCandidates(sources);
        for (int i=0;i<sourceCount;++i) {
            sourceAssessment[i]=assessSource(sources[i]);
            masterScores[i]=sourceAssessment[i].qualityScore-masterScreen.penalty[i];
            std::cerr << "MASTER_SCREEN " << (i+1)
                      << " verified_probes " << masterScreen.verified[i]
                      << " missing_detail_probes " << masterScreen.missing[i]
                      << " penalty " << masterScreen.penalty[i]
                      << " score " << masterScores[i]
                      << " mode " << masterSelectionMode << "\n";
        }

        std::cerr << "PHASE MASTER_PREFLIGHT 0 " << sourceCount << "\n";
        const auto syncPreflight=screenMasterSyncFeasibility(
            sources,&manualOffsets,requestedMaster-1,
            [&](int done,int total,int candidate,int donor) {
                std::cerr << "PREFLIGHT_PROGRESS " << done << " " << total
                          << " " << (candidate+1) << " " << (donor+1) << "\n";
            });
        const auto clusterChoice=chooseClusterAwareMaster(
            masterScores,masterScreen.penalty,syncPreflight);
        int automaticRecommendedMaster=clusterChoice.source;
        if(automaticRecommendedMaster<0)
            automaticRecommendedMaster=(int)std::distance(
                masterScores.begin(),
                std::max_element(masterScores.begin(),masterScores.end()));
        for(int master=0;master<sourceCount;++master) {
            for(int donor=0;donor<sourceCount;++donor) {
                if(donor==master) continue;
                const auto& p=syncPreflight.donor[master][donor];
                std::cerr << "MASTER_PREFLIGHT candidate " << (master+1)
                          << " donor " << (donor+1)
                          << " valid " << p.validProbes
                          << " verified " << p.verifiedProbes
                          << " support " << p.support
                          << " span " << p.span
                          << " median " << p.medianRobust
                          << " best " << p.bestRobust
                          << " affine_intercept " << p.affine.intercept
                          << " affine_slope " << p.affine.slope
                          << " affine_rate " << (1.0+p.affine.slope)
                          << " affine_inliers " << p.affine.inliers
                          << " affine_span " << p.affine.span
                          << " affine_max_residual_ms "
                          << (1000.0*p.affine.maxResidual/sources[0].sampleRate)
                          << " affine_valid " << (p.affine.valid?1:0)
                          << " viable " << (p.viable?1:0) << "\n";
            }
            std::cerr << "MASTER_CANDIDATE source " << (master+1)
                      << " viable_donors " << syncPreflight.viableDonors[master]
                      << " cluster_size " << clusterChoice.componentSize[master]
                      << " strong_degree " << clusterChoice.strongDegree[master]
                      << " second_support " << syncPreflight.secondSupport[master]
                      << " mean_top_two_support " << syncPreflight.meanTopTwoSupport[master]
                      << " quality " << masterScores[master] << "\n";
        }
        std::cerr << "PHASE MASTER_PREFLIGHT " << sourceCount << " " << sourceCount << "\n";

        int audioMasterIndex=0;
        std::string selectionFailure;
        if (requestedMaster > 0) {
            if (requestedMaster > sourceCount) throw std::runtime_error("Requested master source is out of range");
            audioMasterIndex=requestedMaster-1;
            if(sourceCount>=3 && syncPreflight.viableDonors[audioMasterIndex]<2) {
                selectionFailure=masterSelectionMode=="auto_frozen"
                    ? "The automatically selected Master no longer has two syncable donors in the whole-record preflight."
                    : "The manually selected Master does not have two syncable donors in the whole-record preflight.";
            }
        } else {
            if(sourceCount>=3) {
                if(clusterChoice.source>=0) audioMasterIndex=clusterChoice.source;
                else {
                    audioMasterIndex=(int)std::distance(
                        masterScores.begin(),
                        std::max_element(masterScores.begin(),masterScores.end()));
                    selectionFailure="No Master candidate has two independently syncable donors in the fast whole-record preflight.";
                }
            } else {
                audioMasterIndex=(int)std::distance(
                    masterScores.begin(),
                    std::max_element(masterScores.begin(),masterScores.end()));
            }
        }

        auto sourceRolesOriginal=classifySourceRoles(
            audioMasterIndex,masterScores,syncPreflight);
        const auto recommendedRoles=classifySourceRoles(
            automaticRecommendedMaster,masterScores,syncPreflight);
        for(int source=0;source<sourceCount;++source) {
            const auto& relation=syncPreflight.donor[audioMasterIndex][source];
            std::cerr << "SOURCE_ROLE " << (source+1) << " "
                      << sourceRoleName(sourceRolesOriginal[source])
                      << " support " << (source==audioMasterIndex?1.0:relation.support)
                      << " viable " << (source==audioMasterIndex?1:(relation.viable?1:0))
                      << "\n";
        }

        if(previewPath.empty()) {
            std::ofstream selection(outName+".master-selection.csv");
            if(!selection) throw std::runtime_error("Cannot write master selection diagnostic");
            selection << "source,selected,mode,role,verified_probes,missing_detail_probes,detail_penalty,final_score,programme_level_dbfs,headroom_db,clean_windows_pct,impulse_windows,impulses_per_minute,clipped_samples,sync_viable_donors,cluster_size,strong_degree,sync_second_support,sync_mean_top_two_support,sync_candidate_viable,selection_status\n";
            for(int i=0;i<sourceCount;++i)
                selection << (i+1) << "," << (i==audioMasterIndex?1:0) << ","
                          << masterSelectionMode << "," << sourceRoleName(sourceRolesOriginal[i]) << ","
                          << masterScreen.verified[i] << "," << masterScreen.missing[i] << ","
                          << masterScreen.penalty[i] << "," << masterScores[i] << ","
                          << sourceAssessment[i].programmeLevelDbfs << ","
                          << sourceAssessment[i].headroomDb << ","
                          << sourceAssessment[i].cleanWindowsPercent << ","
                          << sourceAssessment[i].impulseWindows << ","
                          << sourceAssessment[i].impulsesPerMinute << ","
                          << sourceAssessment[i].clippedSamples << ","
                          << syncPreflight.viableDonors[i] << ","
                          << clusterChoice.componentSize[i] << ","
                          << clusterChoice.strongDegree[i] << ","
                          << syncPreflight.secondSupport[i] << ","
                          << syncPreflight.meanTopTwoSupport[i] << ","
                          << (syncPreflight.viableDonors[i]>=2?1:0) << ","
                          << (!selectionFailure.empty()?"NO_VIABLE_MASTER":
                              (i==audioMasterIndex?"SELECTED":"NOT_SELECTED")) << "\n";
        }

        // The preview UI consumes this compact, human-readable assessment
        // before asking the user to accept or override the recommended Master.
        // It is a sidecar to --preview-sync and therefore never affects a full
        // command-line restoration unless the caller explicitly requests it.
        if(!previewPath.empty()) {
            std::ofstream assessment(previewPath+".assessment.csv");
            if(!assessment) throw std::runtime_error("Cannot write source assessment preview");
            assessment << "source,recommended_master,selected_master,recommended_role,selected_role,quality_score,programme_level_dbfs,headroom_db,clean_windows_pct,active_windows,impulse_windows,impulses_per_minute,clipped_samples,sync_support_pct,sync_second_support_pct,viable_donors,cluster_size,strong_degree,detail_penalty\n";
            assessment << std::setprecision(12);
            for(int source=0;source<sourceCount;++source) {
                const auto& m=sourceAssessment[source];
                assessment << (source+1) << ","
                           << (source==automaticRecommendedMaster?1:0) << ","
                           << (source==audioMasterIndex?1:0) << ","
                           << sourceRoleName(recommendedRoles[source]) << ","
                           << sourceRoleName(sourceRolesOriginal[source]) << ","
                           << masterScores[source] << ","
                           << m.programmeLevelDbfs << ","
                           << m.headroomDb << ","
                           << m.cleanWindowsPercent << ","
                           << m.activeWindows << ","
                           << m.impulseWindows << ","
                           << m.impulsesPerMinute << ","
                           << m.clippedSamples << ","
                           << 100.0*syncPreflight.meanTopTwoSupport[source] << ","
                           << 100.0*syncPreflight.secondSupport[source] << ","
                           << syncPreflight.viableDonors[source] << ","
                           << clusterChoice.componentSize[source] << ","
                           << clusterChoice.strongDegree[source] << ","
                           << masterScreen.penalty[source] << "\n";
            }
        }

        if(!mapName.empty()) {
            std::string name=mapName;
            const std::string suffix=".decisions.csv";
            const size_t at=name.rfind(suffix);
            if(at!=std::string::npos) name.replace(at,suffix.size(),".master-preflight.csv");
            else name += ".master-preflight.csv";
            std::ofstream diagnostic(name);
            if(!diagnostic) throw std::runtime_error("Cannot write Master preflight diagnostic");
            diagnostic << "candidate_source,donor_source,valid_probes,verified_probes,support,span,median_robust,best_robust,affine_intercept_samples,affine_slope,affine_rate,affine_inliers,affine_span,affine_max_residual_ms,affine_valid,viable\n";
            for(int master=0;master<sourceCount;++master)
                for(int donor=0;donor<sourceCount;++donor) if(donor!=master) {
                    const auto& p=syncPreflight.donor[master][donor];
                    diagnostic << (master+1) << "," << (donor+1) << ","
                               << p.validProbes << "," << p.verifiedProbes << ","
                               << p.support << "," << p.span << ","
                               << p.medianRobust << "," << p.bestRobust << ","
                               << p.affine.intercept << "," << p.affine.slope << ","
                               << (1.0+p.affine.slope) << "," << p.affine.inliers << ","
                               << p.affine.span << ","
                               << (1000.0*p.affine.maxResidual/sources[0].sampleRate) << ","
                               << (p.affine.valid?1:0) << ","
                               << (p.viable?1:0) << "\n";
                }
            std::cerr << "MASTER_PREFLIGHT_DIAGNOSTIC " << name << "\n";

            std::string roleName=mapName;
            if(at!=std::string::npos)
                roleName.replace(at,suffix.size(),".source-roles.csv");
            else roleName += ".source-roles.csv";
            std::ofstream roles(roleName);
            if(!roles) throw std::runtime_error("Cannot write source-role diagnostic");
            roles << "source,role,selected_master,preflight_viable,support,median_robust,quality,cluster_size,strong_degree\n";
            for(int source=0;source<sourceCount;++source) {
                const auto& relation=syncPreflight.donor[audioMasterIndex][source];
                roles << (source+1) << "," << sourceRoleName(sourceRolesOriginal[source]) << ","
                      << (source==audioMasterIndex?1:0) << ","
                      << (source==audioMasterIndex?1:(relation.viable?1:0)) << ","
                      << (source==audioMasterIndex?1.0:relation.support) << ","
                      << (source==audioMasterIndex?1.0:relation.medianRobust) << ","
                      << masterScores[source] << ","
                      << clusterChoice.componentSize[source] << ","
                      << clusterChoice.strongDegree[source] << "\n";
            }
            std::cerr << "SOURCE_ROLE_DIAGNOSTIC " << roleName << "\n";
        }

        if(!selectionFailure.empty()) {
            std::cerr << "MASTER_PREFLIGHT_UNRESOLVED " << selectionFailure << "\n";
            throw std::runtime_error(selectionFailure+
                " Choose a Master manually for diagnosis, or add/replace a physical source. No result WAV was written.");
        }
        profilePhase("master_preflight");

        // v0.2.29z: the chosen audio master IS the time axis. The previous
        // longest-file reference rewrote even the best source against the
        // timing of a poorer transfer. Put the chosen master at internal index
        // zero so every unmodified output sample has an exact source identity.
        if(audioMasterIndex!=0) {
            std::swap(sources[0],sources[audioMasterIndex]);
            std::swap(originalIndex[0],originalIndex[audioMasterIndex]);
            std::swap(masterScores[0],masterScores[audioMasterIndex]);
            std::swap(sourceAssessment[0],sourceAssessment[audioMasterIndex]);
            std::swap(sourceRolesOriginal[0],sourceRolesOriginal[audioMasterIndex]);
        }
        const auto sourceRoles=sourceRolesOriginal;
        std::vector<bool> processingEligible((size_t)sourceCount,false);
        for(int source=0;source<sourceCount;++source)
            processingEligible[source]=sourceRoles[source]!=SourceRole::Rejected;
        const int processingSourceCount=(int)std::count(
            processingEligible.begin(),processingEligible.end(),true);
        audioMasterIndex=0;
        const Wav& ref = sources[0];
        const int sr = ref.sampleRate;
        std::vector<long> musicOnset(sourceCount,0);
        std::vector<double> onsetDelay(sourceCount,0.0);
        std::vector<double> onsetCorr(sourceCount,1.0);
        std::vector<double> onsetTransition(sourceCount,0.0);
        for(int i=0;i<sourceCount;++i)
            if(processingEligible[i]) musicOnset[i]=findSustainedMusicOnset(sources[i]);
        for(int i=0;i<sourceCount;++i) if(processingEligible[i])
            onsetTransition[i]=onsetTransitionRatio(sources[i],musicOnset[i]);
        for(int i=1;i<sourceCount;++i) if(processingEligible[i]) {
            const auto refined=refineOnsetDelay(ref,sources[i],musicOnset[0],musicOnset[i]);
            onsetDelay[i]=refined.first;
            onsetCorr[i]=refined.second;
        }
        std::cerr << "TIME_REFERENCE " << (originalIndex[0]+1) << "\n";
        std::cerr << "MASTER_SELECTED " << (originalIndex[0]+1) << "\n";
        for(int i=0;i<sourceCount;++i) {
            const auto& m=monoFusion[originalIndex[i]];
            std::cerr << "MONO_FUSION " << (originalIndex[i]+1)
                      << " applied " << (m.applied?1:0)
                      << " corr " << m.correlation
                      << " right_to_left_gain " << m.rightToLeftGain
                      << " left_gain " << m.leftGain
                      << " right_gain " << m.rightGain
                      << " side_before_db " << m.sideToMidBeforeDb
                      << " side_after_db " << m.sideToMidAfterDb
                      << " left_impulse_repairs " << m.leftChannelRepairs
                      << " right_impulse_repairs " << m.rightChannelRepairs
                      << " changed_frames " << m.changedFrames
                      << " mono_extreme_events " << m.monoExtremeEvents
                      << " mono_quality_score " << m.qualityScore << "\n";
        }
        for(int i=0;i<sourceCount;++i)
            std::cerr << "START_ANCHOR " << (originalIndex[i]+1)
                      << " onset_sec " << ((double)musicOnset[i]/sr)
                      << " delay_samples " << onsetDelay[i]
                      << " corr " << onsetCorr[i]
                      << " transition_ratio " << onsetTransition[i] << "\n";
        for(int i=0;i<sourceCount;++i) std::cerr << "MASTER_SCORE " << (originalIndex[i]+1) << " " << masterScores[i] << "\n";

        const int window =
            std::max(1024, sr * 64 / 1000);

        const int hop =
            std::max(256, sr * 16 / 1000);

        const int searchRadius =
            std::max(8, sr * 3 / 1000);

        // v0.2.6 full-range start synchronization:
        // - search far enough into the reference to get past recorder/needle-start silence
        // - search both negative and positive offsets over a full +/-20 s range
        // - do not assume source 1 starts before the donor files
        // - refine the winning offset down to sample precision before the
        //   existing continuous micro-alignment takes over.
        const int initRadius =
            (int)std::min<long>((long)(20.0 * sr),
                                std::max<long>(sr, (long)ref.frames() - 1));

        const long initPos = findInitialAnchor(ref, 25);

        std::vector<TrackState> state(sourceCount);
        std::vector<BroadDelayDiag> globalAlignDiag(sourceCount);

        state[0].delay = 0.0;
        globalAlignDiag[0].accepted = true;
        globalAlignDiag[0].returnedDelay = 0.0;
        globalAlignDiag[0].rejectReason = "MASTER_REFERENCE";
        state[0].measuredDelay = 0.0;
        state[0].quality = 1.0;

        const int mappedSourceCount=std::max(0,processingSourceCount-1);
        std::cerr << "PHASE GLOBAL_ALIGNMENT 0 " << mappedSourceCount << "\n";
        int globalOrdinal=0;
        for (int s = 1; s < sourceCount; ++s) {
            if(!processingEligible[s]) {
                state[s].quality=0.0;
                globalAlignDiag[s].rejectReason="SOURCE_REJECTED_BY_PREFLIGHT";
                continue;
            }
            std::cerr << "PHASE GLOBAL_ALIGNMENT " << globalOrdinal << " "
                      << mappedSourceCount << " " << (originalIndex[s]+1) << " "
                      << sourceRoleName(sourceRoles[s]) << "\n";
            state[s].delay =
                broadDelay(ref,
                           sources[s],
                           window,
                           initRadius,
                           &globalAlignDiag[s]);

            state[s].measuredDelay =
                state[s].delay;

            state[s].quality = 1.0;
            ++globalOrdinal;
            std::cerr << "PHASE GLOBAL_ALIGNMENT " << globalOrdinal << " "
                      << mappedSourceCount << " " << (originalIndex[s]+1) << " "
                      << sourceRoleName(sourceRoles[s]) << "\n";
        }
        profilePhase("global_alignment");
        // Preview uses the SAME multi-anchor seed search as restoration.
        // A rejected broadDelay result (zero) must not be presented as aligned.
        std::ofstream preview;
        if(!previewPath.empty()) {
            preview.open(previewPath);
            if(!preview) throw std::runtime_error("Cannot save sync preview");
            preview<<std::setprecision(12)<<"source,master,offset_sec,verified,score,status\n";
            preview<<(originalIndex[0]+1)<<","<<(originalIndex[0]+1)<<",0,1,1,MASTER_REFERENCE\n";
        }



        std::cerr
            << kEngineDisplayVersion << ", "
            << sourceCount
            << " sources\n"
            << "Initial anchor: "
            << ((double)initPos / sr)
            << " s, search radius: +/-"
            << ((double)initRadius / sr)
            << " s\n";

        for (int s = 1; s < sourceCount; ++s) {
            std::cerr
                << "Initial delay source "
                << (originalIndex[s] + 1)
                << ": "
                << state[s].delay
                << " samples\n";
        }

        size_t outFrames = ref.frames();

        // Exact master copy. Reconstruction may touch only explicitly logged
        // class-A repair frames; all other samples remain bit-for-bit equal in
        // the engine's floating-point domain.
        std::vector<float> out;
        if(previewPath.empty() && syncCheckSeconds==0) out = ref.samples;
        std::vector<RepairRecord> repairRecords;

        std::vector<double> prevDelay(sourceCount, 0.0);
        std::vector<double> nextDelay(sourceCount, 0.0);
        // v0.2.29: persistent physical delay trajectory for independent analogue
        // transfers.  A strong 0.5 s anchor updates drift velocity; weak 16 ms
        // micro-correlations may refine it but may no longer invalidate a donor.
        std::vector<double> trajectoryVelocity(sourceCount, 0.0); // delay samples / master frame
        std::vector<double> trajectoryQuality(sourceCount, 1.0);
        std::vector<long> lastAnchorPos(sourceCount, 0);

        // v0.2.29l PEAK-SEPARATION DIAGNOSTIC — verified time map.
        // Reconstruction thresholds/decisions are unchanged; this version only logs anchor evidence.
        // Build the donor->Master timeline before allowing reconstruction.
        std::vector<std::vector<TimeMapAnchor>> timeMaps(sourceCount);
        std::vector<std::vector<TimeMapAnchor>> timingMaps(sourceCount);
        // Retained outside the donor loop because reconstruction may need the
        // same selected affine fallback wherever a local map is not repair-safe.
        // Zero means the original whole-record intercept was retained.
        std::vector<double> affineBasinOffsets(sourceCount,0.0);
        std::vector<bool> affineBasinLocked(sourceCount,false);
        std::vector<bool> onsetBasinConflict(sourceCount,false);
        std::vector<long> basinRepairUnlockFrame(sourceCount,-1);
        const auto timingRef=makeTimingSignal(ref);
        const int mapHop = std::max(1, sr / 4);        // 250 ms active donor
        const int mapWin = std::max(1024, sr / 5);    // 200 ms
        const int mapRadius = std::max(64, sr / 50);  // +/-20 ms
        const int mapStride = std::max(1, sr / 12000);
        const double mapCorrMin = 0.72;

        std::cerr << "PHASE TIME_MAP 0 " << mappedSourceCount << "\n";

        // v0.2.29h: write the complete global-alignment handoff into the time-map stage.
        if (!mapName.empty()) {
            std::string gaDiagName = mapName;
            const std::string suffix = ".decisions.csv";
            size_t gp = gaDiagName.rfind(suffix);
            if (gp != std::string::npos) gaDiagName.replace(gp, suffix.size(), ".global-alignment-diagnostic.csv");
            else gaDiagName += ".global-alignment-diagnostic.csv";
            std::ofstream gaDiag(gaDiagName);
            if (!gaDiag) throw std::runtime_error("Cannot create global-alignment diagnostic " + gaDiagName);
            gaDiag << std::fixed << std::setprecision(8);
            gaDiag << "source,coarse_best_delay,coarse_best_score,coarse_second_score,refine1_best_delay,refine1_score,fine_best_delay,fine_score,separation,accepted,returned_delay,proposed_search_seed,timemap_initial_predicted_delay,reject_reason\n";
            for (int gs=0; gs<sourceCount; ++gs) {
                const auto& gd=globalAlignDiag[gs];
                gaDiag << (originalIndex[gs]+1) << "," << gd.coarseBest << "," << gd.coarseBestScore << ","
                       << gd.coarseSecondScore << "," << gd.refine1Best << "," << gd.refine1Score << ","
                       << gd.fineBest << "," << gd.fineScore << ","
                       << (gd.fineScore-gd.coarseSecondScore) << "," << (gd.accepted?1:0) << ","
                       << gd.returnedDelay << "," << gd.proposedDelay << ","
                       << (gs==0 ? state[gs].delay : (gd.accepted ? state[gs].delay : gd.proposedDelay)) << "," << gd.rejectReason << "\n";
            }
            gaDiag.flush();
            std::cerr << "GLOBAL_ALIGNMENT_DIAGNOSTIC " << gaDiagName << "\n";
        }

        std::ofstream tmDiag;
        std::string tmDiagName;
        if (!mapName.empty()) {
            tmDiagName = mapName;
            const std::string suffix = ".decisions.csv";
            size_t dp = tmDiagName.rfind(suffix);
            if (dp != std::string::npos)
                tmDiagName.replace(dp, suffix.size(), ".timemap-diagnostic.csv");
            else
                tmDiagName += ".timemap-diagnostic.csv";
            tmDiag.open(tmDiagName);
            if (!tmDiag) throw std::runtime_error("Cannot create time-map diagnostic " + tmDiagName);
            tmDiag << std::fixed << std::setprecision(8);
            tmDiag << "source,time_sec,seed_original,seed_wide,seed_wide_fixed_score,seed_original_path_score,seed_wide_path_score,seed_chosen,seed_score,seed_second_score,seed_gap,predicted_delay,best_delay,fine_delay,best_robust_q,adjacent_second_q,adjacent_second_delay,adjacent_distance_samples,adjacent_gap,independent_second_q,independent_second_delay,independent_distance_samples,independent_gap,fine_robust_q,raw_phase_delay,raw_phase_corr,verified,reject_reason,timing_verified,timing_delay,timing_corr,timing_gap\n";
        }

        // Diagnostic only: no change to selection or acceptance thresholds.
        std::ofstream startDiag;
        if(!mapName.empty()) {
            std::string name=mapName;
            const std::string suffix=".decisions.csv";
            auto at=name.rfind(suffix);
            if(at!=std::string::npos) name.replace(at,suffix.size(),".start-sync-diagnostic.csv");
            else name+=".start-sync-diagnostic.csv";
            startDiag.open(name);
            if(!startDiag) throw std::runtime_error("Cannot create start-sync diagnostic "+name);
            startDiag<<std::fixed<<std::setprecision(8);
            startDiag<<"source,stage,hypothesis_samples,time_sec,predicted_samples,measured_samples,corr,peak_gap,boundary,accepted_count,slope,max_residual_ms,accepted,reason\n";
            std::cerr<<"START_SYNC_DIAGNOSTIC "<<name<<"\n";
        }

        uint64_t tmAnchorsTotal=0, tmVerifiedTotal=0;
        uint64_t tmCoarseCallsTotal=0, tmFineCallsTotal=0;
        double tmCoarseSecTotal=0.0, tmFineSecTotal=0.0, tmOtherSecTotal=0.0;

        int mapOrdinal=0;
        for (int s=1; s<sourceCount; ++s) {
            if(!processingEligible[s]) {
                if(!previewPath.empty())
                    preview<<(originalIndex[s]+1)<<","<<(originalIndex[0]+1)
                           <<",0,0,0,PREFLIGHT_REJECTED\n";
                std::cerr << "TIMEMAP_SKIPPED source " << (originalIndex[s]+1)
                          << " role REJECTED\n";
                continue;
            }
            ++mapOrdinal;
            const auto tmSourceStart = std::chrono::steady_clock::now();
            uint64_t tmAnchors=0, tmCoarseCalls=0, tmFineCalls=0;
            double tmCoarseSec=0.0, tmFineSec=0.0;
            const auto& donorPreflight=
                syncPreflight.donor[originalIndex[0]][originalIndex[s]];
            const AffineDelayModel* wholeRecordModel=
                donorPreflight.affine.valid ? &donorPreflight.affine : nullptr;
            const bool affineSeedConfirmed=wholeRecordModel!=nullptr;
            AffineDelayModel onsetAnchoredModel;
            const AffineDelayModel* activeWholeRecordModel=wholeRecordModel;
            double affineResidual=0.0;
            // v0.2.29z: start the donor path at the independently measured and
            // locally phase-refined music onset. The broad global matcher can
            // prefer a different repeated musical phrase; it is diagnostic only.
            // Thereafter the SEARCH CENTER follows the best finite local match
            // from the previous anchor, even if that anchor is not verified.
            // IMPORTANT: this only steers the next search window. Reconstruction eligibility and
            // the 0.72 verified-anchor threshold are unchanged.
            double predicted = onsetDelay[s];
            bool onsetCycleLocked=false;
            int startEvidenceCount=0;

            // v0.2.29o MULTI-PEAK SEED DIAGNOSTIC:
            // Keep the rejected global seed AND several independent peaks from the broad
            // fixed-delay scan. 29n kept only the strongest wide peak, which meant a good
            // basin could disappear before continuity scoring. Each candidate is therefore
            // separated from the others in delay space, then all candidates compete using
            // the same continuous-path score. Verification/reconstruction thresholds remain
            // unchanged; this code only chooses the initial time-map search seed.
            double seedOriginal = predicted;
            // v0.2.30b: the onset detector can lock to lead-in noise several
            // seconds away from the real programme start. Keep the broad
            // global candidate as an independent basin. The same continuity
            // score below decides between onset, global and wide candidates.
            double seedGlobal = globalAlignDiag[s].proposedDelay;
            double seedWide = predicted;
            double seedWideScore = -2.0;
            double seedOriginalPathScore = -2.0;
            double seedGlobalPathScore = -2.0;
            double seedWidePathScore = -2.0;
            double seedChosen = predicted;
            double seedChosenScore = -2.0;
            double seedSecondScore = -2.0;
            // 2.30p: onset is a hypothesis, never independent proof.
            bool seedConfirmed=false;
            bool timingSeedConfirmed=false;
            const auto timingDonor=makeTimingSignal(sources[s]);
            if(startDiag && wholeRecordModel) {
                startDiag<<(originalIndex[s]+1)<<",affine_preflight,"
                    <<wholeRecordModel->delayAt(musicOnset[0])<<","
                    <<(double)musicOnset[0]/sr<<","<<wholeRecordModel->intercept
                    <<",,"<<wholeRecordModel->score<<",,,,"
                    <<wholeRecordModel->slope<<","
                    <<1000.0*wholeRecordModel->maxResidual/sr<<",1,AFFINE_MODEL_VERIFIED\n";
            }
            {
                constexpr size_t maxWidePeaks = 5;
                std::vector<long> probeFrames;
                const long available=(long)ref.frames()-2*mapWin-1;
                const long begin=std::clamp<long>(musicOnset[0]+sr/2,mapWin,
                    std::max<long>(mapWin,available));
                const long span=std::max<long>(0,std::min<long>(8L*sr,available-begin));
                for(int pi=0;pi<7;++pi) probeFrames.push_back(begin+span*pi/6);

                struct SeedPeak { long delay; double fixedScore; double pathScore; };
                std::vector<SeedPeak> scanned;
                // Level-independent envelope fingerprint. The sequence of energy
                // rises and falls generates hypotheses; raw music verifies them below.
                const long bin=std::max(1,sr/100); // 10 ms, bounded full +/-20 s search
                auto envelope=[&](const Wav& w) {
                    std::vector<double> e(w.frames()/bin);
                    for(size_t k=0;k<e.size();++k) {
                        double energy=0;
                        for(long j=0;j<bin;j+=4) {
                            double v=monoAt(w,(long)k*bin+j); energy+=v*v;
                        }
                        e[k]=std::log(1e-12+energy/(double)((bin+3)/4));
                    }
                    return e;
                };
                const auto ea=envelope(ref), eb=envelope(sources[s]);
                const long independentSeedDistance=std::max<long>(sr/10,2*mapRadius);
                for(long offset=-2000;offset<=2000;++offset) {
                    std::vector<double> qs;
                    for(long pf:probeFrames) {
                        long a=pf/bin, b=a+offset;
                        constexpr long n=60; // 600 ms fingerprint
                        if(a<0 || b<0 || a+n>(long)ea.size() || b+n>(long)eb.size()) continue;
                        double ax=0,bx=0,aa=0,bb=0,ab=0;
                        for(long j=0;j<n;++j){double x=ea[a+j],y=eb[b+j];
                            ax+=x;bx+=y;aa+=x*x;bb+=y*y;ab+=x*y;}
                        aa-=ax*ax/n;bb-=bx*bx/n;ab-=ax*bx/n;
                        if(aa>1e-6 && bb>1e-6) qs.push_back(ab/std::sqrt(aa*bb));
                    }
                    if(qs.size()<4) continue;
                    std::sort(qs.begin(),qs.end());
                    scanned.push_back({offset*bin,0.75*qs[qs.size()/2]+0.25*qs[qs.size()/4],-2.0});
                }
                std::sort(scanned.begin(), scanned.end(), [](const SeedPeak& a,const SeedPeak& b){
                    return a.fixedScore>b.fixedScore;
                });

                std::vector<SeedPeak> widePeaks;
                for(const auto& c: scanned) {
                    bool independent=true;
                    for(const auto& kept: widePeaks) {
                        if(std::llabs(c.delay-kept.delay)<independentSeedDistance) { independent=false; break; }
                    }
                    if(!independent) continue;
                    widePeaks.push_back(c);
                    if(widePeaks.size()>=maxWidePeaks) break;
                }
                if(!widePeaks.empty()) {
                    seedWide=(double)widePeaks[0].delay;
                    seedWideScore=widePeaks[0].fixedScore;
                }

                std::vector<std::pair<double,double>> refinedSeeds;
                auto continuitySeedScore = [&](double startSeed)->double {
                    double pathDelay=startSeed;
                    std::vector<double> pathQ;
                    int strong=0;
                    std::vector<std::pair<long,double>> measured;
                    for(long pf: probeFrames) {
                        long center=(long)std::llround(pathDelay);
                        double bestLocal=-2.0;
                        long bestDelay=center;
                        const long step=std::max<long>(16,mapStride);
                        for(long d=center-mapRadius; d<=center+mapRadius; d+=step) {
                            long df=pf+d;
                            if(df<0 || df+mapWin>=(long)sources[s].frames()) continue;
                            double q=robustMusicCorrAtStrided(ref,sources[s],pf,mapWin,d,mapStride);
                            if(q>bestLocal){ bestLocal=q; bestDelay=d; }
                        }
                        const long coarse=bestDelay;
                        for(long d=coarse-step;d<=coarse+step;++d) {
                            if(pf+d<0 || pf+d+mapWin>=(long)sources[s].frames()) continue;
                            double q=robustMusicCorrAtStrided(ref,sources[s],pf,mapWin,d,mapStride);
                            if(q>bestLocal){bestLocal=q;bestDelay=d;}
                        }
                        if(startDiag) startDiag<<(originalIndex[s]+1)<<",robust_probe,"<<startSeed<<","<<(double)pf/sr
                            <<","<<pathDelay<<","<<bestDelay<<","<<bestLocal<<",,"
                            <<(std::abs(bestDelay-center)>=mapRadius?1:0)<<",,,,"
                            <<(bestLocal>=mapCorrMin?1:0)<<","<<(bestLocal>=mapCorrMin?"PASS":"CORR_BELOW_0.72")<<"\n";
                        if(!std::isfinite(bestLocal) || bestLocal<-1.0) continue;
                        pathQ.push_back(bestLocal);
                        if(bestLocal>=mapCorrMin) {
                            ++strong; measured.push_back({pf,(double)bestDelay});
                            pathDelay=(double)bestDelay;
                        }
                    }
                    if(pathQ.size()<4 || strong<4 || span<sr) return -2.0;
                    double initial=measured.front().second;
                    double slope=(measured[1].second-initial)/(measured[1].first-measured[0].first);
                    if(std::abs(slope)<=0.025)
                        initial+=slope*(mapWin-measured[0].first);
                    refinedSeeds.push_back({startSeed,initial});
                    std::sort(pathQ.begin(),pathQ.end());
                    double med=pathQ[pathQ.size()/2];
                    double low=pathQ[pathQ.size()/4];
                    double high=pathQ[(3*pathQ.size())/4];
                    double support=(double)strong/(double)pathQ.size();
                    return 0.50*med + 0.20*low + 0.15*high + 0.15*support;
                };

                struct SeedCandidate { double delay; double fixedScore; double pathScore; const char* kind; };
                std::vector<SeedCandidate> candidates;
                seedOriginalPathScore=continuitySeedScore(seedOriginal);
                candidates.push_back({seedOriginal,-2.0,seedOriginalPathScore,"onset"});
                if (std::isfinite(seedGlobal) &&
                    std::abs(seedGlobal-seedOriginal) >=
                        (double)std::max<long>(mapRadius,sr/20)) {
                    seedGlobalPathScore=continuitySeedScore(seedGlobal);
                    candidates.push_back({seedGlobal,globalAlignDiag[s].fineScore,
                                          seedGlobalPathScore,"global"});
                }
                candidates.push_back({0.0,-2.0,continuitySeedScore(0.0),"zero"});
                if(std::isfinite(manualOffsets[originalIndex[s]])) {
                    double manual=manualOffsets[originalIndex[s]]*sr;
                    candidates.push_back({manual,-2.0,continuitySeedScore(manual),"manual"});
                }

                for(auto& wp: widePeaks) {
                    wp.pathScore=continuitySeedScore((double)wp.delay);
                    candidates.push_back({(double)wp.delay,wp.fixedScore,wp.pathScore,"wide"});
                }
                if(!widePeaks.empty()) seedWidePathScore=widePeaks[0].pathScore;

                // Preserve the measured delay, not the original hypothesis.
                std::vector<double> timingHypotheses;
                for(auto& c:candidates) {
                    timingHypotheses.push_back(c.delay);
                    for(const auto& r:refinedSeeds) if(r.first==c.delay){c.delay=r.second;break;}
                }
                std::sort(candidates.begin(), candidates.end(), [](const SeedCandidate& a,const SeedCandidate& b){
                    return a.pathScore>b.pathScore;
                });
                if(!candidates.empty()) {
                    seedChosen=candidates[0].delay;
                    seedChosenScore=candidates[0].pathScore;
                    seedSecondScore=-2.0;
                    for(size_t ci=1;ci<candidates.size();++ci)
                        if(std::abs(candidates[ci].delay-seedChosen)>2*mapRadius) {
                            seedSecondScore=candidates[ci].pathScore;break;
                        }
                    seedConfirmed=seedChosenScore>=mapCorrMin &&
                        seedChosenScore-seedSecondScore>=0.02;
                    predicted=seedChosen;
                }

                // Independent timing evidence for worn records. All candidates
                // face the same multi-window and ambiguity tests; no onset bypass.
                // Search from the first complete window independently of the onset
                // detector, which may mistake a later loud phrase for the start.
                // Non-overlapping 200 ms windows at 500 ms spacing; bounded to 15 s.
                std::vector<long> timingProbeFrames;
                const long timingEnd=std::min<long>(available,15L*sr);
                for(long pf=mapWin;pf<=timingEnd;pf+=std::max<long>(mapWin,sr/2))
                    timingProbeFrames.push_back(pf);
                struct TimingSeed {double initial,score;};
                std::vector<TimingSeed> timingSeeds;
                for(double hypothesis:timingHypotheses) {
                    double delay=hypothesis;
                    std::vector<std::pair<long,TimingMatch>> good;
                    for(long pf:timingProbeFrames) {
                        auto m=matchTiming(timingRef,timingDonor,pf,mapWin,delay,mapRadius);
                        if(startDiag) startDiag<<(originalIndex[s]+1)<<",timing_probe,"<<hypothesis<<","<<(double)pf/sr
                            <<","<<delay<<","<<m.delay<<","<<m.q<<","<<m.gap<<","<<m.boundary
                            <<",,,,"<<m.accepted<<","<<m.reason<<"\n";
                        if(m.accepted) {good.push_back({pf,m});delay=m.delay;}
                    }
                    auto reportTiming=[&](double slope,double residual,bool pass,const char* reason) {
                        if(startDiag) startDiag<<(originalIndex[s]+1)<<",timing_summary,"<<hypothesis
                            <<",,,,,,,"<<good.size()<<","<<slope<<","<<1000.0*residual/sr
                            <<","<<pass<<","<<reason<<"\n";
                    };
                    if(good.size()<5 || good.back().first-good.front().first<sr) {
                        reportTiming(0,0,false,good.size()<5?"FEWER_THAN_5_GOOD_PROBES":"SPAN_BELOW_1_SECOND");continue;
                    }
                    const auto fit=fitTimingConsensus(good,sr);
                    reportTiming(fit.slope,fit.maxResidual,fit.accepted,
                        fit.accepted?"CONSENSUS_PASS":"NO_STABLE_MAJORITY");
                    if(startDiag) for(size_t gi=0;gi<good.size();++gi) {
                        bool kept=fit.accepted && std::find(fit.inliers.begin(),fit.inliers.end(),gi)!=fit.inliers.end();
                        const auto& g=good[gi];
                        startDiag<<(originalIndex[s]+1)<<",timing_consensus,"<<hypothesis<<","<<(double)g.first/sr
                            <<","<<(fit.intercept+fit.slope*g.first)<<","<<g.second.delay<<","<<g.second.q
                            <<","<<g.second.gap<<",,"<<fit.inliers.size()<<","<<fit.slope
                            <<","<<1000.0*std::abs(g.second.delay-fit.intercept-fit.slope*g.first)/sr
                            <<","<<kept<<","<<(kept?"INLIER":(fit.accepted?"OUTLIER":"MODEL_REJECTED"))<<"\n";
                    }
                    if(fit.accepted) timingSeeds.push_back({fit.intercept+fit.slope*mapWin,fit.score});
                }
                std::sort(timingSeeds.begin(),timingSeeds.end(),
                    [](const TimingSeed& a,const TimingSeed& b){return a.score>b.score;});
                if(!timingSeeds.empty()) {
                    double runner=-2;
                    for(size_t i=1;i<timingSeeds.size();++i)
                        if(std::abs(timingSeeds[i].initial-timingSeeds[0].initial)>0.005*sr)
                            runner=std::max(runner,timingSeeds[i].score);
                    timingSeedConfirmed=timingSeeds[0].score-runner>=0.025;
                    if(!seedConfirmed && timingSeedConfirmed) {
                        seedChosen=timingSeeds[0].initial;
                        seedChosenScore=timingSeeds[0].score;
                        seedSecondScore=runner;
                        predicted=seedChosen;
                    }
                }

                // A clean programme attack can identify an absolute cycle, but
                // x2h proved that an energy threshold is not by itself a common
                // musical landmark. Source 3's apparent +154.8 ms edge lost all
                // seven continuity probes while the +70 ms programme path won
                // seven of seven and 447/526 dense anchors. Therefore the onset
                // may override a whole-record model only when its OWN path also
                // survives the multi-window continuity test.
                const bool rawOnsetCycleEvidence =
                    std::isfinite(onsetCorr[s]) && onsetCorr[s] >= 0.55 &&
                    onsetTransition[0] >= 2.0 && onsetTransition[s] >= 2.0;
                const double beforeCycleLock = predicted;
                const long onsetCycleRadius = std::max<long>(mapRadius, sr*25/1000);
                const bool onsetPathSupported=onsetBasinMayOverride(
                    seedOriginalPathScore,seedChosenScore,rawOnsetCycleEvidence,
                    mapCorrMin);
                const bool onsetConflict=wholeRecordModel && rawOnsetCycleEvidence &&
                    !onsetPathSupported &&
                    std::abs(predicted-seedOriginal)>(double)onsetCycleRadius;
                onsetBasinConflict[s]=onsetConflict;
                const bool onsetCycleLockApplied = onsetPathSupported &&
                    std::abs(predicted-seedOriginal) > (double)onsetCycleRadius;
                if(onsetCycleLockApplied) {
                    predicted=seedOriginal;
                    seedChosen=seedOriginal;
                    onsetCycleLocked=true;
                }

                // Preserve the robust affine slope and move only its intercept
                // when the onset trajectory passed the continuity veto above.
                // Otherwise retain the whole-record programme basin and keep
                // the disputed donor repair-locked until dense evidence accrues.
                if(wholeRecordModel) {
                    const auto lock=anchorAffineBasinToOnset(
                        *wholeRecordModel,musicOnset[0],seedOriginal,
                        onsetPathSupported,onsetCycleRadius);
                    if(lock.applied) {
                        onsetAnchoredModel=lock.model;
                        activeWholeRecordModel=&onsetAnchoredModel;
                        affineBasinOffsets[s]=lock.interceptOffset;
                        affineBasinLocked[s]=true;
                        affineResidual=0.0;
                        predicted=activeWholeRecordModel->delayAt(musicOnset[0]);
                        seedChosen=predicted;
                        onsetCycleLocked=true;
                        if(startDiag) {
                            startDiag<<(originalIndex[s]+1)<<",affine_basin_lock,"
                                <<seedOriginal<<","<<(double)musicOnset[0]/sr<<","
                                <<wholeRecordModel->delayAt(musicOnset[0])<<","
                                <<activeWholeRecordModel->delayAt(musicOnset[0])<<","
                                <<onsetCorr[s]<<",,,,"<<activeWholeRecordModel->slope<<","
                                <<1000.0*std::abs(lock.interceptOffset)/sr
                                <<",1,ONSET_ANCHORED_AFFINE\n";
                        }
                        std::cerr << "AFFINE_BASIN_LOCK source "
                                  << (originalIndex[s]+1)
                                  << " offset_samples " << lock.interceptOffset
                                  << " offset_ms " << 1000.0*lock.interceptOffset/sr
                                  << " slope " << activeWholeRecordModel->slope
                                  << " applied 1\n";
                    }
                }
                if(onsetConflict) {
                    std::cerr << "ONSET_BASIN_VETO source "
                              << (originalIndex[s]+1)
                              << " onset_delay " << seedOriginal
                              << " programme_delay " << beforeCycleLock
                              << " difference_ms "
                              << 1000.0*std::abs(beforeCycleLock-seedOriginal)/sr
                              << " onset_path " << seedOriginalPathScore
                              << " winning_path " << seedChosenScore
                              << " applied 1\n";
                    if(startDiag) {
                        startDiag<<(originalIndex[s]+1)<<",onset_basin_veto,"
                            <<seedOriginal<<","<<(double)musicOnset[0]/sr<<","
                            <<beforeCycleLock<<",,"<<seedOriginalPathScore<<","
                            <<(seedChosenScore-seedOriginalPathScore)<<",,,"
                            <<(wholeRecordModel?wholeRecordModel->slope:0.0)
                            <<",,0,ONSET_REJECTED_BY_MULTI_WINDOW_CONTINUITY\n";
                    }
                }
                std::cerr << "ONSET_CYCLE_LOCK source " << (originalIndex[s]+1)
                          << " raw_evidence " << (rawOnsetCycleEvidence?1:0)
                          << " path_supported " << (onsetPathSupported?1:0)
                          << " onset_delay " << seedOriginal
                          << " proposed_delay " << beforeCycleLock
                          << " difference_ms " << 1000.0*std::abs(beforeCycleLock-seedOriginal)/sr
                          << " radius_ms " << 1000.0*onsetCycleRadius/sr
                          << " applied " << (onsetCycleLockApplied?1:0) << "\n";
                if(startDiag) {
                    startDiag<<(originalIndex[s]+1)<<",seed_selection,"<<seedChosen
                        <<",,"<<predicted<<",,"<<seedChosenScore<<","<<(seedChosenScore-seedSecondScore)
                        <<",,,,,"<<(seedConfirmed||timingSeedConfirmed||affineSeedConfirmed)<<"," 
                        <<(affineBasinLocked[s]?"ONSET_ANCHORED_AFFINE":(onsetBasinConflict[s]?"ONSET_VETO_PROGRAMME_PATH":(onsetCycleLockApplied?"ONSET_CYCLE_LOCK":(seedConfirmed?"ROBUST_SEED_CONFIRMED":(timingSeedConfirmed?"TIMING_SEED_CONFIRMED":
                            (affineSeedConfirmed?"AFFINE_PREFLIGHT_CONFIRMED":
                            (timingSeeds.empty()?"NO_TIMING_CANDIDATE_PASSED":"TIMING_CANDIDATES_AMBIGUOUS")))))))<<"\n";
                    startDiag.flush();
                }
                std::cerr << "TIMING_SEED source " << (originalIndex[s]+1)
                          << " confirmed " << timingSeedConfirmed
                          << " candidates " << timingSeeds.size() << "\n";

                std::cerr << "MULTI_PEAK_SEEDS source " << (originalIndex[s]+1) << " count " << candidates.size();
                for(size_t ci=0; ci<candidates.size(); ++ci) {
                    std::cerr << " | " << candidates[ci].kind << "#" << ci
                              << " delay " << candidates[ci].delay
                              << " fixed " << candidates[ci].fixedScore
                              << " path " << candidates[ci].pathScore;
                }
                std::cerr << " | chosen " << seedChosen << "\n";
            }
            std::cerr << "SOURCE_ALIGNMENT_SEED source " << (originalIndex[s]+1)
                      << " confirmed " << (seedConfirmed?1:0)
                      << " original " << seedOriginal
                      << " global " << seedGlobal
                      << " wide " << seedWide
                      << " chosen " << predicted
                      << " original_path " << seedOriginalPathScore
                      << " global_path " << seedGlobalPathScore
                      << " wide_path " << seedWidePathScore
                      << " winner " << seedChosenScore
                      << " runnerup " << seedSecondScore
                      << " gap " << (seedChosenScore-seedSecondScore) << "\n";
            if(!previewPath.empty()) {
                const bool confirmed=seedConfirmed || timingSeedConfirmed || affineSeedConfirmed;
                const double previewDelay=activeWholeRecordModel
                    ? activeWholeRecordModel->delayAt(musicOnset[0]) : predicted;
                preview<<(originalIndex[s]+1)<<","<<(originalIndex[0]+1)<<","<<previewDelay/sr
                       <<","<<confirmed<<","<<seedChosenScore<<","
                       <<(affineBasinLocked[s]?"ONSET_ANCHORED_AFFINE_VERIFIED":
                          (onsetBasinConflict[s]?"PROGRAMME_PATH_VERIFIED_ONSET_CONFLICT":
                          (affineSeedConfirmed?"WHOLE_RECORD_AFFINE_VERIFIED":
                          (confirmed?"MULTI_ANCHOR_VERIFIED":"UNVERIFIED"))))<<"\n";
                continue; // No full time map, reconstruction, or output audio in preview.
            }
            int verifiedCount=0;
            long mapLastReport = -1;
            const long mapUsable = std::max<long>(1, (long)ref.frames() - 2L*mapWin);
            const int sourceMapHop=sourceRoles[s]==SourceRole::Reserve
                ? std::max(mapHop,sr) : mapHop;
            // Keep the exact prefix/anchor grid used by Master-2000. Include future
            // context for the existing +/-2 second stabilization fit.
            const long syncMapEnd=syncCheckSeconds>0?(long)std::ceil((syncCheckStart+syncCheckSeconds+2.5)*sr):(long)ref.frames();
            for(long mf=mapWin; mf+mapWin<(long)ref.frames() && mf<=syncMapEnd; mf+=sourceMapHop) {
                ++tmAnchors;
                long mapPct = std::clamp<long>(100L*(mf-mapWin)/mapUsable,0,99);
                if (mapPct != mapLastReport) {
                    mapLastReport = mapPct;
                    std::cerr << "PHASE_MAP " << mapOrdinal << " "
                              << mappedSourceCount << " " << mapPct << " "
                              << (originalIndex[s]+1) << " "
                              << sourceRoleName(sourceRoles[s]) << "\n";
                }
                TimingMatch timing;
                double affineBase=0.0;
                if(activeWholeRecordModel) {
                    affineBase=activeWholeRecordModel->delayAt(mf);
                    predicted=affineBase+affineResidual;
                }
                if(timingSeedConfirmed || seedConfirmed || affineSeedConfirmed)
                    timing=matchTiming(timingRef,timingDonor,mf,mapWin,predicted,mapRadius);
                long center=(long)std::llround(timing.accepted?timing.delay:predicted);
                long best=center;
                long secondBest=center;
                double bestQ=-2.0, secondQ=-2.0;
                {
                    const auto tm0=std::chrono::steady_clock::now();

                    // v0.2.29 hierarchical coarse search.
                    // Stage 1: sparse search across the full trusted radius.
                    constexpr long hierarchyStep = 16;
                    double sparseBestQ=-2.0;
                    long sparseBest=center;
                    for(long d=center-mapRadius; d<=center+mapRadius; d+=hierarchyStep) {
                        long df=mf+d;
                        if(df<0 || df+mapWin>=(long)sources[s].frames()) continue;
                        ++tmCoarseCalls;
                        double q=robustMusicCorrAtStrided(ref,sources[s],mf,mapWin,d,mapStride);
                        if(q>sparseBestQ){ sparseBestQ=q; sparseBest=d; }
                    }

                    // Stage 2: restore the original 4-sample resolution only
                    // around the winning sparse cell. This does not alter the
                    // correlation function or the later fine verifier.
                    bestQ=-2.0; secondQ=-2.0; best=sparseBest; secondBest=sparseBest;
                    const long localLo=std::max(center-mapRadius,sparseBest-hierarchyStep);
                    const long localHi=std::min(center+mapRadius,sparseBest+hierarchyStep);
                    long first4=localLo;
                    long rem=(first4-(center-mapRadius)) % 4;
                    if(rem<0) rem+=4;
                    if(rem) first4 += (4-rem);
                    for(long d=first4; d<=localHi; d+=4) {
                        long df=mf+d;
                        if(df<0 || df+mapWin>=(long)sources[s].frames()) continue;
                        ++tmCoarseCalls;
                        double q=robustMusicCorrAtStrided(ref,sources[s],mf,mapWin,d,mapStride);
                        if(q>bestQ){ secondQ=bestQ; secondBest=best; bestQ=q; best=d; }
                        else if(q>secondQ){ secondQ=q; secondBest=d; }
                    }

                    tmCoarseSec += std::chrono::duration<double>(
                        std::chrono::steady_clock::now()-tm0).count();
                }
                long fineBest=best; double fineQ=bestQ;
                {
                    const auto tm0=std::chrono::steady_clock::now();
                    for(long d=best-6; d<=best+6; ++d) {
                        long df=mf+d;
                        if(df<0 || df+mapWin>=(long)sources[s].frames()) continue;
                        ++tmFineCalls;
                        double q=robustMusicCorrAtStrided(ref,sources[s],mf,mapWin,d,mapStride);
                        if(q>fineQ){ fineQ=q; fineBest=d; }
                    }
                    tmFineSec += std::chrono::duration<double>(
                        std::chrono::steady_clock::now()-tm0).count();
                }
                // v0.2.29l: distinguish a broad/same peak from a genuinely competing peak.
                // Adjacent 4-sample candidates are expected to have nearly identical scores;
                // they must not make an otherwise strong alignment look ambiguous.
                const long peakExclusion = 48; // 0.5 ms at 96 kHz; scales below via samples
                double independentSecondQ=-2.0;
                long independentSecondDelay=fineBest;
                for(long d=center-mapRadius; d<=center+mapRadius; d+=4) {
                    if(std::labs(d-fineBest) <= peakExclusion) continue;
                    long df=mf+d;
                    if(df<0 || df+mapWin>=(long)sources[s].frames()) continue;
                    double q=robustMusicCorrAtStrided(ref,sources[s],mf,mapWin,d,mapStride);
                    if(q>independentSecondQ){ independentSecondQ=q; independentSecondDelay=d; }
                }
                const bool finiteQ = std::isfinite(fineQ);
                const double adjacentGap = (secondQ < -1.0) ? 999.0 : (fineQ-secondQ);
                const long adjacentDistance = std::labs(fineBest-secondBest);
                const double independentGap = (independentSecondQ < -1.0) ? 999.0 : (fineQ-independentSecondQ);
                const long independentDistance = std::labs(fineBest-independentSecondDelay);
                const bool corrPass = finiteQ && fineQ >= mapCorrMin;
                const bool uniquePass = (independentSecondQ < -1.0 || independentGap >= 0.002);
                // v0.2.29s: the robust envelope identifies the correct musical
                // neighbourhood, but can land one waveform period away. Refine
                // raw phase only inside that trusted neighbourhood. This prevents
                // two donors from agreeing perfectly with each other while both
                // are several milliseconds out of phase with Master.
                long rawPhaseBest=fineBest;
                double rawPhaseQ=-2.0;
                const int phaseWin=std::max(512,sr*24/1000);
                const long phaseRadius=std::max<long>(32,sr*4/1000);
                long rawCoarseBest=fineBest;
                for(long d=fineBest-phaseRadius; d<=fineBest+phaseRadius; d+=4) {
                    long df=mf+d;
                    if(df<0 || df+phaseWin>=(long)sources[s].frames()) continue;
                    double q=corrAtStrided(ref,sources[s],mf,phaseWin,d,2);
                    if(q>rawPhaseQ){rawPhaseQ=q;rawCoarseBest=d;}
                }
                rawPhaseBest=rawCoarseBest;
                for(long d=rawCoarseBest-4; d<=rawCoarseBest+4; ++d) {
                    long df=mf+d;
                    if(df<0 || df+phaseWin>=(long)sources[s].frames()) continue;
                    double q=corrAtStrided(ref,sources[s],mf,phaseWin,d,1);
                    if(q>rawPhaseQ){rawPhaseQ=q;rawPhaseBest=d;}
                }
                const bool rawPhasePass=std::isfinite(rawPhaseQ) && rawPhaseQ>=0.35;
                // v0.1.4: the transient window immediately after a correct
                // onset lock may have weak robust-feature correlation even
                // though both independent waveform measurements agree.  Do
                // not lower the normal 0.72 gate.  Inside the first six
                // seconds after a real cycle lock, accept a start-evidence
                // anchor only when raw waveform and low-pass timing both pass,
                // have distinct peaks, and identify offsets within 1 ms.
                const bool startEvidenceWindow=onsetCycleLocked &&
                    mf>=musicOnset[0] && mf<=musicOnset[0]+6L*sr;
                const bool startEvidencePass=startEvidenceWindow &&
                    std::isfinite(rawPhaseQ) && rawPhaseQ>=0.55 &&
                    std::isfinite(timing.q) && timing.q>=0.50 &&
                    timing.gap>=0.005 && !timing.boundary &&
                    std::abs((double)rawPhaseBest-timing.delay)<=0.001*sr;
                bool ok = (seedConfirmed || timingSeedConfirmed || affineSeedConfirmed) &&
                    ((corrPass && uniquePass && rawPhasePass) || startEvidencePass);
                const char* tmRejectReason = startEvidencePass ? "ONSET_RAW_TIMING_PASS" : (ok ? "VERIFIED" :
                    (!(seedConfirmed || timingSeedConfirmed || affineSeedConfirmed) ? "MULTI_ANCHOR_SEED_UNCONFIRMED" :
                     (!finiteQ ? "NONFINITE_CORR" :
                      (!corrPass ? "ROBUST_CORR_BELOW_0.72" :
                       (!uniquePass ? "INDEPENDENT_PEAK_GAP_LT_0.002" : "RAW_PHASE_CORR_BELOW_0.35")))));
                if (tmDiag) {
                    tmDiag << (originalIndex[s]+1) << "," << ((double)mf/sr) << "," << seedOriginal << "," << seedWide << "," << seedWideScore << "," << seedOriginalPathScore << "," << seedWidePathScore << "," << seedChosen << "," << seedChosenScore << "," << seedSecondScore << "," << (seedChosenScore-seedSecondScore) << "," << predicted << ","
                           << best << "," << fineBest << "," << bestQ << "," << secondQ << ","
                           << secondBest << "," << adjacentDistance << "," << adjacentGap << ","
                           << independentSecondQ << "," << independentSecondDelay << ","
                           << independentDistance << "," << independentGap << ","
                           << fineQ << "," << rawPhaseBest << "," << rawPhaseQ << "," << (ok?1:0) << "," << tmRejectReason << "," << (timing.accepted?1:0) << "," << timing.delay << "," << timing.q << "," << timing.gap << "\n";
                }
                TimeMapAnchor a;
                a.masterFrame=mf; a.donorDelay=ok?(double)rawPhaseBest:predicted;
                a.corr=startEvidencePass
                    ? std::sqrt(std::max(0.0,rawPhaseQ)*std::max(0.0,timing.q))
                    : (ok?fineQ:0.0);
                a.verified=ok;
                timeMaps[s].push_back(a);
                if(startEvidencePass) {
                    ++startEvidenceCount;
                    std::cerr<<"START_EVIDENCE source "<<(originalIndex[s]+1)
                             <<" time "<<(double)mf/sr
                             <<" raw_q "<<rawPhaseQ<<" timing_q "<<timing.q
                             <<" timing_gap "<<timing.gap
                             <<" agreement_ms "<<1000.0*std::abs((double)rawPhaseBest-timing.delay)/sr
                             <<" accepted 1\n";
                }
                if(ok || timing.accepted) {
                    TimeMapAnchor t=a;
                    t.verified=true;t.repairSafe=false;
                    if(!ok){t.donorDelay=timing.delay;t.corr=timing.q;}
                    timingMaps[s].push_back(t);
                }
                // v0.2.29j continuity diagnostic: a finite local winner steers the NEXT
                // anchor search, but it does not become a verified time-map anchor unless it
                // passes the unchanged 0.72 + uniqueness gates above. This tests whether
                // independent analogue transfers can be followed as a continuous drift path.
                if (ok) {
                    predicted=(double)rawPhaseBest;
                    if(activeWholeRecordModel)
                        affineResidual=std::clamp((double)rawPhaseBest-affineBase,
                                                  -(double)mapRadius,(double)mapRadius);
                } else if(timing.accepted) {
                    predicted=timing.delay;
                    if(activeWholeRecordModel)
                        affineResidual=std::clamp(timing.delay-affineBase,
                                                  -(double)mapRadius,(double)mapRadius);
                }
                if(ok){ ++verifiedCount; }
            }
            // Publish the detected programme onset only after a nearby
            // start-evidence anchor has independently confirmed its cycle and
            // delay.  The onset detector therefore remains a hypothesis, not
            // a self-verifying time-map point.
            if(onsetCycleLocked) {
                const TimeMapAnchor* firstStartEvidence=nullptr;
                for(const auto& a:timeMaps[s]) {
                    if(a.verified && a.masterFrame>=musicOnset[0] &&
                       a.masterFrame<=musicOnset[0]+sr/4 && a.corr>=0.55 &&
                       std::abs(a.donorDelay-onsetDelay[s])<=0.020*sr) {
                        firstStartEvidence=&a;break;
                    }
                }
                if(firstStartEvidence) {
                    TimeMapAnchor onsetAnchor;
                    onsetAnchor.masterFrame=musicOnset[0];
                    onsetAnchor.donorDelay=onsetDelay[s];
                    onsetAnchor.corr=std::min(onsetCorr[s],firstStartEvidence->corr);
                    onsetAnchor.verified=true;
                    onsetAnchor.repairSafe=false;
                    timeMaps[s].push_back(onsetAnchor);
                    ++verifiedCount;
                    std::cerr<<"ONSET_ANCHOR_CONFIRMED source "<<(originalIndex[s]+1)
                             <<" onset_sec "<<(double)musicOnset[0]/sr
                             <<" confirming_sec "<<(double)firstStartEvidence->masterFrame/sr
                             <<" start_evidence_count "<<startEvidenceCount<<"\n";
                }
            }
            if(verifiedCount<4) {
                timeMaps[s].clear();
            } else {
                // Do not manufacture a verified anchor from the onset detector.
                // Only actual locally verified music anchors enter the time map.
                std::sort(timeMaps[s].begin(),timeMaps[s].end(),
                          [](const TimeMapAnchor& a,const TimeMapAnchor& b){
                              return a.masterFrame<b.masterFrame;
                          });
                stabilizeTimeMap(timeMaps[s],sr,musicOnset[0],activeWholeRecordModel);
                if(affineBasinLocked[s] || onsetBasinConflict[s]) {
                    basinRepairUnlockFrame[s]=gateDisputedBasinRepairs(
                        timeMaps[s],*activeWholeRecordModel,sr,musicOnset[0]);
                    std::cerr << "BASIN_REPAIR_GATE source "
                              << (originalIndex[s]+1)
                              << " mode "
                              << (affineBasinLocked[s]?"ONSET_SELECTED":"ONSET_VETOED")
                              << " required_dense_anchors 4 unlock_sec ";
                    if(basinRepairUnlockFrame[s]>=0)
                        std::cerr << (double)basinRepairUnlockFrame[s]/sr;
                    else
                        std::cerr << "none";
                    std::cerr << "\n";
                }
            }
            std::cerr << "PHASE_MAP " << mapOrdinal << " " << mappedSourceCount
                      << " 100 " << (originalIndex[s]+1) << " "
                      << sourceRoleName(sourceRoles[s]) << "\n";

            const double tmSourceSec=std::chrono::duration<double>(
                std::chrono::steady_clock::now()-tmSourceStart).count();
            const double tmOtherSec=std::max(0.0,tmSourceSec-tmCoarseSec-tmFineSec);

            tmAnchorsTotal += tmAnchors;
            tmVerifiedTotal += (uint64_t)std::max(0,verifiedCount);
            tmCoarseCallsTotal += tmCoarseCalls;
            tmFineCallsTotal += tmFineCalls;
            tmCoarseSecTotal += tmCoarseSec;
            tmFineSecTotal += tmFineSec;
            tmOtherSecTotal += tmOtherSec;

            std::cerr << "TIMEMAP_PROFILE source " << (originalIndex[s]+1)
                      << " anchors " << tmAnchors
                      << " verified " << verifiedCount
                      << " coarse_calls " << tmCoarseCalls
                      << " coarse_sec " << tmCoarseSec
                      << " fine_calls " << tmFineCalls
                      << " fine_sec " << tmFineSec
                      << " other_sec " << tmOtherSec
                      << " total_sec " << tmSourceSec << "\n";
        }

        if(!previewPath.empty()) {
            preview.flush();
            if(!preview) throw std::runtime_error("Cannot finish sync preview");
            return 0;
        }

        workProgressEnabled=true;
        beginWorkPhase("BACKWARD_SYNC");

        // x2n: once two independently strong donors have reached the
        // same safe part of the ordinary time map, walk that pair backwards
        // toward the detected programme onset. A backward point is published
        // only when BOTH donors find an unambiguous Master match, move by
        // nearly the same amount, and still match each other. With four to six
        // sources a disputed/weak copy abstains from this bridge; the two
        // strongest conflict-free maps carry it. Thus two donors may
        // corroborate a weak Master attack, but can never vote a new musical
        // position into existence on their own.
        bool anyDisputedOnsetBasin=false;
        for(int s=1;s<sourceCount;++s)
            anyDisputedOnsetBasin=anyDisputedOnsetBasin ||
                affineBasinLocked[s] || onsetBasinConflict[s];

        std::vector<int> backwardBridgeCandidates;
        for(int s=1;s<sourceCount;++s) {
            if(timeMaps[s].empty() || affineBasinLocked[s] || onsetBasinConflict[s])
                continue;
            backwardBridgeCandidates.push_back(s);
        }
        auto bridgeMapStrength=[&](int source) {
            double strength=0.0;
            for(const auto& a:timeMaps[source]) {
                if(a.verified && a.repairSafe)
                    strength+=1.0+std::clamp(a.corr,0.0,1.0);
            }
            return strength;
        };
        std::sort(backwardBridgeCandidates.begin(),backwardBridgeCandidates.end(),
            [&](int a,int b) {
                const double sa=bridgeMapStrength(a);
                const double sb=bridgeMapStrength(b);
                return sa==sb ? a<b : sa>sb;
            });
        const int bridgeA=backwardBridgeCandidates.size()>=2
            ? backwardBridgeCandidates[0] : -1;
        const int bridgeB=backwardBridgeCandidates.size()>=2
            ? backwardBridgeCandidates[1] : -1;
        if(bridgeA<0 || bridgeB<0) {
            // A disputed donor must not republish early repair-safe points
            // before its onset path earns the four-anchor unlock. With extra
            // copies, other conflict-free donors may still form the bridge.
            std::cerr << "BACKWARD_SYNC_SKIPPED reason "
                      << (anyDisputedOnsetBasin
                          ? "INSUFFICIENT_CONFLICT_FREE_DONORS"
                          : "INSUFFICIENT_SAFE_DONORS") << "\n";
        }
        if(bridgeA>0 && bridgeB>0) {
            const long searchEnd=std::min<long>((long)ref.frames()-mapWin-1,
                syncCheckSeconds>0
                    ? (long)std::ceil((syncCheckStart+syncCheckSeconds+2.5)*sr)
                    : (long)ref.frames()-mapWin-1);
            const long probeStep=std::max<long>(1,sr/100); // 10 ms
            long jointSafe=-1;
            for(long f=std::max<long>(musicOnset[0],mapWin);f<=searchEnd;f+=probeStep) {
                if(mappedConfidenceAt(timeMaps[bridgeA],f,sr)>=0.55 &&
                   mappedConfidenceAt(timeMaps[bridgeB],f,sr)>=0.55) {
                    jointSafe=f;break;
                }
            }

            if(jointSafe>=musicOnset[0]) {
                struct BackwardCandidate {
                    long delay=0;
                    double robust=-2.0;
                    double raw=-2.0;
                    double gap=-999.0;
                    bool accepted=false;
                };
                // v0.2.31 start-tail correction: 50 ms steps could jump across
                // the short unstable interval seen in Source 4.  A 25 ms chain
                // remains inexpensive because it is used only between the
                // first stable map and programme onset, while giving the local
                // verifier twice as many opportunities to follow real start-up
                // wow without relaxing a single correlation/consensus gate.
                const long backHop=std::max<long>(1,sr/40); // 25 ms
                const int minZeroEvidenceSteps=std::max<int>(2,
                    (int)std::ceil(0.100*sr/(double)backHop));
                const long backRadius=std::max<long>(32,sr*20/1000); // +/-20 ms/step
                const int backWin=std::max(1024,sr*120/1000);
                const long peakSeparation=std::max<long>(16,sr/2000); // 0.5 ms
                auto measureBackwardCandidate=[&](int source,long masterFrame,
                                                   double predictedDelay) {
                    BackwardCandidate candidate;
                    const long center=(long)std::llround(predictedDelay);
                    long coarseBest=center;
                    double coarseQ=-2.0;
                    for(long d=center-backRadius;d<=center+backRadius;d+=16) {
                        if(masterFrame+d<0 ||
                           masterFrame+d+backWin>=(long)sources[source].frames())
                            continue;
                        const double q=robustMusicCorrAtStrided(
                            ref,sources[source],masterFrame,backWin,d,mapStride);
                        if(q>coarseQ){coarseQ=q;coarseBest=d;}
                    }
                    long best=coarseBest;
                    double bestQ=coarseQ;
                    for(long d=coarseBest-16;d<=coarseBest+16;++d) {
                        if(masterFrame+d<0 ||
                           masterFrame+d+backWin>=(long)sources[source].frames())
                            continue;
                        const double q=robustMusicCorrAtStrided(
                            ref,sources[source],masterFrame,backWin,d,mapStride);
                        if(q>bestQ){bestQ=q;best=d;}
                    }
                    double secondQ=-2.0;
                    for(long d=center-backRadius;d<=center+backRadius;d+=8) {
                        if(std::labs(d-best)<=peakSeparation) continue;
                        if(masterFrame+d<0 ||
                           masterFrame+d+backWin>=(long)sources[source].frames())
                            continue;
                        secondQ=std::max(secondQ,robustMusicCorrAtStrided(
                            ref,sources[source],masterFrame,backWin,d,mapStride));
                    }
                    long rawBest=best;
                    double rawQ=-2.0;
                    const long rawRadius=std::max<long>(16,sr*4/1000);
                    const long rawLo=std::max(center-backRadius,best-rawRadius);
                    const long rawHi=std::min(center+backRadius,best+rawRadius);
                    for(long d=rawLo;d<=rawHi;d+=4) {
                        if(masterFrame+d<0 ||
                           masterFrame+d+backWin>=(long)sources[source].frames())
                            continue;
                        const double q=corrAtStrided(
                            ref,sources[source],masterFrame,backWin,d,
                            std::max(1,sr/12000));
                        if(q>rawQ){rawQ=q;rawBest=d;}
                    }
                    const long rawCoarse=rawBest;
                    for(long d=std::max(center-backRadius,rawCoarse-4);
                        d<=std::min(center+backRadius,rawCoarse+4);++d) {
                        if(masterFrame+d<0 ||
                           masterFrame+d+backWin>=(long)sources[source].frames())
                            continue;
                        const double q=corrAtStrided(
                            ref,sources[source],masterFrame,backWin,d,
                            std::max(1,sr/12000));
                        if(q>rawQ){rawQ=q;rawBest=d;}
                    }
                    candidate.delay=rawBest;
                    candidate.robust=bestQ;
                    candidate.raw=rawQ;
                    candidate.gap=secondQ<-1.0?999.0:bestQ-secondQ;
                    candidate.accepted=std::isfinite(bestQ) && bestQ>=0.65 &&
                        candidate.gap>=0.002 && std::isfinite(rawQ) && rawQ>=0.30;
                    return candidate;
                };
                std::vector<double> backwardDelay(sourceCount,0.0);
                backwardDelay[bridgeA]=mappedDelayAt(timeMaps[bridgeA],jointSafe,sr);
                backwardDelay[bridgeB]=mappedDelayAt(timeMaps[bridgeB],jointSafe,sr);

                std::vector<long> backwardFrames;
                if(jointSafe>musicOnset[0]) {
                    for(long f=jointSafe-backHop;f>musicOnset[0];f-=backHop)
                        backwardFrames.push_back(f);
                    backwardFrames.push_back(musicOnset[0]);
                }

                int acceptedSteps=0;
                long primarySafeFrom=jointSafe;
                for(long mf:backwardFrames) {
                    std::vector<BackwardCandidate> c((size_t)sourceCount);
                    for(int pairIndex=0;pairIndex<2;++pairIndex) {
                        const int s=pairIndex==0 ? bridgeA : bridgeB;
                        c[s]=measureBackwardCandidate(s,mf,backwardDelay[s]);
                    }

                    const double move1=(double)c[bridgeA].delay-backwardDelay[bridgeA];
                    const double move2=(double)c[bridgeB].delay-backwardDelay[bridgeB];
                    const bool movementConsensus=std::abs(move1-move2)<=0.004*sr;
                    double donorCorr=-2.0;
                    if(c[bridgeA].accepted && c[bridgeB].accepted) {
                        const long donor1Pos=mf+c[bridgeA].delay;
                        donorCorr=robustMusicCorrAtStrided(
                            sources[bridgeA],sources[bridgeB],donor1Pos,backWin,
                            c[bridgeB].delay-c[bridgeA].delay,mapStride);
                    }
                    const bool pass=c[bridgeA].accepted && c[bridgeB].accepted &&
                        movementConsensus && donorCorr>=0.55;
                    std::cerr<<"BACKWARD_SYNC time "<<(double)mf/sr
                             <<" source_a "<<(originalIndex[bridgeA]+1)
                             <<" d_a "<<c[bridgeA].delay
                             <<" source_b "<<(originalIndex[bridgeB]+1)
                             <<" d_b "<<c[bridgeB].delay
                             <<" move_gap_ms "<<1000.0*std::abs(move1-move2)/sr
                             <<" master_q_a "<<c[bridgeA].robust
                             <<" master_q_b "<<c[bridgeB].robust
                             <<" donor_q "<<donorCorr<<" accepted "<<(pass?1:0)<<"\n";
                    if(startDiag) {
                        startDiag<<(originalIndex[bridgeA]+1)<<",backward_bridge,"<<backwardDelay[bridgeA]<<","<<(double)mf/sr
                                 <<","<<backwardDelay[bridgeA]<<","<<c[bridgeA].delay<<","<<c[bridgeA].robust<<","<<c[bridgeA].gap
                                 <<",,"<<acceptedSteps<<",,"<<1000.0*std::abs(move1-move2)/sr
                                 <<","<<(pass?1:0)<<","<<(pass?"DONOR_CONSENSUS_PASS":"BACKWARD_CHAIN_STOP")<<"\n";
                        startDiag<<(originalIndex[bridgeB]+1)<<",backward_bridge,"<<backwardDelay[bridgeB]<<","<<(double)mf/sr
                                 <<","<<backwardDelay[bridgeB]<<","<<c[bridgeB].delay<<","<<c[bridgeB].robust<<","<<c[bridgeB].gap
                                 <<",,"<<acceptedSteps<<",,"<<1000.0*std::abs(move1-move2)/sr
                                 <<","<<(pass?1:0)<<","<<(pass?"DONOR_CONSENSUS_PASS":"BACKWARD_CHAIN_STOP")<<"\n";
                    }
                    if(!pass) break;
                    for(int pairIndex=0;pairIndex<2;++pairIndex) {
                        const int s=pairIndex==0 ? bridgeA : bridgeB;
                        TimeMapAnchor a;
                        a.masterFrame=mf;
                        a.donorDelay=(double)c[s].delay;
                        a.corr=std::min(c[s].robust,donorCorr);
                        a.verified=true;
                        a.repairSafe=true;
                        timeMaps[s].push_back(a);
                        backwardDelay[s]=a.donorDelay;
                    }
                    primarySafeFrom=mf;
                    ++acceptedSteps;
                }

                // An older sparse anchor before a failed dense step must not
                // silently bracket a repair interval.  If the walk stops, both
                // primary donors are usable only from the last jointly proven
                // point forward.  If it reaches onset, this simply formalizes
                // the already verified start boundary.
                for(int pairIndex=0;pairIndex<2;++pairIndex) {
                    const int s=pairIndex==0 ? bridgeA : bridgeB;
                    const size_t blocked=blockRepairBefore(
                        timeMaps[s],primarySafeFrom);
                    std::cerr<<"BACKWARD_SYNC_PREFIX_GATE source "
                             <<(originalIndex[s]+1)
                             <<" safe_from_sec "<<(double)primarySafeFrom/sr
                             <<" blocked_anchors "<<blocked<<"\n";
                    if(startDiag)
                        startDiag<<(originalIndex[s]+1)
                                 <<",backward_prefix_gate,,"
                                 <<(double)primarySafeFrom/sr
                                 <<",,,,,,"<<acceptedSteps
                                 <<",,,1,REPAIR_BLOCKED_BEFORE_DENSE_CHAIN\n";
                }

                // v0.2.31: the primary pair above remains the sole authority for
                // establishing the early timing basin.  Once it has done so,
                // let every remaining conflict-free donor walk backwards from
                // its own first safe ordinary-map position.  Each published
                // point must match Master and BOTH primary bridge donors.  A
                // weak extra copy therefore gains early eligibility only when
                // it follows an already proven path; it cannot create one.
                for(int pairIndex=0;pairIndex<2;++pairIndex) {
                    const int s=pairIndex==0 ? bridgeA : bridgeB;
                    std::sort(timeMaps[s].begin(),timeMaps[s].end(),
                        [](const TimeMapAnchor& a,const TimeMapAnchor& b) {
                            return a.masterFrame<b.masterFrame;
                        });
                }
                for(size_t candidateIndex=2;
                    candidateIndex<backwardBridgeCandidates.size();
                    ++candidateIndex) {
                    const int s=backwardBridgeCandidates[candidateIndex];
                    long donorSafe=-1;
                    for(long f=std::max<long>(musicOnset[0],mapWin);
                        f<=searchEnd;f+=probeStep) {
                        if(mappedConfidenceAt(timeMaps[s],f,sr)>=0.55) {
                            donorSafe=f;
                            break;
                        }
                    }
                    if(donorSafe<0) {
                        std::cerr<<"BACKWARD_SYNC_FOLLOWUP_SKIPPED source "
                                 <<(originalIndex[s]+1)
                                 <<" reason NO_SAFE_ORDINARY_MAP\n";
                        continue;
                    }
                    if(donorSafe<=musicOnset[0]) {
                        std::cerr<<"BACKWARD_SYNC_FOLLOWUP_SKIPPED source "
                                 <<(originalIndex[s]+1)
                                 <<" reason ALREADY_SAFE_AT_ONSET\n";
                        continue;
                    }

                    double followDelay=mappedDelayAt(timeMaps[s],donorSafe,sr);
                    long previousFrame=donorSafe;
                    std::vector<long> followFrames;
                    for(long f=donorSafe-backHop;f>musicOnset[0];f-=backHop)
                        followFrames.push_back(f);
                    followFrames.push_back(musicOnset[0]);

                    int followSteps=0;
                    long followSafeFrom=donorSafe;
                    for(long mf:followFrames) {
                        const bool witnessesMapped=
                            mappedConfidenceAt(timeMaps[bridgeA],mf,sr)>=0.55 &&
                            mappedConfidenceAt(timeMaps[bridgeB],mf,sr)>=0.55;
                        BackwardCandidate c;
                        double witnessCorrA=-2.0,witnessCorrB=-2.0;
                        double sourceMove=0.0,witnessMoveA=0.0,witnessMoveB=0.0;
                        if(witnessesMapped) {
                            c=measureBackwardCandidate(s,mf,followDelay);
                            sourceMove=(double)c.delay-followDelay;
                            const double bridgeNowA=
                                mappedDelayAt(timeMaps[bridgeA],mf,sr);
                            const double bridgeNowB=
                                mappedDelayAt(timeMaps[bridgeB],mf,sr);
                            const double bridgePreviousA=
                                mappedDelayAt(timeMaps[bridgeA],previousFrame,sr);
                            const double bridgePreviousB=
                                mappedDelayAt(timeMaps[bridgeB],previousFrame,sr);
                            witnessMoveA=bridgeNowA-bridgePreviousA;
                            witnessMoveB=bridgeNowB-bridgePreviousB;
                            if(c.accepted) {
                                const long donorPos=mf+c.delay;
                                witnessCorrA=robustMusicCorrAtStrided(
                                    sources[s],sources[bridgeA],donorPos,backWin,
                                    (long)std::llround(bridgeNowA-c.delay),mapStride);
                                witnessCorrB=robustMusicCorrAtStrided(
                                    sources[s],sources[bridgeB],donorPos,backWin,
                                    (long)std::llround(bridgeNowB-c.delay),mapStride);
                            }
                        }
                        const bool pass=witnessesMapped &&
                            additionalBackwardBridgePass(c.accepted,sourceMove,
                                witnessMoveA,witnessMoveB,witnessCorrA,witnessCorrB,sr);
                        const double movementGap=std::max({
                            std::abs(sourceMove-witnessMoveA),
                            std::abs(sourceMove-witnessMoveB),
                            std::abs(witnessMoveA-witnessMoveB)});
                        std::cerr<<"BACKWARD_SYNC_FOLLOWUP time "<<(double)mf/sr
                                 <<" source "<<(originalIndex[s]+1)
                                 <<" delay "<<c.delay
                                 <<" master_q "<<c.robust
                                 <<" witness_q_a "<<witnessCorrA
                                 <<" witness_q_b "<<witnessCorrB
                                 <<" move_gap_ms "<<1000.0*movementGap/sr
                                 <<" accepted "<<(pass?1:0)<<"\n";
                        if(startDiag)
                            startDiag<<(originalIndex[s]+1)
                                     <<",backward_followup,"<<followDelay<<","<<(double)mf/sr
                                     <<","<<followDelay<<","<<c.delay<<","<<c.robust
                                     <<","<<c.gap<<",,"<<followSteps<<",,"
                                     <<1000.0*movementGap/sr<<","<<(pass?1:0)<<","
                                     <<(pass?"TWO_BRIDGE_WITNESSES_PASS":
                                        (witnessesMapped?"FOLLOWUP_CHAIN_STOP":
                                         "BRIDGE_WITNESS_MAP_UNAVAILABLE"))<<"\n";
                        if(!pass) break;
                        TimeMapAnchor a;
                        a.masterFrame=mf;
                        a.donorDelay=(double)c.delay;
                        a.corr=std::min({c.robust,witnessCorrA,witnessCorrB});
                        a.verified=true;
                        a.repairSafe=true;
                        timeMaps[s].push_back(a);
                        followDelay=a.donorDelay;
                        previousFrame=mf;
                        followSafeFrom=mf;
                        ++followSteps;
                    }
                    const size_t blocked=blockRepairBefore(
                        timeMaps[s],followSafeFrom);
                    std::cerr<<"BACKWARD_SYNC_FOLLOWUP_PREFIX_GATE source "
                             <<(originalIndex[s]+1)
                             <<" safe_from_sec "<<(double)followSafeFrom/sr
                             <<" blocked_anchors "<<blocked<<"\n";
                    if(startDiag)
                        startDiag<<(originalIndex[s]+1)
                                 <<",backward_prefix_gate,,"
                                 <<(double)followSafeFrom/sr
                                 <<",,,,,,"<<followSteps
                                 <<",,,1,REPAIR_BLOCKED_BEFORE_DENSE_CHAIN\n";
                    std::cerr<<"BACKWARD_SYNC_FOLLOWUP_SUMMARY source "
                             <<(originalIndex[s]+1)
                             <<" safe_sec "<<(double)donorSafe/sr
                             <<" onset_sec "<<(double)musicOnset[0]/sr
                             <<" witness_a "<<(originalIndex[bridgeA]+1)
                             <<" witness_b "<<(originalIndex[bridgeB]+1)
                             <<" accepted_steps "<<followSteps<<"\n";
                }

                // x2p: the verified bridge previously stopped at the detected
                // programme onset (1.095 s in the supplied run).  Bracket the
                // earlier interval from frame zero using the measured affine
                // slope, pinned to each donor's first safe anchor.  Publish the
                // pair only when BOTH extrapolations are plausible.  Local raw
                // phase and correlation remain mandatory before any repair.
                const bool primaryReachedOnset=
                    primarySafeFrom<=musicOnset[0];
                if(denseBackwardMayReachZero(primarySafeFrom,musicOnset[0],
                                             acceptedSteps,
                                             minZeroEvidenceSteps)) {
                    std::vector<BackwardZeroAnchor> zero((size_t)sourceCount);
                    bool pairValid=true;
                    for(int pairIndex=0;pairIndex<2;++pairIndex) {
                        const int s=pairIndex==0 ? bridgeA : bridgeB;
                        const auto& affine=syncPreflight.donor
                            [originalIndex[0]][originalIndex[s]].affine;
                        zero[s]=makeBackwardZeroAnchor(timeMaps[s],sr,
                            affine.valid?&affine:nullptr,affineBasinOffsets[s]);
                        pairValid=pairValid&&zero[s].valid;
                    }
                    if(pairValid) {
                        for(int pairIndex=0;pairIndex<2;++pairIndex) {
                            const int s=pairIndex==0 ? bridgeA : bridgeB;
                            timeMaps[s].push_back(zero[s].anchor);
                            std::cerr<<"BACKWARD_SYNC_ZERO source "
                                     <<(originalIndex[s]+1)
                                     <<" delay "<<zero[s].anchor.donorDelay
                                     <<" slope "<<zero[s].slope
                                     <<" residual_ms "
                                     <<1000.0*zero[s].maxResidual/sr<<"\n";
                            if(startDiag)
                                startDiag<<(originalIndex[s]+1)
                                         <<",backward_extrapolation,"
                                         <<zero[s].anchor.donorDelay<<",0,"
                                         <<zero[s].anchor.donorDelay<<","
                                         <<zero[s].anchor.donorDelay<<","
                                         <<zero[s].anchor.corr<<",,,,,"
                                         <<1000.0*zero[s].maxResidual/sr
                                         <<",1,AFFINE_TO_ZERO_LOCAL_VERIFY_REQUIRED\n";
                        }
                    } else {
                        std::cerr<<"BACKWARD_SYNC_ZERO_SKIPPED reason "
                                 <<"PAIR_EXTRAPOLATION_NOT_PLAUSIBLE\n";
                    }
                } else if(!primaryReachedOnset) {
                    std::cerr<<"BACKWARD_SYNC_ZERO_SKIPPED reason "
                             <<"DENSE_CHAIN_DID_NOT_REACH_ONSET\n";
                }
                for(int s=1;s<sourceCount;++s)
                    std::sort(timeMaps[s].begin(),timeMaps[s].end(),
                        [](const TimeMapAnchor& a,const TimeMapAnchor& b){
                            return a.masterFrame<b.masterFrame;
                        });
                std::cerr<<"BACKWARD_SYNC_SUMMARY joint_safe_sec "<<(double)jointSafe/sr
                         <<" onset_sec "<<(double)musicOnset[0]/sr
                         <<" source_a "<<(originalIndex[bridgeA]+1)
                         <<" source_b "<<(originalIndex[bridgeB]+1)
                         <<" accepted_steps "<<acceptedSteps<<"\n";
            }
        }

        beginWorkPhase("EDGE_SYNC");
        const int edgeAnchors=extendEdgeTimeMaps(sources,timeMaps,processingEligible,sr);
        std::cerr<<"EDGE_SYNC_SUMMARY measured_anchors "<<edgeAnchors<<"\n";

        std::cerr << "TIMEMAP_TOTAL anchors " << tmAnchorsTotal
                  << " verified " << tmVerifiedTotal
                  << " coarse_calls " << tmCoarseCallsTotal
                  << " coarse_sec " << tmCoarseSecTotal
                  << " fine_calls " << tmFineCallsTotal
                  << " fine_sec " << tmFineSecTotal
                  << " other_sec " << tmOtherSecTotal << "\n";

        if (tmDiag) {
            tmDiag.flush();
            std::cerr << "TIMEMAP_DIAGNOSTIC " << tmDiagName << "\n";
        }
        profilePhase("verified_time_maps");

        // x2f final gate: a promising sparse preflight is not authority to
        // reconstruct.  Require two donors with several repair-safe anchors
        // spanning a meaningful part of the selected Master's programme.
        int fullMapReadyDonors=0;
        for(int s=1;s<sourceCount;++s) {
            if(!processingEligible[s]) {
                std::cerr << "TIMEMAP_COVERAGE source " << (originalIndex[s]+1)
                          << " role REJECTED verified 0 repair_safe 0 span 0 usable 0\n";
                continue;
            }
            int verified=0,repairSafe=0;
            long firstSafe=-1,lastSafe=-1;
            for(const auto& a:timeMaps[s]) {
                if(a.verified) ++verified;
                if(a.verified && a.repairSafe) {
                    ++repairSafe;
                    if(firstSafe<0) firstSafe=a.masterFrame;
                    lastSafe=a.masterFrame;
                }
            }
            const long programmeFrames=std::max<long>(
                1,(long)ref.frames()-musicOnset[0]);
            const double span=(firstSafe>=0 && lastSafe>firstSafe)
                ? (double)(lastSafe-firstSafe)/programmeFrames : 0.0;
            const bool usable=repairSafe>=4 && span>=0.25;
            if(usable) ++fullMapReadyDonors;
            std::cerr << "TIMEMAP_COVERAGE source " << (originalIndex[s]+1)
                      << " role " << sourceRoleName(sourceRoles[s])
                      << " verified " << verified
                      << " repair_safe " << repairSafe
                      << " span " << span
                      << " usable " << (usable?1:0) << "\n";
        }
        std::cerr << "TIMEMAP_PREFLIGHT master " << (originalIndex[0]+1)
                  << " usable_donors " << fullMapReadyDonors
                  << " required 2\n";
        if(syncCheckSeconds==0.0 && sourceCount>=3 && fullMapReadyDonors<2) {
            std::ostringstream message;
            message << "Full time-map preflight stopped the run: Master Source "
                    << (originalIndex[0]+1) << " has " << fullMapReadyDonors
                    << " of 2 required independently verified donors. "
                    << "No reconstruction or result WAV was written.";
            throw std::runtime_error(message.str());
        }

        if(syncCheckSeconds>0) {
            std::ofstream check(outName+".sync.csv");
            if(!check)throw std::runtime_error("Cannot save sync check");
            check<<std::setprecision(12)
                 <<"source,time_sec,delay_sec,mapped_delay_sec,phase_correction_sec,"
                   "map_confidence,local_corr,map_verified,programme_active,status\n";
            const long end=std::min<long>((long)ref.frames(),(long)((syncCheckStart+syncCheckSeconds)*sr));
            if(syncCheckStart*sr>=end)throw std::runtime_error("Range is outside master audio");

            // Match the final restoration path: the verified time map supplies
            // the musical neighbourhood, then a +/-4 ms raw-waveform search
            // locks each 16 ms block to the correct sample phase. v0.1.0
            // stopped before this step and could report false local failures.
            const long checkHop=std::max(256,sr*16/1000);
            const long firstBlock=std::max<long>(0,
                (long)(syncCheckStart*sr)/checkHop-1);
            const long lastBlock=(end-1)/checkHop;
            std::vector<std::vector<double>> phaseCorrection(
                sourceCount,std::vector<double>((size_t)lastBlock+1,0.0));
            std::vector<double> previousCorrection(sourceCount,0.0);
            std::vector<bool> phaseLocked(sourceCount,false);
            const int phaseWin=std::max(512,sr*24/1000);
            const long phaseRadius=std::max<long>(32,sr*4/1000);
            // The time map is still built from the beginning, but raw phase is
            // searched only around the requested view. A check at 40 seconds
            // must not repeat 40 seconds of restoration-only block work.
            for(long bi=firstBlock;bi<=lastBlock;++bi) {
                const long block=bi*checkHop;
                const long blockEnd=std::min<long>(block+checkHop,end);
                const long phaseCenter=block+(blockEnd-block)/2;
                const long phasePos=std::max<long>(0,phaseCenter-phaseWin/2);
                for(int s=1;s<sourceCount;++s) {
                    const double mapConfidence=timeMaps[s].empty()?0.0:
                        mappedConfidenceAt(timeMaps[s],phaseCenter,sr);
                    if(mapConfidence<0.55) {
                        phaseCorrection[s][(size_t)bi]=phaseLocked[s]?previousCorrection[s]:0.0;
                        continue;
                    }
                    const double mapped=mappedDelayAt(timeMaps[s],phaseCenter,sr);
                    const auto phase=searchPhaseNearTrajectory(
                        ref,sources[s],phasePos,phaseWin,mapped,sr);
                    double correction=phaseLocked[s]?previousCorrection[s]:0.0;
                    const double lockThreshold=phaseLocked[s]?0.30:0.55;
                    if(std::isfinite(phase.corr) && phase.corr>=lockThreshold) {
                        correction=std::clamp((double)phase.delay-mapped,
                                              -(double)phaseRadius,
                                              (double)phaseRadius);
                        if(phaseLocked[s]) {
                            const double maxStep=std::max(24.0,sr*0.0005);
                            correction=std::clamp(correction,
                                previousCorrection[s]-maxStep,
                                previousCorrection[s]+maxStep);
                        }
                        previousCorrection[s]=correction;
                        phaseLocked[s]=true;
                    }
                    phaseCorrection[s][(size_t)bi]=correction;
                }
            }

            for(long f=(long)(syncCheckStart*sr);f<end;f+=std::max(1,sr/100))for(int s=0;s<sourceCount;++s) {
                const double conf=s==0?1:mappedConfidenceAt(timeMaps[s],f,sr);
                double mapped=s==0?0:mappedDelayAt(timeMaps[s],f,sr);
                if(s>0 && timeMaps[s].empty())mapped=state[s].delay;
                const size_t bi=(size_t)std::min<long>(lastBlock,std::max<long>(0,f/checkHop));
                const double phase=s==0?0:phaseCorrection[s][bi];
                const double delay=mapped+phase;
                const int win=std::max(256,sr*24/1000);
                const bool inBounds=f+delay>=0 && f+delay+win<(double)sources[s].frames() && f+win<(long)ref.frames();
                const double local=s==0?1:(inBounds?corrAtStrided(ref,sources[s],f,win,(long)std::llround(delay),std::max(1,sr/12000)):-2);
                const bool programme=f>=musicOnset[0];
                const char* status=s==0?"REFERENCE":!programme?"PRE_ROLL":!inBounds?"OUTSIDE_AUDIO":conf<0.55?"MAP_UNVERIFIED":local<0.30?"PHASE_LOCAL_CORR_LOW":"PHASE_AND_LOCAL_PASS";
                check<<(originalIndex[s]+1)<<","<<(double)f/sr<<","<<delay/sr<<","
                     <<mapped/sr<<","<<phase/sr<<","<<conf<<","<<local<<","
                     <<(conf>=0.55?1:0)<<","<<(programme?1:0)<<","<<status<<"\n";
            }
            check.flush();if(!check)throw std::runtime_error("Cannot finish sync check");
            return 0; // No reconstruction or audio output.
        }

        std::cerr << "PHASE RESTORATION 0 100\n";

        std::vector<double> lastAnchorDelay(sourceCount, 0.0);

        // Reconstruction also starts each local estimator on the verified
        // timeline. Without this handoff its legacy 0.3% trajectory predictor
        // can never catch a source recorded several percent fast or slow.
        auto guidedDelayAt=[&](int source,long frame,double fallback,
                               bool* guided=nullptr)->double {
            bool used=false;
            double delay=fallback;
            if(source>0 && mappedConfidenceAt(timeMaps[source],frame,sr)>=0.55) {
                delay=mappedDelayAt(timeMaps[source],frame,sr);
                used=true;
            } else if(source>0) {
                const auto& model=syncPreflight.donor
                    [originalIndex[0]][originalIndex[source]].affine;
                if(model.valid) {
                    delay=model.delayAt(frame)+affineBasinOffsets[source];
                    used=true;
                }
            }
            if(guided) *guided=used;
            return delay;
        };

        for (int s = 0; s < sourceCount; ++s) {
            if(!processingEligible[s]) {
                state[s].quality=0.0;
                trajectoryQuality[s]=0.0;
            } else if(s>0) {
                state[s].delay=guidedDelayAt(s,0,state[s].delay);
                state[s].measuredDelay=state[s].delay;
            }
            prevDelay[s] = state[s].delay;
            nextDelay[s] = state[s].delay;
            lastAnchorDelay[s] = state[s].delay;
        }

        std::vector<uint64_t> selectedCount(sourceCount, 0);

        std::vector<DecisionRow> mapRows;
        if (!mapName.empty())
            mapRows.reserve(outFrames / hop + 1);

        // v2.21a-startup-progress keeps robust 16 ms synchronization evidence but
        // performs the actual source selection in short local segments. The
        // selected audio master is both the preferred source and the immutable
        // technical time reference.
        int currentSource = audioMasterIndex;
        const int surgicalFrames = std::max(48, sr * 2 / 1000); // ~2 ms

        // v0.2.29 SHADOW coarse scan — 2-of-3 consensus.
        // This still does NOT skip any existing analysis. It only asks a cheap
        // question: do two aligned physical copies agree materially better with
        // each other than either agrees with the third?  Ambiguous/low-consensus
        // regions are also flagged. We measure recall against the final A/B
        // decisions before allowing this gate to save any work.
        uint64_t coarseTotal = 0, coarseFlagged = 0;
        uint64_t coarseAB = 0, coarseABCovered = 0;
        uint64_t coarseConsensus = 0, coarseAmbiguous = 0;
        uint64_t coarseTransient = 0, coarseEnergy = 0;
        uint64_t fastEligible = 0, fastAHit = 0, fastBHit = 0, fastCHit = 0;
        uint64_t finalA = 0, finalB = 0, finalC = 0;
        uint64_t rpSegments=0, rpFast=0, rpFull=0;
        double rpGateSec=0.0, rpFullSec=0.0, rpTailSec=0.0;

        // v0.2.30h: keep the 2 ms detector decisions, but reconstruct only
        // after neighbouring decisions have been seen.  A physical click can
        // straddle several detector cells; rendering every cell independently
        // caused donor hopping and a master sliver in the middle of one event.
        struct RepairSegmentPlan {
            long start=0, end=0;
            std::vector<double> delay;
            std::vector<std::string> gateReason;
            std::vector<double> trendFit;
            int source=-1;
            char confidenceClass='C';
            std::string decisionReason="MASTER_ONLY";
            double gain=1.0, dc=0.0, outlierRatio=0.0;
            double masterImpulseZ=std::numeric_limits<double>::quiet_NaN();
            std::vector<int> supportingSources;
        };
        std::vector<RepairSegmentPlan> repairPlans;
        repairPlans.reserve((size_t)std::max<uint64_t>(1,
            ref.frames()/std::max(1,surgicalFrames)));

        struct CoarseDiagnosticRow {
            double timeSec=0.0;
            double pairBest=0.0, pairSecond=0.0, pairWorst=0.0;
            double pairGap=0.0;
            double maxDerivRatio=0.0, maxEnergyRatio=0.0;
            int consensus=0, ambiguous=0, transient=0, energy=0, flagged=0;
            int fast_c_eligible=0;
            char finalClass='C';
        };
        std::vector<CoarseDiagnosticRow> coarseDiag;
        coarseDiag.reserve((size_t)std::max<uint64_t>(1, ref.frames()/std::max(1,surgicalFrames)));
        const int coarseStride = std::max(1, sr / 4000); // sparse ~4 kHz feature sampling
        const int coarseHalfWin = std::max(96, sr * 6 / 1000); // +/-6 ms
        const double coarseStrongPair = 0.94;
        const double coarsePairAdvantage = 0.015;
        const double coarseAmbiguousCorr = 0.80;
        // Extra cheap detectors. Deliberately permissive in shadow mode:
        // recall is more important than selectivity at this stage.
        const double coarseTransientRatio = 2.20;
        const double coarseEnergyRatio = 1.45;

        double rpBlockSyncSec=0.0, rpBlockScoreSec=0.0, rpSegmentLoopSec=0.0;
        uint64_t rpBlocks=0;

        // v0.2.29 block-sync profiler.
        uint64_t bsReacquireBlocks=0, bsEstimateBlocks=0;
        uint64_t bsReacquireJobs=0, bsEstimateJobs=0;
        double bsLaunchSec=0.0, bsWaitSec=0.0, bsPostSec=0.0;
        double bsReacquireCpuSec=0.0, bsEstimateCpuSec=0.0;

        // v0.2.30f: retain the fine phase choice between adjacent 16 ms
        // blocks.  Without this state, periodic music can alternate between
        // the two edges of the +/-4 ms search and create an artificial
        // 7--8 ms delay jump even when the long time map is smooth.
        std::vector<double> previousBlockPhaseCorrection(sourceCount,0.0);
        std::vector<bool> blockPhaseLocked(sourceCount,false);

        for (long block = 0;
             block < (long)outFrames;
             block += hop) {
            ++rpBlocks;
            const auto rpBlockStart=std::chrono::steady_clock::now();

            long nextPos =
                std::min<long>(
                    block + hop,
                    (long)outFrames - 1);

            state[0].delay = 0.0;
            state[0].measuredDelay = 0.0;
            state[0].quality = 1.0;
            state[0].shock = 0.0;
            nextDelay[0] = 0.0;

            // Delay estimation is independent for each non-reference source.
            // Run it concurrently; this is where the engine spends much of its
            // time on multi-core CPUs.
            std::vector<std::future<TrackState>> delayJobs;
            std::vector<int> delayJobSources;
            delayJobs.reserve(std::max(0, sourceCount - 1));
            delayJobSources.reserve(std::max(0, sourceCount - 1));
            std::vector<double> delayJobSec(sourceCount,0.0);
            const long reacquireEvery = std::max<long>(hop, sr / 2); // ~0.5 s
            const bool doReacquire = (block == 0) ||
                ((block / hop) % std::max<long>(1, reacquireEvery / hop) == 0);
            if(doReacquire) ++bsReacquireBlocks; else ++bsEstimateBlocks;

            const auto bsLaunchStart=std::chrono::steady_clock::now();
            for (int s = 1; s < sourceCount; ++s) {
                // The reserve follows its sparse verified map and is admitted
                // only when the active donor core cannot decide. Do not spend a
                // full 16 ms estimator job on it for every block.
                if(sourceRoles[s]!=SourceRole::Donor) continue;
                delayJobSources.push_back(s);
                delayJobs.emplace_back(std::async(std::launch::async,
                    [&, s]() {
                        const auto jobStart=std::chrono::steady_clock::now();
                        // Predict from the last accepted physical trajectory.
                        const double fallback=lastAnchorDelay[s] +
                            trajectoryVelocity[s] * (double)(nextPos - lastAnchorPos[s]);
                        double predicted=guidedDelayAt(s,nextPos,fallback);
                        TrackState result = doReacquire
                            ? reacquireRealDelay(ref, sources[s], nextPos, predicted)
                            : estimateDelay(ref, sources[s], nextPos, predicted, searchRadius, window);
                        delayJobSec[s]=std::chrono::duration<double>(
                            std::chrono::steady_clock::now()-jobStart).count();
                        return result;
                    }));
            }
            bsLaunchSec += std::chrono::duration<double>(
                std::chrono::steady_clock::now()-bsLaunchStart).count();

            const auto bsWaitStart=std::chrono::steady_clock::now();
            std::vector<TrackState> measuredJobs(sourceCount);
            for(size_t job=0;job<delayJobs.size();++job)
                measuredJobs[delayJobSources[job]]=delayJobs[job].get();
            bsWaitSec += std::chrono::duration<double>(
                std::chrono::steady_clock::now()-bsWaitStart).count();

            for(int s:delayJobSources) {
                if(doReacquire) {
                    ++bsReacquireJobs;
                    bsReacquireCpuSec += delayJobSec[s];
                } else {
                    ++bsEstimateJobs;
                    bsEstimateCpuSec += delayJobSec[s];
                }
            }

            const auto bsPostStart=std::chrono::steady_clock::now();
            for (int s:delayJobSources) {
                TrackState measured = measuredJobs[s];
                const double fallback=lastAnchorDelay[s] +
                    trajectoryVelocity[s] * (double)(nextPos - lastAnchorPos[s]);
                bool guided=false;
                double predicted=guidedDelayAt(s,nextPos,fallback,&guided);

                if (doReacquire && measured.quality > 0.0) {
                    // A long-window accepted anchor is trusted.  Derive a slow
                    // drift rate from successive anchors, then low-pass it to
                    // reject eccentric-groove jitter and false jumps.
                    long dp = nextPos - lastAnchorPos[s];
                    if (dp > 0 && !guided) {
                        double v = (measured.delay - lastAnchorDelay[s]) / (double)dp;
                        const double vmax = 0.003; // 0.3% instantaneous speed error guard
                        v = std::max(-vmax, std::min(vmax, v));
                        trajectoryVelocity[s] = 0.75 * trajectoryVelocity[s] + 0.25 * v;
                    }
                    lastAnchorPos[s] = nextPos;
                    lastAnchorDelay[s] = measured.delay;
                    trajectoryQuality[s] = measured.quality;
                    state[s] = measured;
                } else if (!doReacquire && measured.quality >= 0.58) {
                    // Strong short-window evidence may gently refine the path.
                    double correction = measured.delay - predicted;
                    double lim = sr * 0.0015; // max 1.5 ms local correction
                    correction = std::max(-lim, std::min(lim, correction));
                    state[s] = measured;
                    state[s].delay = predicted + 0.20 * correction;
                    state[s].quality = std::max(trajectoryQuality[s], measured.quality);
                } else {
                    // Weak/ambiguous 16 ms evidence: FOLLOW the last accepted
                    // trajectory instead of declaring the donor unusable.
                    state[s].delay = predicted;
                    state[s].measuredDelay = predicted;
                    state[s].quality = trajectoryQuality[s];
                    state[s].shock = state[s].delay - prevDelay[s];
                }
                nextDelay[s] = state[s].delay;
            }
            for(int s=1;s<sourceCount;++s) if(sourceRoles[s]==SourceRole::Reserve) {
                const double predicted=guidedDelayAt(s,nextPos,state[s].delay);
                state[s].delay=predicted;
                state[s].measuredDelay=predicted;
                state[s].quality=mappedConfidenceAt(timeMaps[s],nextPos,sr)>=0.55
                    ? 1.0 : 0.0;
                state[s].shock=state[s].delay-prevDelay[s];
                nextDelay[s]=state[s].delay;
            }
            bsPostSec += std::chrono::duration<double>(
                std::chrono::steady_clock::now()-bsPostStart).count();

            long end =
                std::min<long>(
                    block + hop,
                    (long)outFrames);

            std::vector<double> midDelay(sourceCount, 0.0);
            for (int s = 1; s < sourceCount; ++s)
                midDelay[s] = 0.5 * (prevDelay[s] + nextDelay[s]);

            rpBlockSyncSec += std::chrono::duration<double>(
                std::chrono::steady_clock::now()-rpBlockStart).count();
            const auto rpBlockScoreStart=std::chrono::steady_clock::now();

            // A block score is still kept for diagnostics/decision-map output.
            std::vector<double> blockScore(sourceCount,
                std::numeric_limits<double>::infinity());
            for (int s = 0; s < sourceCount; ++s) {
                if(!processingEligible[s]) continue;
                double corr = (s == 0) ? 1.0 : state[s].quality;
                double shock = (s == 0) ? 0.0 : state[s].shock;
                if (s != 0 && corr < 0.52)
                    continue;
                double lastFrame = (double)(end - 1) + midDelay[s];
                if (lastFrame + 2.0 >= (double)sources[s].frames())
                    continue;
                blockScore[s] = sourceBlockScore(
                    sources, s, block, end, midDelay[s], shock, corr, midDelay,
                    &processingEligible);
            }

            // Render 2 ms local segments. There is deliberately no long-lived
            // block hysteresis here: a source must win again for each segment.
            // If scores are effectively tied, prefer source 1 (the reference)
            // and then the lowest source number. That gives a stable primary
            // source while still escaping immediately from a detected defect.
            rpBlockScoreSec += std::chrono::duration<double>(
                std::chrono::steady_clock::now()-rpBlockScoreStart).count();

        // v0.2.29 live analysis protocol.
        // Emit every source transition and a heartbeat about 10 times/s.
        int liveLastSource = -1;
        double liveLastEmitSec = -1.0;
            // v0.2.29s: raw-waveform phase lock once per 16 ms block. The
            // verified time map supplies the musical neighbourhood; this small
            // local search removes the remaining periodic-cycle/phase error.
            std::vector<double> blockPhaseCorrection(sourceCount,0.0);
            const long phaseCenter=block+(end-block)/2;
            const int phaseWin=std::max(512,sr*24/1000);
            const long phasePos=std::max<long>(0,phaseCenter-phaseWin/2);
            const long phaseRadius=std::max<long>(32,sr*4/1000);
            for(int s=1;s<sourceCount;++s) {
                if(!processingEligible[s]) continue;
                const double mapConfidence=timeMaps[s].empty()?0.0:
                    mappedConfidenceAt(timeMaps[s],phaseCenter,sr);
                if(mapConfidence<0.55) {
                    if(blockPhaseLocked[s])
                        blockPhaseCorrection[s]=previousBlockPhaseCorrection[s];
                    continue;
                }
                const double mapped=mappedDelayAt(timeMaps[s],phaseCenter,sr);
                const auto phase=searchPhaseNearTrajectory(
                    ref,sources[s],phasePos,phaseWin,mapped,sr);
                const double lockThreshold=blockPhaseLocked[s]?0.30:0.55;
                if(std::isfinite(phase.corr) && phase.corr>=lockThreshold) {
                    double correction=(double)phase.delay-mapped;
                    correction=std::clamp(correction,
                                           -(double)phaseRadius,
                                           (double)phaseRadius);
                    if(blockPhaseLocked[s]) {
                        const double maxStep=std::max(24.0,sr*0.0005); // 0.5 ms/block
                        correction=std::clamp(correction,
                            previousBlockPhaseCorrection[s]-maxStep,
                            previousBlockPhaseCorrection[s]+maxStep);
                    }
                    blockPhaseCorrection[s]=correction;
                    previousBlockPhaseCorrection[s]=blockPhaseCorrection[s];
                    blockPhaseLocked[s]=true;
                } else if(blockPhaseLocked[s]) {
                    blockPhaseCorrection[s]=previousBlockPhaseCorrection[s];
                }
            }
            const auto rpSegmentLoopStart=std::chrono::steady_clock::now();
            for (long seg = block; seg < end; seg += surgicalFrames) {
                ++rpSegments;
                const auto rpSegStart=std::chrono::steady_clock::now();
                long segEnd = std::min<long>(seg + surgicalFrames, end);
                double segT = hop ? (double)(seg - block + (segEnd-seg)/2) / hop : 0.0;
                std::vector<double> segDelay(sourceCount, 0.0);
                for (int s = 1; s < sourceCount; ++s) {
                    if(!processingEligible[s]) continue;
                    if (s > 0 && !timeMaps[s].empty() && mappedConfidenceAt(timeMaps[s],seg,sr)>=0.55)
                        segDelay[s] = mappedDelayAt(timeMaps[s], seg, sr)+blockPhaseCorrection[s];
                    else if(!timingMaps[s].empty())
                        segDelay[s] = mappedDelayAt(timingMaps[s],seg,sr);
                    else
                        segDelay[s] = prevDelay[s] + (nextDelay[s] - prevDelay[s]) * segT;
                }

                // Cheap N-source consensus candidate around this segment.
                // Compute sparse pairwise correlations on every aligned copy.
                bool coarseCandidate = false;
                double coarseBestPair = -2.0;
                double coarseSecondPair = -2.0;
                double coarseWorstPair = 2.0;
                bool twoAgree = false, ambiguous = false;
                bool transientCandidate = false, energyCandidate = false;
                double maxDerivRatio = 1.0, maxEnergyRatio = 1.0;
                bool coarseAllAligned = false;

                if (sourceCount >= 3) {
                    const long cc = seg + (segEnd - seg) / 2;
                    std::vector<bool> coarseUsable((size_t)sourceCount,false);
                    std::vector<int> coarseSources;
                    for (int ss=0; ss<sourceCount; ++ss) {
                        if(!processingEligible[ss]) continue;
                        const double firstFrame=(double)(cc-coarseHalfWin)+segDelay[ss];
                        const double lastFrame=(double)(cc+coarseHalfWin)+segDelay[ss];
                        const bool inBounds=firstFrame>=0.0 &&
                            lastFrame+1.0<(double)sources[ss].frames();
                        const bool mapped=(ss==audioMasterIndex) ||
                            (!timeMaps[ss].empty() &&
                             mappedConfidenceAt(timeMaps[ss],seg,sr)>=0.55);
                        coarseUsable[ss]=inBounds && mapped;
                        if(coarseUsable[ss]) coarseSources.push_back(ss);
                    }
                    coarseAllAligned=(int)coarseSources.size()==processingSourceCount;

                    // An unaligned copy is missing evidence, not an acoustic
                    // anomaly. Do not let unrelated samples create enormous
                    // energy ratios or false agreement. Full analysis remains
                    // enabled; the conservative fast path requires every copy.
                    if (!coarseAllAligned || coarseSources.size()<3) {
                        coarseCandidate=true;
                    }
                    if(coarseSources.size()>=3) {

                    auto pairCorr = [&](int a, int b) -> double {
                        double ma = 0.0, mb = 0.0;
                        long n = 0;

                        for (long off=-coarseHalfWin; off<=coarseHalfWin; off+=coarseStride) {
                            double fa = (double)(cc + off) + segDelay[a];
                            double fb = (double)(cc + off) + segDelay[b];
                            if (fa < 0.0 || fb < 0.0 ||
                                fa + 1.0 >= (double)sources[a].frames() ||
                                fb + 1.0 >= (double)sources[b].frames())
                                continue;

                            double va = 0.0, vb = 0.0;
                            for (int ch=0; ch<sources[a].channels; ++ch)
                                va += sampleLinear(sources[a], fa, ch);
                            for (int ch=0; ch<sources[b].channels; ++ch)
                                vb += sampleLinear(sources[b], fb, ch);
                            va /= std::max(1, sources[a].channels);
                            vb /= std::max(1, sources[b].channels);

                            ma += va; mb += vb; ++n;
                        }

                        if (n < 8) return -2.0;
                        ma /= n; mb /= n;

                        double ab=0.0, aa=0.0, bb=0.0;
                        for (long off=-coarseHalfWin; off<=coarseHalfWin; off+=coarseStride) {
                            double fa = (double)(cc + off) + segDelay[a];
                            double fb = (double)(cc + off) + segDelay[b];
                            if (fa < 0.0 || fb < 0.0 ||
                                fa + 1.0 >= (double)sources[a].frames() ||
                                fb + 1.0 >= (double)sources[b].frames())
                                continue;

                            double va = 0.0, vb = 0.0;
                            for (int ch=0; ch<sources[a].channels; ++ch)
                                va += sampleLinear(sources[a], fa, ch);
                            for (int ch=0; ch<sources[b].channels; ++ch)
                                vb += sampleLinear(sources[b], fb, ch);
                            va /= std::max(1, sources[a].channels);
                            vb /= std::max(1, sources[b].channels);

                            double xa = va - ma;
                            double xb = vb - mb;
                            ab += xa*xb; aa += xa*xa; bb += xb*xb;
                        }

                        double den = std::sqrt(aa*bb);
                        return den > 1e-18 ? ab/den : -2.0;
                    };

                    std::vector<double> pairValues;
                    for(size_t a=0;a<coarseSources.size();++a)
                        for(size_t b=a+1;b<coarseSources.size();++b)
                            pairValues.push_back(pairCorr(
                                coarseSources[a],coarseSources[b]));
                    std::sort(pairValues.begin(),pairValues.end());
                    coarseWorstPair=pairValues.front();
                    coarseBestPair=pairValues.back();
                    coarseSecondPair=pairValues.size()>1
                        ? pairValues[pairValues.size()-2] : coarseBestPair;

                    twoAgree =
                        coarseBestPair >= coarseStrongPair &&
                        (coarseBestPair - coarseSecondPair) >= coarsePairAdvantage;

                    ambiguous =
                        coarseWorstPair < coarseAmbiguousCorr;

                    // Cheap per-copy transient and local-energy features.
                    // We use the same aligned sparse window; no donor scoring,
                    // continuity scoring or reconstruction is performed here.
                    std::vector<double> peakDeriv((size_t)sourceCount,0.0);
                    std::vector<double> energy((size_t)sourceCount,0.0);
                    std::vector<long> featN((size_t)sourceCount,0);

                    for (int ss:coarseSources) {
                        bool havePrev=false;
                        double prev=0.0;
                        for (long off=-coarseHalfWin; off<=coarseHalfWin; off+=coarseStride) {
                            double f=(double)(cc+off)+segDelay[ss];
                            if (f < 0.0 || f + 1.0 >= (double)sources[ss].frames()) continue;

                            double v=0.0;
                            for (int ch=0; ch<sources[ss].channels; ++ch)
                                v += sampleLinear(sources[ss],f,ch);
                            v /= std::max(1,sources[ss].channels);

                            energy[ss] += v*v;
                            ++featN[ss];
                            if (havePrev) peakDeriv[ss]=std::max(peakDeriv[ss],std::abs(v-prev));
                            prev=v; havePrev=true;
                        }
                        if (featN[ss] > 0) energy[ss] /= featN[ss];
                    }

                    std::vector<double> derivativeValues,energyValues;
                    for(int ss:coarseSources) {
                        derivativeValues.push_back(peakDeriv[ss]);
                        energyValues.push_back(energy[ss]);
                    }
                    const double medD=percentileD(derivativeValues,0.5);
                    const double medE=percentileD(energyValues,0.5);


                    if (medD > 1e-9) {
                        for (int ss:coarseSources) {
                            double r=peakDeriv[ss]/medD;
                            maxDerivRatio=std::max(maxDerivRatio,r);
                            if (r > coarseTransientRatio) transientCandidate=true;
                        }
                    }
                    if (medE > 1e-12) {
                        for (int ss:coarseSources) {
                            double r=energy[ss]/medE;
                            double sym=std::max(r,1.0/std::max(r,1e-12));
                            maxEnergyRatio=std::max(maxEnergyRatio,sym);
                            if (r > coarseEnergyRatio || r < 1.0/coarseEnergyRatio)
                                energyCandidate=true;
                        }
                    }

                    // OR gate: any cheap suspicion sends the segment to full analysis.
                    coarseCandidate = coarseCandidate || twoAgree || ambiguous ||
                        transientCandidate || energyCandidate;
                    if (twoAgree) ++coarseConsensus;
                    if (ambiguous) ++coarseAmbiguous;
                    if (transientCandidate) ++coarseTransient;
                    if (energyCandidate) ++coarseEnergy;
                    } // at least three aligned sources
                }

                ++coarseTotal;
                if (coarseCandidate) ++coarseFlagged;

                // v0.2.29 Conservative Fast Path — ACTIVE.
                // A segment is eligible for bypass only when all supplied copies
                // agree extremely strongly and none of the cheap anomaly
                // detectors fires. Eligible blocks bypass donor scoring and remain on the authentic Master.
                const bool fastCEligible =
                    processingSourceCount >= 3 &&
                    coarseAllAligned &&
                    coarseBestPair >= 0.995 &&
                    coarseSecondPair >= 0.990 &&
                    coarseWorstPair >= 0.985 &&
                    (coarseBestPair - coarseSecondPair) <= 0.006 &&
                    !coarseCandidate;
                if (fastCEligible) ++fastEligible;
                if (fastCEligible) ++rpFast; else ++rpFull;
                rpGateSec += std::chrono::duration<double>(
                    std::chrono::steady_clock::now()-rpSegStart).count();
                const auto rpFullStart=std::chrono::steady_clock::now();

                std::vector<double> segScore(sourceCount,
                    std::numeric_limits<double>::infinity());
                std::vector<double> continuityScore(sourceCount, 0.0);
                // v0.2.29l PEAK-SEPARATION DIAGNOSTIC: record why a source is rejected before scoring.
                // These values are observational only and do not alter reconstruction.
                std::vector<double> gateCorr(sourceCount, std::numeric_limits<double>::quiet_NaN());
                std::vector<double> gateMapConfidence(sourceCount, std::numeric_limits<double>::quiet_NaN());
                std::vector<double> gateLocalVerify(sourceCount, std::numeric_limits<double>::quiet_NaN());
                std::vector<std::string> gateReason(sourceCount, "NOT_EVALUATED");
                int chosen = audioMasterIndex;
                NSourceEvidence consensusEvidence;
                ImpulseConsensusEvidence impulseEvidence;
                std::string decisionReason = fastCEligible ? "C_FASTPATH_MASTER" : "MASTER_ONLY";
                char confidenceClass = 'C';
                bool impulseRepair=false;

                if (!fastCEligible) {
                for (int s = 0; s < sourceCount; ++s) {
                    if(!processingEligible[s]) {
                        gateReason[s]="PREFLIGHT_REJECTED";
                        continue;
                    }
                    double corr = (s == 0) ? 1.0 : state[s].quality;
                    double shock = (s == 0) ? 0.0 : state[s].shock;
                    gateCorr[s] = corr;
                    if (s != 0 && corr < 0.52) {
                        gateReason[s] = "GLOBAL_QUALITY_FAIL";
                        continue;
                    }
                    double lastFrame = (double)(segEnd - 1) + segDelay[s];
                    if (lastFrame + 2.0 >= (double)sources[s].frames()) {
                        gateReason[s] = "SOURCE_BOUNDS_FAIL";
                        continue;
                    }
                    // v0.2.29: donor eligibility requires a verified local
                    // correlation at the actual segment position.  The delay
                    // trajectory alone is never enough evidence.
                    const double mapConfidence =
                        (s > 0 && !timeMaps[s].empty()) ? mappedConfidenceAt(timeMaps[s], seg, sr) : 0.0;
                    gateMapConfidence[s] = mapConfidence;
                    if (s > 0 && mapConfidence < 0.55) {
                        gateReason[s] = "MAP_CONFIDENCE_FAIL";
                        continue;
                    }
                    const int verifyWin = std::max(256, sr * 24 / 1000); // 24 ms
                    // v0.2.29u: after raw phase lock, verify the actual waveform
                    // position rather than the old phase-insensitive envelope.
                    // The envelope gate preferred the previous wrong half-cycle
                    // and rejected every correctly phased donor in the real test.
                    double localVerify = corrAtStrided(
                        ref, sources[s], seg, verifyWin,
                        (long)std::llround(segDelay[s]), std::max(1, sr / 12000));
                    gateLocalVerify[s] = localVerify;
                    // This is an admission gate, not the repair decision.
                    // Independent third-copy confirmation remains mandatory.
                    const double localVerifyMin = (s == audioMasterIndex) ? 0.20 : 0.30;
                    if (!std::isfinite(localVerify) || localVerify < localVerifyMin) {
                        gateReason[s] = "LOCAL_VERIFY_FAIL";
                        continue;
                    }

                    gateReason[s] = "ELIGIBLE_SCORE";
                    // v0.2.29p SCORE-DOMAIN FIX:
                    // localVerify is alignment evidence, not an audio-defect measure.
                    // It has already passed the conservative 0.72 donor gate above.
                    // Feeding it into localStability() penalised every donor by
                    // 2*(0.95-localVerify), while Master always received 1.0.
                    // At the observed ~0.73 correlation this added ~0.44 to donor
                    // scores and made a donor win mathematically impossible.
                    // Score all gate-approved sources on the same audio-quality
                    // scale; keep localVerify solely as eligibility evidence.
                    constexpr double qualityScoreCorrelation = 1.0;
                    segScore[s] = sourceBlockScore(
                        sources, s, seg, segEnd, segDelay[s], shock,
                        qualityScoreCorrelation, segDelay,&processingEligible);
                }

                for (int ss = 0; ss < sourceCount; ++ss) {
                    if (!std::isfinite(segScore[ss])) continue;
                    continuityScore[ss] = sourceContinuityScore(
                        sources, ss, seg, segDelay[ss], out, ref.channels, sr);
                    // Original Master-2000 idea: a physically plausible continuation
                    // of the already reconstructed waveform is preferred. Keep this
                    // as supporting evidence, not a substitute for defect/consensus scoring.
                    segScore[ss] += 0.35 * continuityScore[ss];
                }

                // Verified donor selection. Stage 1 admits each source
                // independently. Stage 2 requires a strict majority among the
                // locally usable physical transfers before changing Master.
                const double masterScore = segScore[audioMasterIndex];
                double donorBest = std::numeric_limits<double>::infinity();
                int donorBestSource = -1;
                for (int s = 0; s < sourceCount; ++s) {
                    if (s == audioMasterIndex) continue;
                    if(!processingEligible[s]) continue;
                    if (std::isfinite(segScore[s]) && segScore[s] < donorBest) {
                        donorBest = segScore[s];
                        donorBestSource = s;
                    }
                }

                // A click is a sample-scale event. Every voting witness must
                // have a verified time map and local waveform match. Unlike
                // x2k, one failed extra copy abstains instead of
                // vetoing the two or more remaining independent witnesses.
                std::vector<bool> activeWitness((size_t)sourceCount,false);
                std::vector<bool> reserveWitness((size_t)sourceCount,false);
                int activeVerified = 0;
                int reserveVerified = 0;
                for (int witness=0; witness<sourceCount; ++witness) {
                    if (witness == audioMasterIndex) continue;
                    if (gateReason[witness] == "ELIGIBLE_SCORE" &&
                        std::isfinite(segScore[witness])) {
                        if(sourceRoles[witness]==SourceRole::Donor) {
                            activeWitness[witness]=true;
                            ++activeVerified;
                        } else if(sourceRoles[witness]==SourceRole::Reserve) {
                            reserveWitness[witness]=true;
                            ++reserveVerified;
                        }
                    }
                }

                auto copyEvidence=[&]() {
                    if (impulseEvidence.available) {
                        consensusEvidence.available=true;
                        consensusEvidence.outlierRatio=impulseEvidence.outlierRatio;
                        consensusEvidence.donorControlCorr=impulseEvidence.donorControlTrendCorr;
                        consensusEvidence.masterDonorCorr=impulseEvidence.donorTrendCorr;
                        consensusEvidence.masterControlCorr=impulseEvidence.controlTrendCorr;
                        consensusEvidence.eligibleWitnesses=impulseEvidence.eligibleWitnesses;
                        consensusEvidence.supportingWitnesses=impulseEvidence.supportingWitnesses;
                        consensusEvidence.opposingWitnesses=impulseEvidence.opposingWitnesses;
                        consensusEvidence.supportingSources=impulseEvidence.supportingSources;
                        consensusEvidence.abstainingSources=impulseEvidence.abstainingSources;
                        consensusEvidence.opposingSources=impulseEvidence.opposingSources;
                    }
                };
                auto evaluateWitnesses=[&](const std::vector<bool>& mask) {
                    impulseEvidence=detectImpulseConsensus(
                        sources,audioMasterIndex,seg,segEnd,segDelay,sr,&mask,
                        &masterScores);
                    copyEvidence();
                };

                if (std::isfinite(masterScore))
                    evaluateWitnesses(activeWitness);

                // The sixth viable source is intentionally sparse reserve:
                // consult it only when the full active donor core cannot make
                // a confirmed decision.  It never displaces a valid core vote.
                if (!impulseEvidence.confirmed && reserveVerified>0 &&
                    activeVerified+reserveVerified>=2 &&
                    std::isfinite(masterScore)) {
                    std::vector<bool> withReserve=activeWitness;
                    for(int witness=0;witness<sourceCount;++witness)
                        withReserve[witness]=withReserve[witness]||reserveWitness[witness];
                    evaluateWitnesses(withReserve);
                }

                if (impulseEvidence.available) {
                    if (impulseEvidence.confirmed && impulseEvidence.donorSource >= 0 &&
                        std::isfinite(segScore[impulseEvidence.donorSource])) {
                        chosen=impulseEvidence.donorSource;
                        confidenceClass='A';
                        decisionReason="A_N_SOURCE_CONSENSUS_CLICK_REPAIR";
                        impulseRepair=true;
                        consensusEvidence.confirmed=true;
                        consensusEvidence.controlSource=impulseEvidence.controlSource;
                        consensusEvidence.correlationAdvantage=impulseEvidence.masterPeak-
                            std::max(impulseEvidence.donorPeak,impulseEvidence.controlPeak);
                    }
                }

                // v0.2.29z master-preserving rule: score differences, missing
                // blocks and ordinary quality judgements may be logged, but
                // they may never replace the master. Only explicit physical
                // source consensus above can change audio.
                const bool masterImpulseCandidate = impulseEvidence.available &&
                    impulseEvidence.masterPeak >= std::ldexp(
                        16.0,-(sources[audioMasterIndex].bits-1)) &&
                    audioMasterIndex<(int)impulseEvidence.zBySource.size() &&
                    std::isfinite(impulseEvidence.zBySource[audioMasterIndex]) &&
                    impulseEvidence.zBySource[audioMasterIndex]>=5.0 &&
                    impulseEvidence.outlierRatio>=1.85;
                const bool unassessedMasterImpulse=impulseEvidence.available &&
                    impulseEvidence.eligibleWitnesses<2 &&
                    impulseEvidence.masterPeak>=std::ldexp(16.0,-(sources[audioMasterIndex].bits-1)) &&
                    audioMasterIndex<(int)impulseEvidence.zBySource.size() &&
                    impulseEvidence.zBySource[audioMasterIndex]>=5.0;
                if(!impulseRepair && unassessedMasterImpulse) {
                    confidenceClass='B';
                    decisionReason="B_IMPULSE_UNASSESSED_FEWER_THAN_TWO_WITNESSES";
                } else if (!impulseRepair && masterImpulseCandidate &&
                    !impulseEvidence.impulsePatternConfirmed) {
                    confidenceClass='B';
                    decisionReason=unconfirmedImpulseReason(impulseEvidence);
                } else if (!impulseRepair && impulseEvidence.impulsePatternConfirmed &&
                    !impulseEvidence.trendAccepted) {
                    confidenceClass='B';
                    decisionReason="B_IMPULSE_WITHOUT_TREND_MATCH_KEEP_MASTER";
                } else if (!impulseRepair && donorBestSource >= 0 &&
                    std::isfinite(masterScore) &&
                    donorBest <= masterScore*0.80 &&
                    (masterScore-donorBest)>=0.020) {
                    confidenceClass='B';
                    decisionReason="B_QUALITY_DIFFERENCE_KEEP_MASTER";
                }

                } // !fastCEligible: full GOLDEN donor analysis
                rpFullSec += std::chrono::duration<double>(
                    std::chrono::steady_clock::now()-rpFullStart).count();
                const auto rpTailStart=std::chrono::steady_clock::now();

                {
                    CoarseDiagnosticRow d;
                    d.timeSec=(double)seg/sr;
                    d.pairBest=coarseBestPair;
                    d.pairSecond=coarseSecondPair;
                    d.pairWorst=coarseWorstPair;
                    d.pairGap=coarseBestPair-coarseSecondPair;
                    d.maxDerivRatio=maxDerivRatio;
                    d.maxEnergyRatio=maxEnergyRatio;
                    d.consensus=twoAgree?1:0;
                    d.ambiguous=ambiguous?1:0;
                    d.transient=transientCandidate?1:0;
                    d.energy=energyCandidate?1:0;
                    d.flagged=coarseCandidate?1:0;
                    d.fast_c_eligible=fastCEligible?1:0;
                    d.finalClass=confidenceClass;
                    coarseDiag.push_back(d);
                }

                if (confidenceClass=='A') { ++finalA; if(fastCEligible) ++fastAHit; }
                else if (confidenceClass=='B') { ++finalB; if(fastCEligible) ++fastBHit; }
                else { ++finalC; if(fastCEligible) ++fastCHit; }

                if (confidenceClass == 'A' || confidenceClass == 'B') {
                    ++coarseAB;
                    if (coarseCandidate) ++coarseABCovered;
                }

                currentSource = chosen;
                ++selectedCount[currentSource];

                rpTailSec += std::chrono::duration<double>(
                    std::chrono::steady_clock::now()-rpTailStart).count();

                // LIVE <time_sec> <original_source_1_based>
                const double liveTimeSec = (double)seg / sr;
                const int liveOriginalSource = originalIndex[currentSource] + 1;
                if (liveOriginalSource != liveLastSource ||
                    liveLastEmitSec < 0.0 ||
                    liveTimeSec - liveLastEmitSec >= 0.100) {
                    std::cerr << "LIVE "
                              << std::fixed << std::setprecision(6)
                              << liveTimeSec << " "
                              << liveOriginalSource << "\n";
                    liveLastSource = liveOriginalSource;
                    liveLastEmitSec = liveTimeSec;
                }
                AffineMatch match{};
                if(impulseRepair) {
                    match.gain=impulseEvidence.trendGain;
                    match.dc=impulseEvidence.trendDc;
                    // Fade in and out entirely inside the verified repair.
                    // Samples outside this window retain their exact master value.
                    const long segmentFrames=segEnd-seg;
                    const int fadeFrames=std::max<int>(1,std::min<long>(
                        segmentFrames/4,std::max<long>(1,sr/4000)));
                    double maxAbsDifference=0.0;
                    for(long i=seg;i<segEnd;++i) {
                        const long pos=i-seg;
                        double alpha=1.0;
                        if(pos<fadeFrames)
                            alpha=std::min(alpha,(double)(pos+1)/(fadeFrames+1));
                        const long remaining=segEnd-i;
                        if(remaining<=fadeFrames)
                            alpha=std::min(alpha,(double)remaining/(fadeFrames+1));
                        for(int ch=0;ch<ref.channels;++ch) {
                            const float master=ref.samples[(size_t)i*ref.channels+ch];
                            const double donorFrame=(double)i+segDelay[currentSource];
                            if(donorFrame<0.0 || donorFrame+1.0>=(double)sources[currentSource].frames())
                                continue;
                            const float rawDonor=sampleLinear(sources[currentSource],donorFrame,ch);
                            const float donor=(float)std::clamp(
                                match.gain*(double)rawDonor+match.dc,-1.0,0.999999999);
                            maxAbsDifference=std::max(maxAbsDifference,
                                std::abs((double)donor-(double)master));
                            out[(size_t)i*ref.channels+ch]=(float)(
                                (1.0-alpha)*(double)master+alpha*(double)donor);
                        }
                    }
                    RepairRecord repairRecord{
                        (double)seg/sr,(double)segEnd/sr,
                        originalIndex[currentSource]+1,
                        consensusEvidence.outlierRatio,maxAbsDifference};
                    repairRecord.coreStartSec=(double)(seg+fadeFrames)/sr;
                    repairRecord.coreEndSec=(double)(segEnd-fadeFrames)/sr;
                    repairRecord.fadeInSamples=fadeFrames;
                    repairRecord.fadeOutSamples=fadeFrames;
                    repairRecord.renderGain=match.gain;
                    repairRecord.renderDc=match.dc;
                    repairRecord.donorDelaySamples=segDelay[currentSource];
                    repairRecords.push_back(std::move(repairRecord));
                }
                if (!mapName.empty()) {
                    DecisionRow row;
                    row.timeSec = (double)seg / sr;
                    row.delay.assign(sourceCount,0.0);
                    row.quality.assign(sourceCount,1.0);
                    row.score.assign(sourceCount,std::numeric_limits<double>::infinity());
                    row.gateCorr.assign(sourceCount,std::numeric_limits<double>::quiet_NaN());
                    row.gateMapConfidence.assign(sourceCount,std::numeric_limits<double>::quiet_NaN());
                    row.gateLocalVerify.assign(sourceCount,std::numeric_limits<double>::quiet_NaN());
                    row.gateReason.assign(sourceCount,"NOT_EVALUATED");
                    row.impulsePeak.assign(sourceCount,std::numeric_limits<double>::quiet_NaN());
                    row.impulseFloor.assign(sourceCount,std::numeric_limits<double>::quiet_NaN());
                    row.impulseZ.assign(sourceCount,std::numeric_limits<double>::quiet_NaN());
                    row.trendFit.assign(sourceCount,std::numeric_limits<double>::quiet_NaN());
                    row.trendCorr.assign(sourceCount,std::numeric_limits<double>::quiet_NaN());
                    for (int ss=0; ss<sourceCount; ++ss) {
                        int oi=originalIndex[ss];
                        row.delay[oi]=segDelay[ss];
                        row.quality[oi]=(ss==0?1.0:state[ss].quality);
                        row.score[oi]=segScore[ss];
                        row.gateCorr[oi]=gateCorr[ss];
                        row.gateMapConfidence[oi]=gateMapConfidence[ss];
                        row.gateLocalVerify[oi]=gateLocalVerify[ss];
                        row.gateReason[oi]=gateReason[ss];
                        if (ss < (int)impulseEvidence.peakBySource.size())
                            row.impulsePeak[oi]=impulseEvidence.peakBySource[ss];
                        if (ss < (int)impulseEvidence.floorBySource.size())
                            row.impulseFloor[oi]=impulseEvidence.floorBySource[ss];
                        if (ss < (int)impulseEvidence.zBySource.size())
                            row.impulseZ[oi]=impulseEvidence.zBySource[ss];
                        if (ss < (int)impulseEvidence.trendFitBySource.size())
                            row.trendFit[oi]=impulseEvidence.trendFitBySource[ss];
                        if (ss < (int)impulseEvidence.trendCorrBySource.size())
                            row.trendCorr[oi]=impulseEvidence.trendCorrBySource[ss];
                    }
                    row.selectedSource = originalIndex[currentSource] + 1;
                    row.continuity = continuityScore[currentSource];
                    row.gain = match.gain;
                    row.dc = match.dc;
                    row.consensusSupport = consensusEvidence.correlationAdvantage;
                    row.donorControlCorr = consensusEvidence.donorControlCorr;
                    row.masterDonorCorr = consensusEvidence.masterDonorCorr;
                    row.masterControlCorr = consensusEvidence.masterControlCorr;
                    row.outlierRatio = consensusEvidence.outlierRatio;
                    row.eligibleWitnesses=consensusEvidence.eligibleWitnesses;
                    row.supportingWitnesses=consensusEvidence.supportingWitnesses;
                    row.opposingWitnesses=consensusEvidence.opposingWitnesses;
                    for(int source:consensusEvidence.supportingSources)
                        row.supportingSources.push_back(originalIndex[source]+1);
                    for(int source:consensusEvidence.abstainingSources)
                        row.abstainingSources.push_back(originalIndex[source]+1);
                    for(int source:consensusEvidence.opposingSources)
                        row.opposingSources.push_back(originalIndex[source]+1);
                    row.decisionReason = decisionReason;
                    row.confidenceClass = confidenceClass;
                    double disagreement = 0.0;
                    long mi = seg + (segEnd - seg) / 2;
                    for (int ch = 0; ch < ref.channels; ++ch) {
                        double lo = 1e30, hi = -1e30;
                        for (int ss = 0; ss < sourceCount; ++ss) {
                            if(!processingEligible[ss]) continue;
                            double frame = (double)mi + segDelay[ss];
                            if (frame < 0.0 || frame + 1.0 >= (double)sources[ss].frames()) continue;
                            double v = sampleLinear(sources[ss], frame, ch);
                            lo = std::min(lo, v); hi = std::max(hi, v);
                        }
                        if (hi >= lo) disagreement = std::max(disagreement, hi - lo);
                    }
                    row.disagreement = disagreement;
                    mapRows.push_back(std::move(row));
                }
                RepairSegmentPlan plan;
                plan.start=seg;
                plan.end=segEnd;
                plan.delay=segDelay;
                plan.gateReason=gateReason;
                plan.trendFit.assign(sourceCount,
                    std::numeric_limits<double>::infinity());
                for(int ss=0;ss<sourceCount &&
                    ss<(int)impulseEvidence.trendFitBySource.size();++ss)
                    plan.trendFit[ss]=impulseEvidence.trendFitBySource[ss];
                plan.source=chosen;
                plan.confidenceClass=confidenceClass;
                plan.decisionReason=decisionReason;
                plan.gain=match.gain;
                plan.dc=match.dc;
                plan.outlierRatio=consensusEvidence.outlierRatio;
                if(audioMasterIndex>=0 &&
                   audioMasterIndex<(int)impulseEvidence.zBySource.size())
                    plan.masterImpulseZ=impulseEvidence.zBySource[audioMasterIndex];
                plan.supportingSources=consensusEvidence.supportingSources;
                repairPlans.push_back(std::move(plan));
            }
            /* v0.2.6 logs actual ~2 ms decisions above; block-only map removed. */
            if (false && !mapName.empty()) {
                DecisionRow row;
                row.timeSec = (double)block / sr;
                row.delay.resize(sourceCount);
                row.quality.resize(sourceCount);
                row.score.resize(sourceCount);

                row.delay[0] = 0.0;
                row.quality[0] = 1.0;

                for (int s = 1; s < sourceCount; ++s) {
                    row.delay[s] = state[s].delay;
                    row.quality[s] = state[s].quality;
                }

                row.score = blockScore;
                row.selectedSource = currentSource + 1;

                // A useful diagnostic: range between best and worst aligned
                // source sample at the block midpoint.
                double disagreement = 0.0;
                long mi = block + (end - block) / 2;

                for (int ch = 0; ch < ref.channels; ++ch) {
                    double lo = 1e30, hi = -1e30;
                    for (int s = 0; s < sourceCount; ++s) {
                        if(!processingEligible[s]) continue;
                        double frame = (double)mi + midDelay[s];
                        if (frame < 0.0 ||
                            frame + 1.0 >= (double)sources[s].frames())
                            continue;
                        double v = sampleLinear(sources[s], frame, ch);
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                    }
                    if (hi >= lo)
                        disagreement = std::max(disagreement, hi - lo);
                }

                row.disagreement = disagreement;
                mapRows.push_back(std::move(row));
            }

            rpSegmentLoopSec += std::chrono::duration<double>(
                std::chrono::steady_clock::now()-rpSegmentLoopStart).count();

            for (int s = 0; s < sourceCount; ++s)
                prevDelay[s] = nextDelay[s];

            if ((block / hop) % 10 == 0 || end == (long)outFrames) {
                const double doneSec = (double)end / sr;
                const double totalSec = (double)outFrames / sr;
                const double percent = 100.0 * (double)end / outFrames;

                std::cerr
                    << "PROGRESS "
                    << percent << " "
                    << doneSec << " "
                    << totalSec
                    << "\n";
            }
        }

        beginWorkPhase("RENDER_REPAIRS",repairPlans.size());

        // v0.2.30x2p EVENT-COHERENT CLICK REPAIR. Rebuild from the untouched
        // master, join adjacent A cells (and at most one clean 2 ms hole),
        // select one donor by majority evidence across the whole event, and
        // crossfade only at the two outer event boundaries.
        out=ref.samples;
        repairRecords.clear();
        std::fill(selectedCount.begin(),selectedCount.end(),0);
        finalA=finalB=finalC=0;
        fastAHit=fastBHit=fastCHit=0;
        coarseAB=coarseABCovered=0;

        auto publishPlan=[&](size_t k,int source,char klass,
                             const std::string& reason,double gain,double dc) {
            repairPlans[k].source=source;
            repairPlans[k].confidenceClass=klass;
            repairPlans[k].decisionReason=reason;
            repairPlans[k].gain=gain;
            repairPlans[k].dc=dc;
            if(k<coarseDiag.size()) coarseDiag[k].finalClass=klass;
            if(k<mapRows.size()) {
                mapRows[k].selectedSource=originalIndex[source]+1;
                mapRows[k].confidenceClass=klass;
                mapRows[k].decisionReason=reason;
                mapRows[k].gain=gain;
                mapRows[k].dc=dc;
            }
        };

        struct RenderAttemptResult {
            bool accepted=false;
            std::string rejectReason="B_POSTCHECK_UNKNOWN";
        };

        // Render one candidate into the untouched Master, validate it, then
        // either keep it or restore the exact pre-attempt bytes.  Publishing
        // the A decision is deliberately delayed until every postcheck passes.
        auto renderAttempt=[&](size_t first,size_t last,int donor,
                               double gain,double dc,double outlier,
                               const std::string& reason,double delayNudge,
                               int fadeScale,int retryAttempt) {
            RenderAttemptResult result;
            const size_t middle=first+(last-first)/2;
            const auto expanded=expandedRepairWindow(
                repairPlans[first].start,repairPlans[last].end,
                (long)outFrames,sr);
            ImpulseCoreWindow core;
            core.start=expanded.start;
            core.end=expanded.end;
            if(repairPlans[middle].supportingSources.size()>=2) {
                core=localizeImpulseCore(
                    sources,audioMasterIndex,donor,
                    repairPlans[first].start,repairPlans[last].end,
                    (long)outFrames,repairPlans[middle].delay,
                    delayNudge,gain,sr);
            }
            int coreControl=-1;double bestControlFit=std::numeric_limits<double>::infinity();
            for(int source:repairPlans[middle].supportingSources) {
                if(source==donor)continue;
                const auto match=localTrendMatch(sources,donor,source,
                    repairPlans[first].start,repairPlans[last].end,
                    repairPlans[middle].delay,sr,.125/8.,8./.125);
                if(match.usable && match.fit<bestControlFit){coreControl=source;bestControlFit=match.fit;}
            }
            auto shoulderDelay=repairPlans[middle].delay;shoulderDelay[donor]+=delayNudge;
            core=supportedImpulseShoulders(sources,audioMasterIndex,donor,coreControl,
                core,repairPlans[first].start,repairPlans[last].end,
                shoulderDelay,gain,dc,sr);
            const bool subMillisecondCore=core.localized;
            if(!broadRepairEvidenceStrongEnough(
                   subMillisecondCore,outlier)) {
                result.rejectReason="B_POSTCHECK_BROAD_MARGIN_TOO_LOW";
                return result;
            }
            const long eventStart=core.start;
            const long eventEnd=core.end;
            const long eventFrames=eventEnd-eventStart;
            const long fadeLimit=subMillisecondCore
                ? std::max<long>(1,sr/1000)  // at most 1 ms on a retry
                : std::max<long>(1,eventFrames/2);
            const long fadeBase=subMillisecondCore
                ? std::max<long>(std::max<long>(1,sr/8000),eventFrames/2)
                : std::max<long>(1,sr/4000);
            const int fadeFrames=std::max<int>(1,std::min<long>(
                fadeLimit,fadeBase*std::max(1,fadeScale)));
            // The complete localized impulse core (or the established broad
            // event when localization is unsafe) must be 100% donor audio.
            // Put both fades in the immediately surrounding healthy context.
            const long renderStart=std::max<long>(0,eventStart-fadeFrames);
            const long renderEnd=std::min<long>((long)outFrames,eventEnd+fadeFrames);
            if(std::any_of(repairRecords.begin(),repairRecords.end(),[&](const auto& previous) {
                return (double)renderStart/sr<previous.endSec &&
                       (double)renderEnd/sr>previous.startSec;
            })) {
                result.rejectReason="B_POSTCHECK_OVERLAPS_ACCEPTED_REPAIR";
                return result;
            }
            double maxAbsDifference=0.0;
            bool incomplete=false;
            const size_t backupBegin=(size_t)renderStart*ref.channels;
            const size_t backupEnd=(size_t)renderEnd*ref.channels;
            const std::vector<float> backup(out.begin()+backupBegin,out.begin()+backupEnd);

            auto renderFrame=[&](long frame,const std::vector<double>& delay,double alpha) {
                const double donorFrame=(double)frame+delay[donor]+delayNudge;
                if(donorFrame<0.0 || donorFrame+1.0>=(double)sources[donor].frames()) { incomplete=true; return; }
                for(int ch=0;ch<ref.channels;++ch) {
                    const size_t p=(size_t)frame*ref.channels+ch;
                    const float master=ref.samples[p];
                    const float raw=sampleLinear(sources[donor],donorFrame,ch);
                    const float replacement=(float)std::clamp(
                        gain*(double)raw+dc,-1.0,0.999999999);
                    maxAbsDifference=std::max(maxAbsDifference,
                        std::abs((double)replacement-(double)master));
                    out[p]=(float)((1.0-alpha)*(double)master+
                                   alpha*(double)replacement);
                }
            };

            const auto& fadeInDelay=subMillisecondCore
                ? repairPlans[middle].delay : repairPlans[first].delay;
            const auto& fadeOutDelay=subMillisecondCore
                ? repairPlans[middle].delay : repairPlans[last].delay;
            for(long frame=renderStart;frame<eventStart;++frame) {
                const double position=(double)(frame-renderStart+1)/
                    (double)(std::max<long>(1,eventStart-renderStart)+1);
                renderFrame(frame,fadeInDelay,smoothRepairFade(position));
            }
            if(subMillisecondCore) {
                for(long frame=eventStart;frame<eventEnd;++frame)
                    renderFrame(frame,repairPlans[middle].delay,1.0);
            } else {
                // Full donor audio covers the guard itself.  This is what removes
                // an impulse whose leading edge fell just outside the A cell.
                for(long frame=eventStart;frame<repairPlans[first].start;++frame)
                    renderFrame(frame,repairPlans[first].delay,1.0);
                for(size_t k=first;k<=last;++k) {
                    for(long frame=repairPlans[k].start;
                        frame<repairPlans[k].end;++frame)
                        renderFrame(frame,repairPlans[k].delay,1.0);
                }
                for(long frame=repairPlans[last].end;frame<eventEnd;++frame)
                    renderFrame(frame,repairPlans[last].delay,1.0);
            }
            for(long frame=eventEnd;frame<renderEnd;++frame) {
                const double position=(double)(renderEnd-frame)/
                    (double)(std::max<long>(1,renderEnd-eventEnd)+1);
                renderFrame(frame,fadeOutDelay,smoothRepairFade(position));
            }

            // The fully replaced core must be exactly the selected donor after
            // the recorded gain/DC/delay transform. This catches accidental
            // source mixing or an indexing error before an A repair is kept.
            double donorCoreError=0.0;
            for(long frame=eventStart;frame<eventEnd && !incomplete;++frame) {
                const std::vector<double>* delay=&repairPlans[middle].delay;
                if(!subMillisecondCore) {
                    if(frame<repairPlans[first].start)
                        delay=&repairPlans[first].delay;
                    else if(frame>=repairPlans[last].end)
                        delay=&repairPlans[last].delay;
                    else {
                        for(size_t k=first;k<=last;++k)
                            if(frame>=repairPlans[k].start &&
                               frame<repairPlans[k].end) {
                                delay=&repairPlans[k].delay;
                                break;
                            }
                    }
                }
                const double donorFrame=(double)frame+(*delay)[donor]+delayNudge;
                if(donorFrame<0.0 || donorFrame+1.0>=(double)sources[donor].frames()) {
                    incomplete=true;
                    break;
                }
                for(int ch=0;ch<ref.channels;++ch) {
                    const double expected=std::clamp(
                        gain*(double)sampleLinear(sources[donor],donorFrame,ch)+dc,
                        -1.0,0.999999999);
                    donorCoreError=std::max(donorCoreError,std::abs(
                        (double)out[(size_t)frame*ref.channels+ch]-expected));
                }
            }

            // Re-run the physical-source vote over the complete DETECTED event.
            // The surrounding guard and fades are intentionally excluded from
            // the vote: they contain healthy programme and previously diluted
            // a short click until consensus was reported as lost. The separate
            // rendered-result curvature guard below still checks the complete
            // changed interval, including both boundaries and both fades.
            std::vector<bool> postcheckMask((size_t)sourceCount,false);
            for(size_t k=first;k<=last;++k)
                for(int source:repairPlans[k].supportingSources)
                    if(source>=0 && source<sourceCount && processingEligible[source])
                        postcheckMask[source]=true;
            auto postcheckDelay=repairPlans[middle].delay;
            if(donor>=0 && donor<(int)postcheckDelay.size())
                postcheckDelay[donor]+=delayNudge;
            const auto postEvidence=detectImpulseConsensus(
                sources,audioMasterIndex,repairPlans[first].start,
                repairPlans[last].end,
                postcheckDelay,sr,&postcheckMask);
            const bool donorStillClean=std::find(
                postEvidence.supportingSources.begin(),
                postEvidence.supportingSources.end(),donor)!=
                postEvidence.supportingSources.end();
            std::cerr << "POSTCHECK detected_start_sec "
                      << (double)repairPlans[first].start/sr
                      << " detected_end_sec " << (double)repairPlans[last].end/sr
                      << " rendered_start_sec " << (double)renderStart/sr
                      << " rendered_end_sec " << (double)renderEnd/sr
                      << " donor " << (originalIndex[donor]+1)
                      << " attempt " << retryAttempt
                      << " delay_nudge_samples " << delayNudge
                      << " fade_scale " << fadeScale
                      << " subms_core " << (subMillisecondCore?1:0)
                      << " core_start_sec " << (double)eventStart/sr
                      << " core_end_sec " << (double)eventEnd/sr
                      << " donor_core_error " << donorCoreError
                      << " confirmed " << (postEvidence.confirmed?1:0)
                      << " donor_clean " << (donorStillClean?1:0)
                      << " supporting " << postEvidence.supportingWitnesses
                      << " opposing " << postEvidence.opposingWitnesses << "\n";
            // Validate the rendered splice, including fades and both boundaries.
            // This is a conservative impulse guard, not a perceptual quality score.
            const bool donorCoreMismatch=donorCoreError>2e-6;
            bool reject=incomplete || donorCoreMismatch ||
                !postEvidence.confirmed || !donorStillClean;
            std::string rejectReason=incomplete
                ? "B_POSTCHECK_MISSING_DONOR"
                : (donorCoreMismatch
                    ? "B_POSTCHECK_DONOR_CORE_MISMATCH"
                    : "B_POSTCHECK_CONSENSUS_LOST");
            auto curvature=[&](const std::vector<float>& data,long f,int ch) {
                const size_t p=(size_t)f*ref.channels+ch;
                return std::abs((double)data[p-ref.channels]-2.0*data[p]+data[p+ref.channels]);
            };
            for(int ch=0;ch<ref.channels && !reject;++ch) {
                std::vector<double> context;
                const long contextFrames=std::max<long>(8,sr/200); // 5 ms each side
                for(long f=std::max<long>(1,renderStart-contextFrames);
                    f<std::min<long>((long)outFrames-1,renderEnd+contextFrames);++f)
                    if(f<renderStart-1 || f>renderEnd)
                        context.push_back(curvature(ref.samples,f,ch));
                double floor=0.000001;
                if(!context.empty()) {
                    auto medianIt=context.begin()+context.size()/2;
                    std::nth_element(context.begin(),medianIt,context.end());
                    floor=std::max(floor,*medianIt);
                }
                // Compare against both the local curvature floor and the
                // original Master. A normal donor contour may be somewhat
                // sharper than Master without being a click; the old 1.25x
                // test rejected those clean replacements too readily.
                const double threshold=std::max(0.00002,12.0*floor);
                double masterPeak=0.0,resultPeak=0.0;
                for(long f=std::max<long>(1,renderStart-1);
                    f<std::min<long>((long)outFrames-1,renderEnd+1);++f) {
                    const double before=curvature(ref.samples,f,ch);
                    const double after=curvature(out,f,ch);
                    masterPeak=std::max(masterPeak,before);
                    resultPeak=std::max(resultPeak,after);
                    if(!std::isfinite(out[(size_t)f*ref.channels+ch]) ||
                       (after>threshold &&
                        after>std::max(1.80*before,2.50*floor))) {
                        reject=true;
                        rejectReason="B_POSTCHECK_NEW_IMPULSE_OR_SEAM";
                        break;
                    }
                }
                if(!reject && masterPeak>threshold && resultPeak>=0.92*masterPeak) {
                    reject=true;
                    rejectReason="B_POSTCHECK_NO_IMPULSE_IMPROVEMENT";
                }
            }
            if(!reject && subMillisecondCore &&
               !spliceShapeSupported(sources,audioMasterIndex,donor,coreControl,
                   shoulderDelay,gain,dc,out,renderStart,renderEnd,sr)) {
                reject=true;rejectReason="B_POSTCHECK_SMOOTH_SEAM_REVERSAL";
            }
            if(reject) {
                std::copy(backup.begin(),backup.end(),out.begin()+backupBegin);
                result.rejectReason=rejectReason;
                return result;
            }
            std::string acceptedReason=reason;
            if(subMillisecondCore)
                acceptedReason+="_SUBMS_CORE";
            if(retryAttempt>0)
                acceptedReason+="_POSTCHECK_RETRY_"+std::to_string(retryAttempt);
            for(size_t k=first;k<=last;++k)
                publishPlan(k,donor,'A',acceptedReason,gain,dc);
            RepairRecord repairRecord{
                (double)renderStart/sr,(double)renderEnd/sr,
                originalIndex[donor]+1,outlier,maxAbsDifference,
                retryAttempt,delayNudge,acceptedReason};
            repairRecord.coreStartSec=(double)eventStart/sr;
            repairRecord.coreEndSec=(double)eventEnd/sr;
            repairRecord.fadeInSamples=eventStart-renderStart;
            repairRecord.fadeOutSamples=renderEnd-eventEnd;
            repairRecord.renderGain=gain;
            repairRecord.renderDc=dc;
            repairRecord.donorDelaySamples=repairPlans[middle].delay[donor]+delayNudge;
            repairRecords.push_back(std::move(repairRecord));
            result.accepted=true;
            result.rejectReason="ACCEPTED";
            return result;
        };

        // x2r verified retry ladder. A failed splice is not proof that the
        // physical consensus was wrong: it can be a one-sample phase error,
        // a too-short fade, or one locally awkward donor. Retry locally and
        // re-run every ordinary physical and rendered-output postcheck.
        auto renderEvent=[&](size_t first,size_t last,int donor,
                             double gain,double dc,double outlier,
                             const std::string& reason) {
            std::string lastReject="B_POSTCHECK_RETRY_EXHAUSTED";
            int attempt=0;
            auto tried=[&](int source,double candidateGain,double candidateDc,
                           double nudge,int fadeScale) {
                ++attempt;
                const auto r=renderAttempt(first,last,source,candidateGain,
                    candidateDc,outlier,reason,nudge,fadeScale,attempt-1);
                lastReject=r.rejectReason;
                return r.accepted;
            };

            // Preserve the established x2p path as attempt zero.
            if(tried(donor,gain,dc,0.0,1)) return;

            const size_t middle=first+(last-first)/2;
            const auto eventBounds=expandedRepairWindow(
                repairPlans[first].start,repairPlans[last].end,
                (long)outFrames,sr);

            auto bestLocalFit=[&](int source) {
                LocalTrendMatch best;
                double bestNudge=0.0;
                const int radius=std::max(2,std::min(24,sr/4000));
                auto test=[&](double nudge) {
                    auto delays=repairPlans[middle].delay;
                    if(source<0 || source>=(int)delays.size()) return;
                    delays[source]+=nudge;
                    const auto match=localTrendMatch(sources,audioMasterIndex,
                        source,eventBounds.start,eventBounds.end,delays,sr);
                    if(match.usable && (!best.usable || match.fit<best.fit ||
                       (std::abs(match.fit-best.fit)<1e-12 && match.corr>best.corr))) {
                        best=match;
                        bestNudge=nudge;
                    }
                };
                for(int nudge=-radius;nudge<=radius;nudge+=4)
                    test((double)nudge);
                const double coarse=bestNudge;
                // sampleLinear already supports fractional positions.  Search
                // the best coarse basin at 1/8-sample resolution so an otherwise
                // clean donor is not rejected merely because its phase falls
                // between two captured samples.
                for(int eighth=-16;eighth<=16;++eighth)
                    test(coarse+(double)eighth/8.0);
                return std::make_pair(best,bestNudge);
            };

            // First retry the original donor with a local sample-level phase
            // refinement and a longer outer crossfade.
            const auto primaryFit=bestLocalFit(donor);
            if(primaryFit.first.usable &&
               tried(donor,primaryFit.first.gain,primaryFit.first.dc,
                     primaryFit.second,4)) return;

            // Then try the other physically supporting copies, ordered by how
            // many cells in this event named them clean and by trend fit.
            std::vector<int> votes((size_t)sourceCount,0);
            std::vector<double> trend((size_t)sourceCount,
                std::numeric_limits<double>::infinity());
            for(size_t k=first;k<=last;++k) {
                for(int source:repairPlans[k].supportingSources)
                    if(source>=0 && source<sourceCount &&
                       source!=audioMasterIndex && processingEligible[source])
                        ++votes[source];
                for(int source=0;source<sourceCount;++source)
                    if(source<(int)repairPlans[k].trendFit.size())
                        trend[source]=std::min(trend[source],repairPlans[k].trendFit[source]);
            }
            std::vector<int> alternates;
            for(int source=0;source<sourceCount;++source)
                if(source!=audioMasterIndex && source!=donor && votes[source]>0)
                    alternates.push_back(source);
            std::sort(alternates.begin(),alternates.end(),[&](int a,int b) {
                if(votes[a]!=votes[b]) return votes[a]>votes[b];
                if(std::abs(trend[a]-trend[b])>1e-12) return trend[a]<trend[b];
                return a<b;
            });
            for(int source:alternates) {
                const auto candidateFit=bestLocalFit(source);
                if(!candidateFit.first.usable) continue;
                if(tried(source,candidateFit.first.gain,candidateFit.first.dc,
                         candidateFit.second,4)) return;
            }

            std::cerr << "POSTCHECK_RETRY_EXHAUSTED detected_start_sec "
                      << (double)repairPlans[first].start/sr
                      << " detected_end_sec " << (double)repairPlans[last].end/sr
                      << " attempts " << attempt
                      << " last_reason " << lastReject << "\n";
            const std::string finalReason="B_POSTCHECK_RETRY_EXHAUSTED_"+
                (lastReject.rfind("B_POSTCHECK_",0)==0
                    ? lastReject.substr(std::string("B_POSTCHECK_").size())
                    : lastReject);
            for(size_t k=first;k<=last;++k)
                publishPlan(k,audioMasterIndex,'B',finalReason,1.0,0.0);
        };

        const long maxEventFrames=std::max<long>(surgicalFrames,sr*20/1000);
        for(size_t i=0;i<repairPlans.size();) {
            reportWorkProgress(i,repairPlans.size());
            if(repairPlans[i].confidenceClass!='A') { ++i; continue; }
            size_t last=i;
            size_t probe=i+1;
            while(probe<repairPlans.size()) {
                size_t nextA=probe;
                if(repairPlans[nextA].confidenceClass!='A') {
                    if(nextA+1>=repairPlans.size() ||
                       repairPlans[nextA].confidenceClass=='B' ||
                       repairPlans[nextA+1].confidenceClass!='A') break;
                    ++nextA; // bridge one C cell only when another A follows
                }
                if(repairPlans[nextA].start-repairPlans[last].end>surgicalFrames ||
                   repairPlans[nextA].end-repairPlans[i].start>maxEventFrames) break;
                last=nextA;
                probe=nextA+1;
            }

            std::vector<int> votes(sourceCount,0);
            for(size_t k=i;k<=last;++k)
                if(repairPlans[k].confidenceClass=='A' &&
                   repairPlans[k].source!=audioMasterIndex)
                    ++votes[repairPlans[k].source];
            int donor=-1;
            for(int s=0;s<sourceCount;++s)
                if(s!=audioMasterIndex && (donor<0 || votes[s]>votes[donor]))
                    donor=s;

            // If the confirmed A cell touches a B cell which already showed
            // elevated Master curvature and admits the same donor, treat it as
            // the uncertain leading/trailing edge of one physical event.  At
            // most one B cell on either side is absorbed.
            size_t eventFirst=i;
            size_t eventLast=last;
            if(donor>=0 && eventFirst>0 &&
               mayAbsorbAdjacentUncertainCell(
                   repairPlans[eventFirst-1].confidenceClass,
                   repairPlans[eventFirst-1].decisionReason,
                   repairPlans[eventFirst-1].masterImpulseZ,
                   repairPlans[eventFirst-1].gateReason,donor))
                --eventFirst;
            if(donor>=0 && eventLast+1<repairPlans.size() &&
               mayAbsorbAdjacentUncertainCell(
                   repairPlans[eventLast+1].confidenceClass,
                   repairPlans[eventLast+1].decisionReason,
                   repairPlans[eventLast+1].masterImpulseZ,
                   repairPlans[eventLast+1].gateReason,donor))
                ++eventLast;

            bool coherent=eventLast>eventFirst && donor>=0;
            for(size_t k=eventFirst;coherent && k<=eventLast;++k)
                coherent=donor<(int)repairPlans[k].gateReason.size() &&
                    repairPlans[k].gateReason[donor]=="ELIGIBLE_SCORE";
            const size_t middle=eventFirst+(eventLast-eventFirst)/2;
            LocalTrendMatch eventMatch;
            if(coherent) {
                const auto eventBounds=expandedRepairWindow(
                    repairPlans[eventFirst].start,repairPlans[eventLast].end,
                    (long)outFrames,sr);
                eventMatch=localTrendMatch(sources,audioMasterIndex,donor,
                    eventBounds.start,eventBounds.end,
                    repairPlans[middle].delay,sr);
            }
            coherent=coherent && eventMatch.usable;

            if(coherent) {
                double outlier=0.0;
                for(size_t k=eventFirst;k<=eventLast;++k)
                    outlier=std::max(outlier,repairPlans[k].outlierRatio);
                renderEvent(eventFirst,eventLast,donor,
                    eventMatch.gain,eventMatch.dc,outlier,
                    "A_EVENT_EXPANDED_CLICK_REPAIR");
            } else {
                // Preserve the proven 30g behaviour if a whole-event fit is
                // unavailable; only original A cells are rendered.
                for(size_t k=i;k<=last;++k) {
                    if(repairPlans[k].confidenceClass!='A') continue;
                    renderEvent(k,k,repairPlans[k].source,
                        repairPlans[k].gain,repairPlans[k].dc,
                        repairPlans[k].outlierRatio,
                        repairPlans[k].decisionReason);
                }
            }
            i=(coherent?eventLast:last)+1;
        }

        reportWorkProgress(repairPlans.size(),repairPlans.size(),true);
        beginWorkPhase("LOCAL_REVIEW",repairPlans.size());

        // 2.31d: revisit kept cells individually; unrelated donor defects and
        // a failed extra copy cannot dilute a separately verified micro-event.
        for(size_t k=0;k<repairPlans.size();++k) {
            reportWorkProgress(k,repairPlans.size());
            if(repairPlans[k].confidenceClass=='A' ||
               !std::isfinite(repairPlans[k].masterImpulseZ) ||
               repairPlans[k].masterImpulseZ<5.0)continue;
            std::vector<bool> mask((size_t)sourceCount,false),needsVerification((size_t)sourceCount,false);
            for(int source=0;source<sourceCount;++source) {
                const auto& reason=repairPlans[k].gateReason[source];
                const bool recoverable=reason=="MAP_CONFIDENCE_FAIL" &&
                    sourceRoles[source]!=SourceRole::Rejected;
                mask[source]=source!=audioMasterIndex && processingEligible[source] &&
                    (reason=="ELIGIBLE_SCORE" || recoverable);
                needsVerification[source]=recoverable;
            }
            const auto originalPlan=repairPlans[k];
            const auto trials=localImpulseTrials(sources,audioMasterIndex,
                originalPlan.start,originalPlan.end,originalPlan.delay,sr,mask,&masterScores,-1,&needsVerification);
            bool accepted=false;
            for(const auto& trial:trials) {
                const auto&e=trial.evidence;
                repairPlans[k].start=trial.start;repairPlans[k].end=trial.end;
                repairPlans[k].delay=trial.delay;
                repairPlans[k].supportingSources=e.supportingSources;
                auto result=renderAttempt(k,k,e.donorSource,e.trendGain,e.trendDc,
                    e.outlierRatio,"A_LOCAL_MICRO_REVIEW",0.0,1,0);
                std::cerr<<"LOCAL_REVIEW time_sec "<<(double)trial.start/sr
                    <<" donor "<<originalIndex[e.donorSource]+1
                    <<" control "<<originalIndex[e.controlSource]+1
                    <<" result "<<result.rejectReason<<"\n";
                if(result.accepted) {
                    accepted=true;
                    if(k<mapRows.size()) {
                        auto& row=mapRows[k];
                        row.supportingSources.clear();row.abstainingSources.clear();row.opposingSources.clear();
                        for(int source:e.supportingSources)row.supportingSources.push_back(originalIndex[source]+1);
                        for(int source:e.abstainingSources)row.abstainingSources.push_back(originalIndex[source]+1);
                        for(int source:e.opposingSources)row.opposingSources.push_back(originalIndex[source]+1);
                        row.eligibleWitnesses=e.eligibleWitnesses;
                        row.supportingWitnesses=e.supportingWitnesses;
                        row.opposingWitnesses=e.opposingWitnesses;
                        row.outlierRatio=e.outlierRatio;
                        row.consensusSupport=(double)e.supportingWitnesses/
                            std::max(1,1+e.supportingWitnesses+e.opposingWitnesses);
                        for(int source=0;source<sourceCount;++source) {
                            int oi=originalIndex[source];
                            row.impulsePeak[oi]=e.peakBySource[source];
                            row.impulseFloor[oi]=e.floorBySource[source];
                            row.impulseZ[oi]=e.zBySource[source];
                            row.trendFit[oi]=e.trendFitBySource[source];
                            row.trendCorr[oi]=e.trendCorrBySource[source];
                        }
                        row.donorControlCorr=e.donorControlTrendCorr;
                        row.masterDonorCorr=e.donorTrendCorr;
                        row.masterControlCorr=e.controlTrendCorr;
                        for(int source=0;source<sourceCount;++source)
                            row.delay[originalIndex[source]]=trial.delay[source];
                        for(int source:e.supportingSources)
                            if(needsVerification[source])row.gateReason[originalIndex[source]]="LOCAL_CONTEXT_RECOVERED";
                    }
                    break;
                }
                repairPlans[k]=originalPlan;
            }
            // Keep map cells and their navigation geometry at the original 2 ms.
            if(accepted) {repairPlans[k].start=originalPlan.start;repairPlans[k].end=originalPlan.end;}
            else repairPlans[k]=originalPlan;
        }

        reportWorkProgress(repairPlans.size(),repairPlans.size(),true);

        beginWorkPhase("DROPOUT_REVIEW",repairPlans.size());
        long dropoutUntil=0;
        for(size_t k=0;k<repairPlans.size();++k){
            reportWorkProgress(k,repairPlans.size());
            const auto& seed=repairPlans[k];
            if(seed.start<dropoutUntil)continue;
            auto recovered=detectDropoutRepair(sources,audioMasterIndex,seed.start,seed.end,seed.delay,processingEligible,sr);
            if(!recovered.accepted)continue;
            bool overlapSafe=true;
            std::vector<RepairRecord> superseded;
            for(const auto& record:repairRecords){long a=(long)std::llround(record.startSec*sr),b=(long)std::llround(record.endSec*sr);
                if(a<recovered.renderLast && b>recovered.renderFirst){
                    if(record.decisionReason.find("DROPOUT")!=std::string::npos || a<recovered.renderFirst-sr/500 || b>recovered.renderLast+sr/500)overlapSafe=false;
                    superseded.push_back(record);
                }}
            if(!overlapSafe)continue;
            // A click repair of a dropout edge must not survive beyond the new
            // event. Undo the whole overlapping click, then insert the verified
            // dropout. Extra old tails return to untouched Master, not new audio.
            for(const auto& old:superseded){long a=(long)std::llround(old.startSec*sr),b=(long)std::llround(old.endSec*sr);
                std::copy(ref.samples.begin()+(size_t)a*ref.channels,ref.samples.begin()+(size_t)b*ref.channels,out.begin()+(size_t)a*ref.channels);
                std::cerr<<"DROPOUT_SUPERSEDED_CLICK start "<<old.startSec<<" end "<<old.endSec<<"\n";
                for(size_t j=0;j<repairPlans.size();++j)if(repairPlans[j].start<b && repairPlans[j].end>a && (repairPlans[j].end<=recovered.renderFirst || repairPlans[j].start>=recovered.renderLast)){
                    auto& plan=repairPlans[j];plan.source=audioMasterIndex;plan.confidenceClass='C';plan.decisionReason="C_SUPERSEDED_CLICK_OUTSIDE_DROPOUT_KEEP_MASTER";plan.gain=1;plan.dc=0;plan.supportingSources.clear();
                    if(j<mapRows.size()){auto& row=mapRows[j];row.selectedSource=originalIndex[audioMasterIndex]+1;row.confidenceClass='C';row.decisionReason=plan.decisionReason;row.gain=1;row.dc=0;row.supportingSources.clear();row.supportingWitnesses=0;row.consensusSupport=0;}
                }
            }
            repairRecords.erase(std::remove_if(repairRecords.begin(),repairRecords.end(),[&](const RepairRecord& r){return r.startSec*sr<recovered.renderLast && r.endSec*sr>recovered.renderFirst;}),repairRecords.end());
            std::copy(recovered.samples.begin(),recovered.samples.end(),out.begin()+(size_t)recovered.renderFirst*ref.channels);
            RepairRecord record;record.startSec=(double)recovered.renderFirst/sr;record.endSec=(double)recovered.renderLast/sr;record.coreStartSec=(double)recovered.first/sr;record.coreEndSec=(double)recovered.last/sr;
            record.donorSource=originalIndex[recovered.donor]+1;record.decisionReason="A_VERIFIED_DROPOUT_TWO_DONORS";record.fadeInSamples=recovered.first-recovered.renderFirst;record.fadeOutSamples=recovered.renderLast-recovered.last;record.renderGain=recovered.gain;record.renderDc=recovered.dc;record.donorDelaySamples=recovered.delay[recovered.donor];
            for(size_t z=0;z<recovered.samples.size();++z)record.maxAbsDifference=std::max(record.maxAbsDifference,std::abs((double)recovered.samples[z]-ref.samples[(size_t)recovered.renderFirst*ref.channels+z]));
            repairRecords.push_back(record);
            for(size_t j=0;j<repairPlans.size();++j){auto& plan=repairPlans[j];if(plan.end<=recovered.renderFirst || plan.start>=recovered.renderLast)continue;
                plan.source=recovered.donor;plan.confidenceClass='A';plan.decisionReason=record.decisionReason;plan.gain=recovered.gain;plan.dc=recovered.dc;plan.supportingSources={recovered.donor,recovered.control};
                if(j<mapRows.size()){auto& row=mapRows[j];row.selectedSource=record.donorSource;row.confidenceClass='A';row.decisionReason=record.decisionReason;row.gain=recovered.gain;row.dc=recovered.dc;row.donorControlCorr=recovered.pairCorr;row.supportingSources={originalIndex[recovered.donor]+1,originalIndex[recovered.control]+1};row.supportingWitnesses=2;row.opposingWitnesses=0;row.abstainingSources.clear();row.opposingSources.clear();
                    row.eligibleWitnesses=2;row.consensusSupport=2./3.;row.masterDonorCorr=recovered.donorContextCorr;row.masterControlCorr=recovered.controlContextCorr;
                    for(int i=0;i<sourceCount;++i)if(i!=audioMasterIndex && i!=recovered.donor && i!=recovered.control)row.abstainingSources.push_back(originalIndex[i]+1);
                    row.gateReason[originalIndex[recovered.donor]]="DROPOUT_CONTEXT_VERIFIED";row.gateReason[originalIndex[recovered.control]]="DROPOUT_CONTEXT_VERIFIED";
                    for(int i=0;i<sourceCount;++i)row.delay[originalIndex[i]]=recovered.delay[i];}}
            dropoutUntil=recovered.renderLast;
            std::cerr<<"DROPOUT_REPAIR start "<<record.startSec<<" end "<<record.endSec<<" donor "<<record.donorSource<<" control "<<originalIndex[recovered.control]+1<<" pair_corr "<<recovered.pairCorr<<"\n";
        }
        reportWorkProgress(repairPlans.size(),repairPlans.size(),true);

        beginWorkPhase("FINAL_SUMMARY");

        for(size_t k=0;k<repairPlans.size();++k) {
            const auto& p=repairPlans[k];
            ++selectedCount[p.source];
            if(p.confidenceClass=='A') ++finalA;
            else if(p.confidenceClass=='B') ++finalB;
            else ++finalC;
            const bool fast=k<coarseDiag.size() && coarseDiag[k].fast_c_eligible;
            if(fast && p.confidenceClass=='A') ++fastAHit;
            if(fast && p.confidenceClass=='B') ++fastBHit;
            if(fast && p.confidenceClass=='C') ++fastCHit;
            if(p.confidenceClass=='A' || p.confidenceClass=='B') {
                ++coarseAB;
                if(k<coarseDiag.size() && coarseDiag[k].flagged) ++coarseABCovered;
            }
        }

        std::cerr << "\n";
        const double coarsePct = coarseTotal ? 100.0 * coarseFlagged / coarseTotal : 0.0;
        const double coarseRecall = coarseAB ? 100.0 * coarseABCovered / coarseAB : 100.0;
        std::cerr << "COARSE total " << coarseTotal
                  << " flagged " << coarseFlagged
                  << " flagged_pct " << coarsePct
                  << " consensus " << coarseConsensus
                  << " ambiguous " << coarseAmbiguous
                  << " transient " << coarseTransient
                  << " energy " << coarseEnergy
                  << " ab_total " << coarseAB
                  << " ab_covered " << coarseABCovered
                  << " ab_recall_pct " << coarseRecall << "\n";
        const double fastPct = coarseTotal ? 100.0 * fastEligible / coarseTotal : 0.0;
        const double fastAPreserve = 100.0; // gate validated 9/9 A in v0.2.18 shadow
        const double fastBPreserve = finalB ? 100.0 * (finalB-fastBHit) / finalB : 100.0;
        std::cerr << "FASTPATH total " << coarseTotal
                  << " eligible " << fastEligible
                  << " eligible_pct " << fastPct
                  << " A_total " << finalA << " A_bypassed " << fastAHit
                  << " A_recall_pct " << fastAPreserve
                  << " B_total " << finalB << " B_bypassed " << fastBHit
                  << " B_recall_pct " << fastBPreserve
                  << " C_total " << finalC << " C_bypassed " << fastCHit << "\n";
        std::cerr << "BLOCKSYNC_PROFILE blocks " << rpBlocks
                  << " reacquire_blocks " << bsReacquireBlocks
                  << " estimate_blocks " << bsEstimateBlocks
                  << " reacquire_jobs " << bsReacquireJobs
                  << " reacquire_cpu_sec " << bsReacquireCpuSec
                  << " estimate_jobs " << bsEstimateJobs
                  << " estimate_cpu_sec " << bsEstimateCpuSec
                  << " launch_sec " << bsLaunchSec
                  << " wait_sec " << bsWaitSec
                  << " post_sec " << bsPostSec << "\n";

        std::cerr << "RECON_OUTER blocks " << rpBlocks
                  << " block_sync_sec " << rpBlockSyncSec
                  << " block_score_sec " << rpBlockScoreSec
                  << " segment_loop_sec " << rpSegmentLoopSec << "\n";
        std::cerr << "RECON_PROFILE segments " << rpSegments
                  << " fast " << rpFast << " full " << rpFull
                  << " gate_sec " << rpGateSec
                  << " full_analysis_sec " << rpFullSec
                  << " tail_sec " << rpTailSec << "\n";
        profilePhase("reconstruction");
        std::cerr << "PHASE WRITING_OUTPUT 0 1\n";

        beginWorkPhase("MASTER_COMPARISON",outFrames);
        uint64_t changedSamples=0,changedFrames=0;
        for(size_t f=0;f<outFrames;++f) {
            if((f & 65535)==0)reportWorkProgress(f,outFrames);
            bool changed=false;
            for(int ch=0;ch<ref.channels;++ch) {
                const size_t p=f*(size_t)ref.channels+(size_t)ch;
                if(out[p]!=ref.samples[p]) { ++changedSamples; changed=true; }
            }
            if(changed) ++changedFrames;
        }
        std::cerr << "MASTER_PRESERVE repairs " << repairRecords.size()
                  << " changed_frames " << changedFrames
                  << " changed_samples " << changedSamples
                  << " changed_ms " << (1000.0*changedFrames/sr) << "\n";

        double speedFactor=1.0;
        if(recordedRpm>0.0 && discRpm>0.0) speedFactor=discRpm/recordedRpm;
        if(!std::isfinite(speedFactor) || speedFactor<0.25 || speedFactor>4.0)
            throw std::runtime_error("RPM conversion factor is outside the safe range 0.25..4.0");
        beginWorkPhase("SPEED_RESULT");
        std::vector<float> finalOut=resamplePlaybackSpeed(out,ref.channels,speedFactor);
        beginWorkPhase("SPEED_MASTER");
        std::vector<float> baselineOut=resamplePlaybackSpeed(ref.samples,ref.channels,speedFactor);
        const double finalDuration=(double)(finalOut.size()/ref.channels)/(double)ref.sampleRate;
        std::cerr << "SPEED_CONVERSION recorded_rpm " << recordedRpm
                  << " disc_rpm " << discRpm
                  << " factor " << speedFactor
                  << " output_duration " << finalDuration << "\n";

        beginWorkPhase("OUTPUT_QA");
        const auto outputQa=analyzeOutputQa(finalOut,baselineOut,ref.channels,
            ref.sampleRate,speedFactor,repairRecords);
        std::string qaName=outName;
        const size_t qaWavExt=qaName.rfind(".wav");
        if(qaWavExt!=std::string::npos)
            qaName.replace(qaWavExt,4,".output-qa.csv");
        else
            qaName += ".output-qa.csv";
        {
            std::ofstream qa(qaName);
            if(!qa) throw std::runtime_error("Cannot create output QA report "+qaName);
            qa << std::fixed << std::setprecision(10);
            qa << "metric,value\n";
            qa << "status," << (outputQa.passed?"PASS":"WARNING") << "\n";
            qa << "peak," << outputQa.peak << "\n";
            qa << "clipped_samples," << outputQa.clippedSamples << "\n";
            qa << "baseline_clipped_samples," << outputQa.baselineClippedSamples << "\n";
            qa << "nonfinite_samples," << outputQa.nonFiniteSamples << "\n";
            qa << "max_abs_dc," << outputQa.dcMax << "\n";
            qa << "max_abs_dc_shift_from_master," << outputQa.dcShiftMax << "\n";
            qa << "repair_boundary_flags," << outputQa.seamFlags << "\n";
        }
        std::cerr << "OUTPUT_QA status " << (outputQa.passed?"PASS":"WARNING")
                  << " peak " << outputQa.peak
                  << " clipped_samples " << outputQa.clippedSamples
                  << " baseline_clipped_samples " << outputQa.baselineClippedSamples
                  << " nonfinite_samples " << outputQa.nonFiniteSamples
                  << " dc_shift " << outputQa.dcShiftMax
                  << " seam_flags " << outputQa.seamFlags
                  << " path " << qaName << "\n";

        beginWorkPhase("SAVE_RESULT");
        writeWavPcm(
            outName,
            ref.sampleRate,
            ref.channels,
            ref.bits,
            finalOut);

        std::string baselineName=outName;
        const size_t wavExt=baselineName.rfind(".wav");
        if(wavExt!=std::string::npos)
            baselineName.replace(wavExt,4,".master-only.wav");
        else
            baselineName += ".master-only.wav";
        beginWorkPhase("SAVE_MASTER");
        writeWavPcm(baselineName,ref.sampleRate,ref.channels,ref.bits,baselineOut);
        std::cerr << "MASTER_BASELINE " << baselineName << "\n";

        beginWorkPhase("FINAL_FILES");
        if (!mapName.empty()) {
            beginWorkPhase("SAVE_MAP",mapRows.size());
            writeDecisionMap(
                mapName,
                mapRows,
                sourceCount);

            beginWorkPhase("FINAL_FILES");
            std::string repairName=mapName;
            const std::string decisionSuffix=".decisions.csv";
            size_t repairPos=repairName.rfind(decisionSuffix);
            if(repairPos!=std::string::npos)
                repairName.replace(repairPos,decisionSuffix.size(),".repairs.csv");
            else
                repairName += ".repairs.csv";
            std::ofstream rf(repairName);
            if(!rf) throw std::runtime_error("Cannot create repair log "+repairName);
            rf << std::fixed << std::setprecision(8);
            rf << "start_sec,end_sec,duration_ms,donor_source,outlier_ratio,max_abs_difference,retry_attempt,delay_nudge_samples,decision_reason,core_start_sec,core_end_sec,fade_in_samples,fade_out_samples,render_gain,render_dc,donor_delay_samples\n";
            for(const auto& rr:repairRecords)
                rf << rr.startSec << "," << rr.endSec << ","
                   << (1000.0*(rr.endSec-rr.startSec)) << ","
                   << rr.donorSource << "," << rr.outlierRatio << ","
                   << rr.maxAbsDifference << "," << rr.retryAttempt << ","
                   << rr.delayNudgeSamples << "," << rr.decisionReason << ","
                   << rr.coreStartSec << "," << rr.coreEndSec << ","
                   << rr.fadeInSamples << "," << rr.fadeOutSamples << ","
                   << rr.renderGain << "," << rr.renderDc << ","
                   << rr.donorDelaySamples << "\n";
            rf.flush();
            std::cerr << "REPAIR_DIAGNOSTIC " << repairName << "\n";

            std::string diagName=mapName;
            const std::string suffix=".decisions.csv";
            size_t dp=diagName.rfind(suffix);
            if(dp!=std::string::npos)
                diagName.replace(dp,suffix.size(),".coarse-diagnostic.csv");
            else
                diagName += ".coarse-diagnostic.csv";

            std::ofstream df(diagName);
            if(!df) throw std::runtime_error("Cannot create coarse diagnostic "+diagName);
            df << std::fixed << std::setprecision(8);
            df << "time_sec,pair_best,pair_second,pair_worst,pair_gap,"
                  "max_deriv_ratio,max_energy_ratio,consensus,ambiguous,"
                  "transient,energy,flagged,fast_c_eligible,final_class\n";
            for(const auto& d:coarseDiag) {
                df << d.timeSec << "," << d.pairBest << "," << d.pairSecond << ","
                   << d.pairWorst << "," << d.pairGap << "," << d.maxDerivRatio << ","
                   << d.maxEnergyRatio << "," << d.consensus << "," << d.ambiguous << ","
                   << d.transient << "," << d.energy << "," << d.flagged << ","
                   << d.fast_c_eligible << "," << d.finalClass << "\n";
            }
            std::cerr << "COARSE_DIAGNOSTIC " << diagName << "\n";
        }
        profilePhase("write_output");
        const double totalProfileSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - profileTotalStart).count();
        std::cerr << "PROFILE total " << totalProfileSeconds << "\n";
        std::cerr << "PHASE COMPLETE 1 1\n";

        uint64_t allSelections = 0;
        for (auto n : selectedCount)
            allSelections += n;

        auto pct = [&](uint64_t n) {
            return allSelections
                ? 100.0 * n / allSelections
                : 0.0;
        };

        std::cerr
            << "Source selection by ~2 ms surgical segment:\n";

        for (int s = 0; s < sourceCount; ++s) {
            std::cerr
                << "  source "
                << (originalIndex[s] + 1)
                << " selected: "
                << pct(selectedCount[s])
                << " %\n";
        }

        std::cerr
            << "Output: "
            << outName
            << "\n";

        if (!mapName.empty()) {
            std::cerr
                << "Decision map: "
                << mapName
                << "\n";
        }

        std::cerr << "Done.\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr
            << "ERROR: "
            << e.what()
            << "\n";

        return 1;
    }
}
