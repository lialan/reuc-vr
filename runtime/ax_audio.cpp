#include "ax_audio.h"
#include "audio_gap_audit.h"
#include "gx_vertices.h"
#include "guest_call.h"
#include "clock.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
#include <cstdio>
namespace {
uc::Sched* scheduler;
uc::AudioSink output;
bool started=false;
bool replaying=false;
bool profile_audio=false;
struct VoiceProbe { bool running=false,tracked=false; int32_t peak=0; };
std::array<VoiceProbe,128> voice_probes{};
uint32_t voice_probe_starts=0;
std::atomic<uint64_t> frames{0},nonzero{0};
gx::Memory ram(const ppc::Ctx& c) { return {{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}}; }
// A guest RAM range as ram(c).read() accepts it (MEM1 24 MB, MEM2 64 MB, through any of their aliases); anything else goes through read(),
// which throws the same error as before.
inline bool in_ram(uint32_t address,uint32_t size) {
    const uint32_t alias=address>>28;
    if(!((0x7303u>>alias)&1)) return false;   // aliases 0, 1, 8, 9, 12, 13, 14
    const uint32_t physical=address&0x3FFFFFFF;
    if(physical<0x10000000) return physical<=(24u<<20) && size<=(24u<<20)-physical;
    return physical<0x20000000 && physical-0x10000000<=(64u<<20) && size<=(64u<<20)-(physical-0x10000000);
}
inline void check_ram(const ppc::Ctx& c,uint32_t address,uint32_t size) { if(!in_ram(address,size)) [[unlikely]] ram(c).read(address,size); }
// The parameter block fields the sample decoder reads and writes, held in locals for the 96 samples of a frame and written back once (also
// when it throws). Same values, and the same fields written, as the per-sample reads and writes of guest memory did before: nothing else
// touches the block during the frame and a voice's sample data never lies inside its own block. tests/test_ax_audio.cpp compares the two.
struct Voice {
    uint32_t pb;
    uint16_t state,stream,format,loop_flag,pred,loop_pred,loop_yn1,loop_yn2;
    int16_t yn1,yn2,coef[16];
    uint32_t pos,end,loop;
    bool wrote_state=false,wrote_pred=false,wrote_history=false,wrote_pos=false;
    Voice(ppc::Ctx& c,uint32_t at) : pb(at) {
        state=uint16_t(ppc::rd16(c,pb+0x10)); stream=uint16_t(ppc::rd16(c,pb+0x12)); format=uint16_t(ppc::rd16(c,pb+0x70));
        loop_flag=uint16_t(ppc::rd16(c,pb+0x6E)); pred=uint16_t(ppc::rd16(c,pb+0xA0)); loop_pred=uint16_t(ppc::rd16(c,pb+0xB4));
        loop_yn1=uint16_t(ppc::rd16(c,pb+0xB6)); loop_yn2=uint16_t(ppc::rd16(c,pb+0xB8));
        yn1=int16_t(ppc::rd16(c,pb+0xA2)); yn2=int16_t(ppc::rd16(c,pb+0xA4));
        for(uint32_t i=0;i<16;++i) coef[i]=int16_t(ppc::rd16(c,pb+0x7E +i*2));   // eight predictor pairs at 0x7E + 4n, 0x80 + 4n
        pos=ppc::rd32(c,pb+0x7A); end=ppc::rd32(c,pb+0x76); loop=ppc::rd32(c,pb+0x72);
    }
    void store(ppc::Ctx& c) const {
        if(wrote_pred) ppc::wr16(c,pb+0xA0,pred);
        if(wrote_history) { ppc::wr16(c,pb+0xA4,uint16_t(yn2)); ppc::wr16(c,pb+0xA2,uint16_t(yn1)); }
        if(wrote_state) ppc::wr16(c,pb+0x10,state);
        if(wrote_pos) ppc::wr32(c,pb+0x7A,pos);
    }
    int16_t next_sample(ppc::Ctx& c) {
        if(state!=1) return 0;
        uint32_t at=pos;   // the block's position changes only when the sample is complete
        int32_t value;
        if (format==10) {
            if (at>UINT32_MAX/2) throw std::invalid_argument("AX PCM16 address overflows");
            check_ram(c,at*2,2); value=int16_t(ppc::rd16(c,at*2));
        } else if (format==25) {
            check_ram(c,at,1); value=int8_t(ppc::rd8(c,at))*256;
        } else {
            if ((at&15)<2) at=(at&~15u)+2;
            check_ram(c,at/2,1);
            if ((at&15)==2) { check_ram(c,(at&~15u)/2,1); pred=uint16_t(ppc::rd8(c,(at&~15u)/2)); wrote_pred=true; }
            if (pred>255 || (pred>>4)>7) throw std::invalid_argument("invalid AX ADPCM predictor");
            const uint8_t byte=uint8_t(ppc::rd8(c,at/2));
            int32_t nibble=(at&1) ? byte&15 : byte>>4;
            if (nibble>=8) nibble-=16;
            const int64_t prediction=int64_t(coef[(pred>>4)*2])*yn1+int64_t(coef[(pred>>4)*2+1])*yn2;
            value=int32_t(std::clamp<int64_t>((int64_t(nibble)*(1<<(pred&15))*2048+prediction+1024)>>11,-32768,32767));
            yn2=yn1; yn1=int16_t(value); wrote_history=true;
        }
        // End is a playback boundary, not an upper RAM bound; streaming may jump beyond it.
        if(at==end) {
            at=loop;
            if(!loop_flag) { state=0; wrote_state=true; }
            else {
                pred=loop_pred; wrote_pred=true;
                if(stream!=1) { yn1=int16_t(loop_yn1); yn2=int16_t(loop_yn2); wrote_history=true; }
            }
        } else ++at;
        pos=at; wrote_pos=true;
        return int16_t(value);
    }
};
void arm(uint64_t at) {
    scheduler->post_at(at,[at] {
        if (!started) return;
        hle_AXFrame(scheduler->irq_ctx());
        // Every interval produces96 samples; skipping overdue intervals starves the device.
        arm(at+uc::ms_to_ticks(3));
    },uc::Sched::EventKind::Audio);
}
}
namespace uc {
void mix_ax_voice(ppc::Ctx& c,uint32_t pb,std::span<int32_t,192> mix) {
    ram(c).read(pb,320);
    if (ppc::rd16(c,pb+0x10)!=1) return;
    const auto format=ppc::rd16(c,pb+0x70);
    if (format!=0 && format!=10 && format!=25) throw std::invalid_argument("unsupported native AX sample format");
    const auto ratio=ppc::rd32(c,pb+0xA6);
    if (ratio>4*65536) throw std::invalid_argument("native AX sample rate exceeds4x");
    // shortcut: nearest native resampling;4-tap filtering, AUX/reverb and surround fidelity remain TD-042.
    uint32_t phase=ppc::rd16(c,pb+0xAA);
    int32_t sample=int16_t(ppc::rd16(c,pb+0xB2));
    int32_t envelope=ppc::rd16(c,pb+0x6A);
    int32_t left=ppc::rd16(c,pb+0x14),right=ppc::rd16(c,pb+0x18);
    const auto control=ppc::rd32(c,pb+0x0C);
    const int32_t envelope_step=int16_t(ppc::rd16(c,pb+0x6C)),left_step=int16_t(ppc::rd16(c,pb+0x16)),right_step=int16_t(ppc::rd16(c,pb+0x1A));
    Voice voice(c,pb);
    try {
        for (size_t i=0;i<96;++i) {
            if (voice.state!=1) break;
            phase+=ratio;
            while (phase>=65536) { sample=voice.next_sample(c); phase-=65536; }
            const auto scaled=int64_t(sample)*envelope/32768;
            if (control&5) mix[i*2]+=int32_t(scaled*left/32768);
            if (control&6) mix[i*2+1]+=int32_t(scaled*right/32768);
            envelope=std::clamp(envelope+envelope_step,0,65535);
            if (control&4) left=std::clamp(left+left_step,0,65535);
            if (control&4) right=std::clamp(right+right_step,0,65535);
        }
    } catch(...) { voice.store(c); throw; }
    voice.store(c);
    ppc::wr16(c,pb+0xAA,uint16_t(phase)); ppc::wr16(c,pb+0xB2,uint16_t(sample));
    ppc::wr16(c,pb+0x6A,uint16_t(envelope)); ppc::wr16(c,pb+0x14,uint16_t(left)); ppc::wr16(c,pb+0x18,uint16_t(right));
}
void audio_hle_init(Sched& s,AudioSink sink,bool profile) {
    scheduler=&s; output=std::move(sink); started=false; replaying=false; frames=nonzero=0;
    profile_audio=profile; voice_probes={}; voice_probe_starts=0;
}
void audio_hle_shutdown() { started=false; output={}; scheduler=nullptr; }
AudioStats audio_stats() { return {frames.load(),nonzero.load()}; }
struct AudioSnapshot {
    bool started;
    std::array<VoiceProbe,128> probes;
    uint32_t starts;
    AudioStats stats;
};
std::shared_ptr<const AudioSnapshot> audio_checkpoint() {
    return std::make_shared<AudioSnapshot>(AudioSnapshot{started,voice_probes,voice_probe_starts,audio_stats()});
}
void audio_restore(const AudioSnapshot& saved) {
    started=saved.started; voice_probes=saved.probes; voice_probe_starts=saved.starts;
    frames=saved.stats.frames; nonzero=saved.stats.nonzero_samples;
}
void audio_set_replaying(bool on) { replaying=on; }
}
void hle_AIStartDMA(ppc::Ctx&) {
    if (scheduler && !started) { started=true; arm(uc::now_ticks()+uc::ms_to_ticks(3)); }
}
void hle_AXFrame(ppc::Ctx& c) {
    // Verified RBUE08 AX service/frame callback and320-byte PBs, without DSP command/mail execution.
    ram(c).read(c.r[13]-25636,4);
    const auto count=ppc::rd32(c,c.r[13]-25524),base=ppc::rd32(c,c.r[13]-25536);
    if (count>128) throw std::invalid_argument("invalid AX voice count");
    if (count) {
        ram(c).read(base,size_t(count)*320);
        uc::call_guest(c,0x80275F34,{0x5F50});
        uc::call_guest(c,0x80273688,{});
    }
    const auto callback=ppc::rd32(c,c.r[13]-25636);
    if (callback) uc::call_guest(c,callback,{});
    std::array<int32_t,192> mix{};
    const bool audit=uc::audio_gap_audit_enabled();
    for (uint32_t i=0;i<count;++i) {
        const uint32_t pb=base+i*320;
        std::array<int32_t,192> before;
        auto& probe=voice_probes[i];
        if(profile_audio) {
            const bool running=ppc::rd16(c,pb+0x10)==1;
            if(!running && probe.running && probe.tracked) std::fprintf(stderr,"[ax-sfx-end] frame %llu index %u pb %08X whole-voice peak %d\n",(unsigned long long)frames.load(),i,pb,probe.peak);
            if(!running) probe={};
            if(running && !probe.running) {
                probe.tracked=voice_probe_starts<64 && ppc::rd16(c,pb+0x12)==0;
                probe.peak=0;
                if(probe.tracked) {
                    ++voice_probe_starts;
                    std::fprintf(stderr,"[ax-voice] frame %llu index %u pb %08X format %u stream %u control %08X envelope %u LR %u:%u pos %08X end %08X\n",(unsigned long long)frames.load(),i,pb,ppc::rd16(c,pb+0x70),ppc::rd16(c,pb+0x12),ppc::rd32(c,pb+0x0C),ppc::rd16(c,pb+0x6A),ppc::rd16(c,pb+0x14),ppc::rd16(c,pb+0x18),ppc::rd32(c,pb+0x7A),ppc::rd32(c,pb+0x76));
                }
            }
            probe.running=running;
            if(probe.tracked) before=mix;
        }
        uc::AudioVoiceKey audit_key;
        if(audit) { audit_key=uc::audio_gap_voice_key(c,pb); before=mix; }
        uc::mix_ax_voice(c,pb,mix);
        if(audit) uc::audio_gap_audit_voice(audit_key,before,mix);
        if(profile_audio && probe.tracked) {
            for(size_t sample=0;sample<mix.size();++sample) probe.peak=std::max(probe.peak,int32_t(std::abs(mix[sample]-before[sample])));
            if(ppc::rd16(c,pb+0x10)!=1) {
                std::fprintf(stderr,"[ax-sfx-end] frame %llu index %u pb %08X whole-voice peak %d\n",(unsigned long long)frames.load(),i,pb,probe.peak);
                probe={};
            }
        }
    }
    std::array<int16_t,192> pcm{};
    uint64_t active=0;
    for (size_t i=0;i<pcm.size();++i) { pcm[i]=int16_t(std::clamp(mix[i],-32768,32767)); active+=pcm[i]!=0; }
    ++frames; nonzero+=active;
    if (output && !replaying) output(pcm);
}
