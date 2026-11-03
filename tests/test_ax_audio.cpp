#include "check.h"
#include "ax_audio.h"
#include "ppc_fixture.h"
#include <array>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#include <random>
#include <stdexcept>
#include <vector>
// The mixer as it was before the parameter block moved into locals (every field read and written in guest memory per sample): the reference
// the fast version must match bit for bit, guest memory and mix alike.
namespace reference {
int16_t next_sample(ppc::Ctx& c,uint32_t pb) {
    if(ppc::rd16(c,pb+0x10)!=1) return 0;
    uint32_t pos=ppc::rd32(c,pb+0x7A);
    const uint32_t end=ppc::rd32(c,pb+0x76);
    const uint16_t format=ppc::rd16(c,pb+0x70);
    int32_t value;
    if (format==10) {
        if (pos>UINT32_MAX/2) throw std::invalid_argument("AX PCM16 address overflows");
        value=int16_t(ppc::rd16(c,pos*2));
    } else if (format==25) {
        value=int8_t(ppc::rd8(c,pos))*256;
    } else {
        if ((pos&15)<2) pos=(pos&~15u)+2;
        if ((pos&15)==2) ppc::wr16(c,pb+0xA0,ppc::rd8(c,(pos&~15u)/2));
        const auto pred=ppc::rd16(c,pb+0xA0);
        if (pred>255 || (pred>>4)>7) throw std::invalid_argument("invalid AX ADPCM predictor");
        const uint8_t byte=uint8_t(ppc::rd8(c,pos/2));
        int32_t nibble=(pos&1) ? byte&15 : byte>>4;
        if (nibble>=8) nibble-=16;
        const int32_t first=int16_t(ppc::rd16(c,pb+0x7E +(pred>>4)*4));
        const int32_t second=int16_t(ppc::rd16(c,pb+0x80+(pred>>4)*4));
        const int64_t prediction=int64_t(first)*int16_t(ppc::rd16(c,pb+0xA2))+int64_t(second)*int16_t(ppc::rd16(c,pb+0xA4));
        value=int32_t(std::clamp<int64_t>((int64_t(nibble)*(1<<(pred&15))*2048+prediction+1024)>>11,-32768,32767));
        ppc::wr16(c,pb+0xA4,ppc::rd16(c,pb+0xA2)); ppc::wr16(c,pb+0xA2,uint16_t(value));
    }
    if(pos==end) {
        pos=ppc::rd32(c,pb+0x72);
        if(!ppc::rd16(c,pb+0x6E)) ppc::wr16(c,pb+0x10,0);
        else {
            ppc::wr16(c,pb+0xA0,ppc::rd16(c,pb+0xB4));
            if(ppc::rd16(c,pb+0x12)!=1) { ppc::wr16(c,pb+0xA2,ppc::rd16(c,pb+0xB6)); ppc::wr16(c,pb+0xA4,ppc::rd16(c,pb+0xB8)); }
        }
    } else ++pos;
    ppc::wr32(c,pb+0x7A,pos);
    return int16_t(value);
}
void mix_ax_voice(ppc::Ctx& c,uint32_t pb,std::span<int32_t,192> mix) {
    if (ppc::rd16(c,pb+0x10)!=1) return;
    const auto ratio=ppc::rd32(c,pb+0xA6);
    uint32_t phase=ppc::rd16(c,pb+0xAA);
    int32_t sample=int16_t(ppc::rd16(c,pb+0xB2));
    int32_t envelope=ppc::rd16(c,pb+0x6A);
    int32_t left=ppc::rd16(c,pb+0x14),right=ppc::rd16(c,pb+0x18);
    const auto control=ppc::rd32(c,pb+0x0C);
    for (size_t i=0;i<96;++i) {
        if (ppc::rd16(c,pb+0x10)!=1) break;
        phase+=ratio;
        while (phase>=65536) { sample=next_sample(c,pb); phase-=65536; }
        const auto scaled=int64_t(sample)*envelope/32768;
        if (control&5) mix[i*2]+=int32_t(scaled*left/32768);
        if (control&6) mix[i*2+1]+=int32_t(scaled*right/32768);
        envelope=std::clamp(envelope+int16_t(ppc::rd16(c,pb+0x6C)),0,65535);
        if (control&4) left=std::clamp(left+int16_t(ppc::rd16(c,pb+0x16)),0,65535);
        if (control&4) right=std::clamp(right+int16_t(ppc::rd16(c,pb+0x1A)),0,65535);
    }
    ppc::wr16(c,pb+0xAA,uint16_t(phase)); ppc::wr16(c,pb+0xB2,uint16_t(sample));
    ppc::wr16(c,pb+0x6A,uint16_t(envelope)); ppc::wr16(c,pb+0x14,uint16_t(left)); ppc::wr16(c,pb+0x18,uint16_t(right));
}
}
// Random voices (every format, loops, streams, predictor switches, ramps, rates up to 4x, a few invalid predictors): the fast mixer and the
// reference leave the same parameter block, the same mix and the same exception, frame after frame.
void compare_with_reference(ppc::Ctx& c) {
    constexpr uint32_t pb=0x80003000,data=0x80004000,size=0x800;
    std::mt19937 random(1234);
    const auto bits=[&](uint32_t n) { return uint32_t(random()) & ((1u<<n)-1); };
    for (int trial=0;trial<3000;++trial) {
        const uint16_t format=std::array<uint16_t,3>{0,10,25}[bits(2)%3];
        std::vector<uint8_t> bytes(size);
        for (auto& b:bytes) b=uint8_t(random());
        if (format==0) for (uint32_t i=0;i<size;i+=8) if (bits(6)) bytes[i]&=0x7F;   // ADPCM frame headers: mostly valid predictors
        std::memcpy(c.mem+(data&0x3FFFFFFF),bytes.data(),size);
        std::memset(c.mem+(pb&0x3FFFFFFF),0,320);
        const uint32_t unit=format==10 ? (data&0x3FFFFFFF)/2 : format==25 ? (data&0x3FFFFFFF) : (data&0x3FFFFFFF)*2;   // sample addresses
        const uint32_t units=format==10 ? size/2 : format==25 ? size : size*2;
        const auto at=[&] { return unit+bits(16)%(units-64); };
        ppc::wr16(c,pb+0x10,1); ppc::wr16(c,pb+0x12,uint16_t(bits(1))); ppc::wr32(c,pb+0x0C,bits(3));
        ppc::wr16(c,pb+0x14,uint16_t(random())); ppc::wr16(c,pb+0x16,uint16_t(int16_t(bits(9))-256));
        ppc::wr16(c,pb+0x18,uint16_t(random())); ppc::wr16(c,pb+0x1A,uint16_t(int16_t(bits(9))-256));
        ppc::wr16(c,pb+0x6A,uint16_t(random())); ppc::wr16(c,pb+0x6C,uint16_t(int16_t(bits(10))-512));
        ppc::wr16(c,pb+0x6E,uint16_t(bits(1))); ppc::wr16(c,pb+0x70,format);
        const uint32_t start=at(); ppc::wr32(c,pb+0x7A,start); ppc::wr32(c,pb+0x76,start+bits(6)); ppc::wr32(c,pb+0x72,at());
        for (uint32_t i=0;i<16;++i) ppc::wr16(c,pb+0x7E +i*2,uint16_t(random()));
        ppc::wr16(c,pb+0xA0,uint16_t(bits(7))); ppc::wr16(c,pb+0xA2,uint16_t(random())); ppc::wr16(c,pb+0xA4,uint16_t(random()));
        ppc::wr32(c,pb+0xA6,bits(4)==0 ? 65536 : bits(18)); ppc::wr16(c,pb+0xAA,uint16_t(random())); ppc::wr16(c,pb+0xB2,uint16_t(random()));
        ppc::wr16(c,pb+0xB4,uint16_t(bits(4)==0 ? 0x80+bits(7) : bits(7))); ppc::wr16(c,pb+0xB6,uint16_t(random())); ppc::wr16(c,pb+0xB8,uint16_t(random()));
        std::vector<uint8_t> start_block(c.mem+(pb&0x3FFFFFFF),c.mem+(pb&0x3FFFFFFF)+320);
        std::array<std::vector<uint8_t>,2> blocks; std::array<std::array<int32_t,192>,2> mixes{}; std::array<int,2> thrown{};
        for (int which=0;which<2;++which) {
            std::memcpy(c.mem+(pb&0x3FFFFFFF),start_block.data(),320);
            std::memcpy(c.mem+(data&0x3FFFFFFF),bytes.data(),size);
            for (int frame=0;frame<4 && !thrown[which];++frame) {
                try { which ? uc::mix_ax_voice(c,pb,mixes[which]) : reference::mix_ax_voice(c,pb,mixes[which]); }
                catch (const std::invalid_argument&) { thrown[which]=frame+1; }
            }
            blocks[which].assign(c.mem+(pb&0x3FFFFFFF),c.mem+(pb&0x3FFFFFFF)+320);
            CHECK(std::equal(bytes.begin(),bytes.end(),c.mem+(data&0x3FFFFFFF)));
        }
        CHECK(thrown[0]==thrown[1]);
        CHECK(blocks[0]==blocks[1]);
        CHECK(mixes[0]==mixes[1]);
    }
}
int main() {
    Fix f; auto& c=f.c; constexpr uint32_t pb=0x80001000;
    compare_with_reference(c);
    auto setup=[&](uint16_t format,uint32_t start,uint32_t end) {
        std::memset(c.mem+0x1000,0,320);
        ppc::wr16(c,pb+0x10,1); ppc::wr32(c,pb+0x0C,5);
        ppc::wr16(c,pb+0x14,0x8000); ppc::wr16(c,pb+0x18,0x4000);
        ppc::wr16(c,pb+0x6A,0x8000); ppc::wr16(c,pb+0x70,format);
        ppc::wr32(c,pb+0x7A,start); ppc::wr32(c,pb+0x76,end);
        ppc::wr32(c,pb+0xA6,0x10000);
    };
    std::array<int32_t,192> mix{};
    ppc::wr16(c,0x80002000,1000); ppc::wr16(c,0x80002002,uint16_t(-1000));
    setup(10,0x1000,0x1001); uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==1000 && mix[1]==500 && mix[2]==-1000 && mix[3]==-500);
    CHECK(mix[4]==0 && ppc::rd16(c,pb+0x10)==0);
    mix={}; setup(10,0x1000,0x1001); ppc::wr32(c,pb+0x0C,2);
    uc::mix_ax_voice(c,pb,mix); CHECK(mix[0]==0 && mix[1]==500);
    mix={}; setup(10,0x1000,0x1001); ppc::wr16(c,pb+0x6E,1); ppc::wr32(c,pb+0x72,0x1000);
    uc::mix_ax_voice(c,pb,mix); CHECK(mix[190]==-1000 && ppc::rd16(c,pb+0x10)==1);
    // Streaming switches to a later buffer; loop address need not precede the old end.
    for(uint32_t i=0;i<96;++i) ppc::wr16(c,0x80002200+i*2,uint16_t(2000+i));
    mix={}; setup(10,0x1000,0x1001); ppc::wr16(c,pb+0x6E,1); ppc::wr32(c,pb+0x72,0x1100);
    uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==1000 && mix[2]==-1000 && mix[4]==2000 && mix[6]==2001);
    CHECK(ppc::rd32(c,pb+0x7A)==0x115E && ppc::rd16(c,pb+0x10)==1);
    mix={}; setup(10,0x1100,0x1001);
    uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==2000 && mix[190]==2095 && ppc::rd16(c,pb+0x10)==1);
    mix={}; setup(10,0x1000,0x1001); ppc::wr16(c,pb+0x6E,1); ppc::wr32(c,pb+0x72,0x66000000);
    bool bad_loop=false;
    try { uc::mix_ax_voice(c,pb,mix); } catch(const std::exception&) { bad_loop=true; }
    CHECK(bad_loop);
    mix={}; ppc::wr8(c,0x80002000,0); ppc::wr8(c,0x80002001,0x1F);
    setup(0,0x4002,0x4003); uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==1 && mix[2]==-1 && ppc::rd16(c,pb+0x10)==0);
    mix={};
    for (uint32_t i=0;i<16;++i) ppc::wr8(c,0x80002000+i,0x11);
    ppc::wr8(c,0x80002000,0); ppc::wr8(c,0x80002008,1);
    setup(0,0x4002,0x401F); uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==1 && mix[26]==1 && mix[28]==2 && mix[54]==2 && mix[56]==0);
    CHECK(ppc::rd16(c,pb+0x10)==0);
    mix={}; ppc::wr8(c,0x80002000,0x80); setup(25,0x2000,0x2000); uc::mix_ax_voice(c,pb,mix);
    CHECK(mix[0]==-32768 && mix[1]==-16384);
    setup(123,0x2000,0x2000); bool rejected=false;
    try { uc::mix_ax_voice(c,pb,mix); } catch (const std::invalid_argument&) { rejected=true; }
    CHECK(rejected);

    uc::Sched sched(c.mem);
    uint64_t blocks=0;
    uc::audio_hle_init(sched,[&](std::span<const int16_t> pcm) { CHECK(pcm.size()==192); ++blocks; });
    auto* audio_thread=sched.create(0,16,[&](ppc::Ctx& active) {
        active.r[13]=0x80400000;
        ppc::wr32(active,active.r[13]-25524,0);
        ppc::wr32(active,active.r[13]-25636,0);
        hle_AIStartDMA(active);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        sched.safe_point();
        // Each due 3ms interval owns 96 PCM samples, even after a delayed safe point.
        CHECK(blocks>=10);
        CHECK(uc::audio_stats().frames==blocks);
        uc::audio_hle_shutdown();
    },&c);
    sched.run(audio_thread);
}
