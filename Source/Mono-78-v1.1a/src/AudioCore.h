#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
struct Wav {
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;
    std::vector<float> samples;

    size_t frames() const {
        return channels ? samples.size() / (size_t)channels : 0;
    }
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
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
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
            if (!f || size < 16)
                throw std::runtime_error(path + ": bad fmt chunk");

            audioFormat = rd16(fmt.data());
            channels = rd16(fmt.data() + 2);
            sampleRate = rd32(fmt.data() + 4);
            blockAlign = rd16(fmt.data() + 12);
            bits = rd16(fmt.data() + 14);

        } else if (id == "data") {
            data.resize(size);
            if (size) {
                f.read((char*)data.data(), size);
                if(!f) throw std::runtime_error("Truncated WAV data");
            }

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
    if (!sampleRate || sampleRate>768000 || blockAlign != channels * bytes || data.empty() || data.size()%blockAlign)
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
static MonoFusionStats fuseStereoGrooveToMono(Wav& w, bool repair=false) {
    MonoFusionStats st;
    if (w.channels != 2) return st;
    if(w.frames()<1024) throw std::runtime_error("File too short for channel matching; choose simple mono.");

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
        mono[i]=(float)ma;
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
        if(!repair || keep<0) continue;
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
        mono[i]=(float)((1.0-a)*(double)mono[i]+a*clean);
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

    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot create " + path);

    const int bytes = bits / 8;
    if(x.size() > (UINT32_MAX-37ull)/bytes) throw std::runtime_error("Output exceeds WAV 4 GB limit");
    uint32_t dataBytes = (uint32_t)(x.size() * bytes);

    f.write("RIFF", 4);
    wr32(f, 36 + dataBytes + (dataBytes & 1));
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

    for (float vf : x) {
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
    if(dataBytes & 1) f.put(0);
    f.flush();
    if(!f) throw std::runtime_error("Cannot finish WAV: check free disk space");
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
            output[of*(size_t)channels+(size_t)ch]=(float)sum;
        }
    }
    return output;
}

