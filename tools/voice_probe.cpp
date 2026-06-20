// Voice chain probe: DDC a C-channel -> OQPSK 8400 -> AeroL voice frames ->
// AmbeDecoder -> PCM. Reports lock/ebno, voice frame count, PCM RMS.
#include "dsp/ddc.h"
#include "jaero_demod.h"
#include "voice/ambe_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" { double oqpsk_lockingbw = 0.0; }
static uint32_t rd32(const unsigned char* p){return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24);}
static uint16_t rd16(const unsigned char* p){return (uint16_t)(p[0]|(p[1]<<8));}

struct VCtx { AmbeDecoder dec; uint64_t frames=0; double pwr=0; uint64_t samps=0; };

static uint64_t g_dec = 0;
static void decCb(const uint8_t*, int, int, void*) { g_dec++; }

static void voiceCb(const uint8_t* frame, int len, int, void* user)
{
    if (len != 12) return;
    VCtx* v = (VCtx*)user;
    int16_t pcm[160];
    v->dec.decode(frame, pcm);
    for (int i = 0; i < 160; ++i) { v->pwr += (double)pcm[i] * pcm[i]; }
    v->samps += 160;
    v->frames++;
}

int main(int argc, char** argv)
{
    const char* path = argv[1];
    double offHz = (argc > 2) ? atof(argv[2]) : 0.0;
    double seconds = (argc > 3) ? atof(argv[3]) : 20.0;

    std::ifstream f(path, std::ios::binary);
    if(!f){ std::printf("cannot open %s\n", path); return 1; }
    unsigned char hdr[12]; f.read((char*)hdr,12);
    int ch=2,bits=8; double Fs=2048000; uint64_t dataOff=0;
    while(f){unsigned char c[8];f.read((char*)c,8);if(!f)break;uint32_t sz=rd32(c+4);
        if(!std::memcmp(c,"fmt ",4)){std::vector<unsigned char>fm(sz);f.read((char*)fm.data(),sz);
            ch=rd16(fm.data()+2);Fs=rd32(fm.data()+4);bits=rd16(fm.data()+14);if(sz&1)f.seekg(1,std::ios::cur);}
        else if(!std::memcmp(c,"data",4)){dataOff=(uint64_t)f.tellg();break;}
        else f.seekg(sz+(sz&1),std::ios::cur);}
    int fb=ch*(bits/8);
    uint64_t nF=(uint64_t)(seconds*Fs);
    std::vector<unsigned char> raw((size_t)nF*fb);
    f.seekg((std::streamoff)dataOff,std::ios::beg);
    f.read((char*)raw.data(),(std::streamsize)raw.size());
    uint64_t got=(uint64_t)(f.gcount()/fb);
    std::vector<float> iq((size_t)got*2);
    for(uint64_t i=0;i<got;++i){const unsigned char* p=raw.data()+i*fb;
        iq[i*2]=((int)p[0]-128)*(1.f/128.f); iq[i*2+1]=(ch==2)?((int)p[1]-128)*(1.f/128.f):0.f;}
    std::printf("file %.0f Hz, %llu frames, offset %.0f Hz, 8400 OQPSK\n", Fs,(unsigned long long)got,offHz);

    Ddc ddc(Fs, offHz, 48000.0, 14000.0);
    VCtx vc;
    jaero_oqpsk_cont_demod_t* d = jaero_oqpsk_cont_create(ddc.outputRate(), 8400.0, 0, nullptr, nullptr);
    jaero_oqpsk_cont_set_voice_callback(d, voiceCb, &vc);
    jaero_oqpsk_cont_set_decoded_callback(d, decCb, nullptr);

    const int B=32768; std::vector<double> od;
    for(uint64_t off=0;off+B<=got;off+=B){od.clear();ddc.process(&iq[off*2],B,od);if(!od.empty())jaero_oqpsk_cont_feed_iq(d,od.data(),(int)(od.size()/2));}

    double rms = vc.samps ? std::sqrt(vc.pwr/vc.samps) : 0.0;
    std::printf("RESULT: lock=%d ebno=%.1f mse=%.3f  decoded=%llu  voice_frames=%llu  pcm_rms=%.0f (%.1f dB)\n",
        jaero_oqpsk_cont_is_locked(d), jaero_oqpsk_cont_get_ebno(d), jaero_oqpsk_cont_get_mse(d),
        (unsigned long long)g_dec, (unsigned long long)vc.frames, rms, 20.0*std::log10(rms/32768.0+1e-9));
    jaero_oqpsk_cont_destroy(d);
    return 0;
}
