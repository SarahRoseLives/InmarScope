#include "voice/wav_writer.h"
#include <ogg/ogg.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#define REQUIRE(x) do { if (!(x)) { std::cerr << "FAILED line " << __LINE__ << ": " #x << '\n'; return 1; } } while (0)
static uint32_t le32(const std::vector<char>& b, size_t n) {
    return static_cast<unsigned char>(b[n]) | (static_cast<unsigned char>(b[n+1])<<8) |
           (static_cast<unsigned char>(b[n+2])<<16) | (static_cast<unsigned char>(b[n+3])<<24);
}
static std::vector<char> read(const std::filesystem::path& p) {
    std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
int main() {
    const auto dir=std::filesystem::temp_directory_path()/
        ("inmarscope-voice-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    std::vector<int16_t> pcm(8000);
    for(size_t i=0;i<pcm.size();++i)pcm[i]=static_cast<int16_t>(6000*std::sin(i*2*3.141592653589793*440/8000));
    WavWriter writer;
    for(auto format:{RecordFormat::WAV,RecordFormat::OGG}) {
        const auto file=dir/(format==RecordFormat::WAV?"call.wav":"call.ogg");
        writer.setFormat(format);REQUIRE(writer.open(file.string(),8000,1));
        // Same 160-sample frame size as the voice decoder's PCM output.
        for(size_t i=0;i<pcm.size();i+=160)writer.write(pcm.data()+i,160);
        writer.close();writer.close();REQUIRE(!writer.isOpen());
        const auto bytes=read(file);REQUIRE(bytes.size()>44);
        if(format==RecordFormat::WAV) {
            REQUIRE(std::memcmp(bytes.data(),"RIFF",4)==0 && std::memcmp(bytes.data()+8,"WAVE",4)==0);
            REQUIRE(le32(bytes,4)==bytes.size()-8 && le32(bytes,24)==8000 && le32(bytes,40)==16000);
            REQUIRE(bytes.size()==16044 && std::memcmp(bytes.data()+44,pcm.data(),16000)==0);
        } else {
            // Check complete Ogg pages, Vorbis identification and final sample
            // count; a header-only or unfinalized file must not pass.
            ogg_sync_state sync{};ogg_sync_init(&sync);
            std::memcpy(ogg_sync_buffer(&sync,static_cast<long>(bytes.size())),bytes.data(),bytes.size());
            ogg_sync_wrote(&sync,static_cast<long>(bytes.size()));
            ogg_page page{};bool first=true,eos=false;int pages=0;
            for(int result; (result=ogg_sync_pageout(&sync,&page))!=0;) {
                REQUIRE(result==1);++pages;
                if(first) {
                    REQUIRE(ogg_page_bos(&page));REQUIRE(page.body_len>=16);
                    REQUIRE(page.body[0]==1 && std::memcmp(page.body+1,"vorbis",6)==0 && page.body[11]==1);
                    first=false;
                }
                if(ogg_page_eos(&page)) {eos=true;REQUIRE(ogg_page_granulepos(&page)==8000);}
            }
            ogg_sync_clear(&sync);REQUIRE(pages>=2 && eos);
        }
        std::filesystem::remove(file);
        const auto empty=dir/(format==RecordFormat::WAV?"empty.wav":"empty.ogg");
        REQUIRE(writer.open(empty.string(),8000,1));writer.close();REQUIRE(!std::filesystem::exists(empty));
    }
    REQUIRE(!writer.open((dir/"missing"/"call.wav").string()));
    std::filesystem::remove(dir);
    std::cout << "PASS: voice PCM frames produce complete WAV/OGG recordings; sample counts; finalization; empty-file cleanup; invalid-folder failure\n";
}
