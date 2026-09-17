// Hardware-independent fault injection for the SDRplay Soapy C API boundary.
#include "sdr/sdrplay_source.h"
#include <SoapySDR/Device.h>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#define REQUIRE(x) do { if(!(x)) {std::cerr << "FAILED line " << __LINE__ << ": " #x << std::endl; std::exit(1);} } while(0)
struct SoapySDRDevice { std::string serial, mode; double frequency=1545e6,rate=2e6,bandwidth=1536000;std::map<std::string,std::string> settings; };
struct SoapySDRStream { int reads=0; };
static int devices=0,streams=0;
static bool failOpen=false,failSetup=false,failActivate=false;
static bool failGain=false;
static std::string gainSerial, gainKey;
static double gainValue=0;
static std::atomic<bool> failRead{false};
static const char* models[] = {"RSPdx", "RSPduo", "RSP1", "RSP1A", "RSP1B", "RSP2", "RSP2pro", "RSPdx-R2"};
static const char* serials[] = {"001", "002", "003", "004", "005", "006", "007", "008"};
static char* copy(const char* s) {auto* p=(char*)malloc(strlen(s)+1);strcpy(p,s);return p;}
static char** list(std::initializer_list<const char*> values,size_t* n) {
*n=values.size();auto** p=(char**)calloc(*n,sizeof(char*));size_t i=0;for(auto v:values)p[i++]=copy(v);return p;}
static double* nums(std::initializer_list<double> values,size_t* n) {
*n=values.size();auto* p=(double*)calloc(*n,sizeof(double));size_t i=0;for(auto v:values)p[i++]=v;return p;}

char* SoapySDRDevice_getHardwareKey(const SoapySDRDevice* device) {
    return copy(models[std::stoi(device->serial)-1]);
}

void SoapySDRArgInfoList_clear(SoapySDRArgInfo *info, const size_t length) {
    for(size_t i=0;i<length;++i) {free(info[i].key);free(info[i].value);free(info[i].name);} free(info);
}

int SoapySDRDevice_activateStream(SoapySDRDevice *device,
    SoapySDRStream *stream,
    const int flags,
    const long long timeNs,
    const size_t numElems) {
    return failActivate?-1:0;
}

int SoapySDRDevice_closeStream(SoapySDRDevice *device, SoapySDRStream *stream) {
    delete stream;--streams;return 0;
}

int SoapySDRDevice_deactivateStream(SoapySDRDevice *device,
    SoapySDRStream *stream,
    const int flags,
    const long long timeNs) {
    return 0;
}

SoapySDRKwargs *SoapySDRDevice_enumerateStrArgs(const char *args, size_t *length) {
    REQUIRE(std::string(args)=="driver=sdrplay");
    *length=12; auto* result=(SoapySDRKwargs*)calloc(*length,sizeof(SoapySDRKwargs));
    for(size_t i=0;i<8;++i) {
        SoapySDRKwargs_set(&result[i],"serial",serials[i]);
        SoapySDRKwargs_set(&result[i],"label",models[i]);
    }
    SoapySDRKwargs_set(&result[1],"mode","ST");
    const char* modes[]={"DT","MA","MA8","SL"};
    for(size_t i=8;i<12;++i) {
        SoapySDRKwargs_set(&result[i],"serial","002");
        SoapySDRKwargs_set(&result[i],"label","RSPduo");
        SoapySDRKwargs_set(&result[i],"mode",modes[i-8]);
    }
    return result;
}

SoapySDRArgInfo *SoapySDRDevice_getChannelSettingInfo(const SoapySDRDevice *device, const int direction, const size_t channel, size_t *length) {
    *length=0;return nullptr;
}

double SoapySDRDevice_getFrequency(const SoapySDRDevice *device, const int direction, const size_t channel) {
    return device->frequency;
}

double SoapySDRDevice_getGainElement(const SoapySDRDevice *device, const int direction, const size_t channel, const char *name) {
    return 20;
}

SoapySDRRange SoapySDRDevice_getGainElementRange(const SoapySDRDevice *device, const int direction, const size_t channel, const char *name) {
    return {0,59,1};
}

double SoapySDRDevice_getSampleRate(const SoapySDRDevice *device, const int direction, const size_t channel) {
    return device->rate;
}

SoapySDRArgInfo *SoapySDRDevice_getSettingInfo(const SoapySDRDevice *device, size_t *length) {
    *length=2;auto* p=(SoapySDRArgInfo*)calloc(2,sizeof(SoapySDRArgInfo));
p[0].key=copy("biasT_ctrl");p[0].name=copy("Bias tee");p[0].value=copy("false");p[0].type=SOAPY_SDR_ARG_INFO_BOOL;
p[1].key=copy("agc_setpoint");p[1].name=copy("AGC setpoint");p[1].value=copy("-30");p[1].type=SOAPY_SDR_ARG_INFO_INT;p[1].range={-60,0,1};return p;
}

bool SoapySDRDevice_hasDCOffsetMode(const SoapySDRDevice *device, const int direction, const size_t channel) {
    return true;
}

bool SoapySDRDevice_hasFrequencyCorrection(const SoapySDRDevice *device, const int direction, const size_t channel) {
    return true;
}

bool SoapySDRDevice_hasGainMode(const SoapySDRDevice *device, const int direction, const size_t channel) {
    return true;
}

bool SoapySDRDevice_hasIQBalanceMode(const SoapySDRDevice *device, const int direction, const size_t channel) {
    return true;
}

const char *SoapySDRDevice_lastError(void) {
    return "injected driver failure";
}

char **SoapySDRDevice_listAntennas(const SoapySDRDevice *device, const int direction, const size_t channel, size_t *length) {
    return list({"Antenna A","Antenna B","Antenna C"},length);
}

double *SoapySDRDevice_listBandwidths(const SoapySDRDevice *device, const int direction, const size_t channel, size_t *length) {
    return nums({200000,1536000,8000000},length);
}

char **SoapySDRDevice_listGains(const SoapySDRDevice *device, const int direction, const size_t channel, size_t *length) {
    return list({"IFGR","RFGR"},length);
}

double *SoapySDRDevice_listSampleRates(const SoapySDRDevice *device, const int direction, const size_t channel, size_t *length) {
    return nums({62500,2000000,6000000,10000000},length);
}

SoapySDRDevice *SoapySDRDevice_make(const SoapySDRKwargs *args) {
    if(failOpen)return nullptr; auto* d=new SoapySDRDevice;
const char* a=SoapySDRKwargs_get(args,"antenna"); REQUIRE(!a || std::string(a).rfind("Tuner ",0)==0);
d->serial=SoapySDRKwargs_get(args,"serial");
const char* mode=SoapySDRKwargs_get(args,"mode");
// Match the real driver's rejection of RSPduo-only arguments on other models.
REQUIRE(d->serial=="002" ? mode!=nullptr : mode==nullptr && a==nullptr);
d->mode=mode?mode:"";++devices;return d;
}

char *SoapySDRDevice_readChannelSetting(const SoapySDRDevice *device, const int direction, const size_t channel, const char *key) {
    return copy("");
}

char *SoapySDRDevice_readSetting(const SoapySDRDevice *device, const char *key) {
    return copy(!strcmp(key,"biasT_ctrl")?"false":"-30");
}

int SoapySDRDevice_readStream(SoapySDRDevice *device,
    SoapySDRStream *stream,
    void * const *buffs,
    const size_t numElems,
    int *flags,
    long long *timeNs,
    const long timeoutUs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
int n=stream->reads++;if(n==0)return SOAPY_SDR_TIMEOUT;if(n==1)return SOAPY_SDR_OVERFLOW;
if(failRead)return SOAPY_SDR_STREAM_ERROR;
float* p=(float*)buffs[0];p[0]=device->serial=="001"?0.25f:0.5f;p[1]=-p[0];return 1;
}

int SoapySDRDevice_setAntenna(SoapySDRDevice *device, const int direction, const size_t channel, const char *name) {
    return 0;
}

int SoapySDRDevice_setBandwidth(SoapySDRDevice *device, const int direction, const size_t channel, const double bw) {
    device->bandwidth = bw > 0 ? bw : 1536000;
    return 0;
}
double SoapySDRDevice_getBandwidth(const SoapySDRDevice* device, const int, const size_t) { return device->bandwidth; }

int SoapySDRDevice_setDCOffsetMode(SoapySDRDevice *device, const int direction, const size_t channel, const bool automatic) {
    return 0;
}

int SoapySDRDevice_setFrequency(SoapySDRDevice *device, const int direction, const size_t channel, const double frequency, const SoapySDRKwargs *args) {
    device->frequency=frequency;return 0;
}

int SoapySDRDevice_setFrequencyCorrection(SoapySDRDevice *device, const int direction, const size_t channel, const double value) {
    REQUIRE(device->mode!="SL");return 0;
}

int SoapySDRDevice_setGain(SoapySDRDevice *device, const int direction, const size_t channel, const double value) {
    return 0;
}

int SoapySDRDevice_setGainElement(SoapySDRDevice *device, const int direction, const size_t channel, const char *name, const double value) {
    if(failGain) return -1;
    gainSerial=device->serial;gainKey=name;gainValue=value;
    return 0;
}

int SoapySDRDevice_setGainMode(SoapySDRDevice *device, const int direction, const size_t channel, const bool automatic) {
    return 0;
}

int SoapySDRDevice_setIQBalanceMode(SoapySDRDevice *device, const int direction, const size_t channel, const bool automatic) {
    return 0;
}

int SoapySDRDevice_setSampleRate(SoapySDRDevice *device, const int direction, const size_t channel, const double rate) {
    device->rate=rate==2000001?2000000:rate;return 0;
}

SoapySDRStream *SoapySDRDevice_setupStream(SoapySDRDevice *device,
    const int direction,
    const char *format,
    const size_t *channels,
    const size_t numChans,
    const SoapySDRKwargs *args) {
    REQUIRE(!strcmp(format,"CF32") && numChans==1 && channels[0]==0); if(failSetup)return nullptr;++streams;return new SoapySDRStream;
}

int SoapySDRDevice_unmake(SoapySDRDevice *device) {
    delete device;--devices;return 0;
}

int SoapySDRDevice_writeChannelSetting(SoapySDRDevice *device, const int direction, const size_t channel, const char *key, const char *value) {
    device->settings[key]=value;return 0;
}

int SoapySDRDevice_writeSetting(SoapySDRDevice *device, const char *key, const char *value) {
    device->settings[key]=value;return 0;
}

void SoapySDRKwargsList_clear(SoapySDRKwargs *args, const size_t length) {
    for(size_t i=0;i<length;++i) SoapySDRKwargs_clear(&args[i]); free(args);
}

void SoapySDRKwargs_clear(SoapySDRKwargs *args) {
    for(size_t i=0;i<args->size;++i) {free(args->keys[i]);free(args->vals[i]);} free(args->keys);free(args->vals);*args={};
}

const char *SoapySDRKwargs_get(const SoapySDRKwargs *args, const char *key) {
    for(size_t i=0;i<args->size;++i) if(!strcmp(args->keys[i],key)) return args->vals[i]; return nullptr;
}

int SoapySDRKwargs_set(SoapySDRKwargs *args, const char *key, const char *val) {
    for(size_t i=0;i<args->size;++i) if(!strcmp(args->keys[i],key)) {free(args->vals[i]);args->vals[i]=copy(val);return 0;}
args->keys=(char**)realloc(args->keys,(args->size+1)*sizeof(char*));
args->vals=(char**)realloc(args->vals,(args->size+1)*sizeof(char*));
args->keys[args->size]=copy(key);args->vals[args->size++]=copy(val);return 0;
}

void SoapySDRStrings_clear(char ***elems, const size_t length) {
    for(size_t i=0;i<length;++i) free((*elems)[i]);free(*elems);
}

const char *SoapySDR_errToStr(const int errorCode) {
    return "injected stream failure";
}

void SoapySDR_free(void *ptr) {
    free(ptr);
}


static void waitUntil(const std::function<bool()>& condition) {
    for(int i=0;i<2000 && !condition();++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(condition());
}
int main() {
    RspConfig a; a.serial="001";a.antenna="Antenna B";a.agc=false;a.gains["IFGR"]=30;
    a.settings["device:biasT_ctrl"]="true";a.settings["device:agc_setpoint"]="-25";
    RspConfig round; REQUIRE(parseRspConfig(serializeRspConfig(a),round));
    REQUIRE(serializeRspConfig(a)==serializeRspConfig(round));
    REQUIRE(!parseRspConfig("broken",round)); REQUIRE(round.serial=="001");
    REQUIRE(!parseRspConfig(serializeRspConfig(a)+" trailing",round));
    auto bad=a;bad.rate=-1; REQUIRE(!parseRspConfig(serializeRspConfig(bad),round));
    SdrplaySource rx,rxB;std::string err;
    const auto found=rx.listDevices(); REQUIRE(found.size()==8);
    for(size_t i=0;i<8;++i) {
        REQUIRE(found[i].serial==serials[i] && found[i].name==models[i]);
        REQUIRE(found[i].hasTunerModes==(i==1));
        RspConfig model; model.serial=serials[i];
        REQUIRE(rx.prepare(model,err)); REQUIRE(rx.apply(model,err)); rx.close();
    }
    bad=a;bad.serial="missing";REQUIRE(!rx.prepare(bad,err));REQUIRE(devices==0);
    failOpen=true;REQUIRE(!rx.prepare(a,err)); REQUIRE(devices==0);failOpen=false;
    bad=a;bad.mode="MA";REQUIRE(!rx.prepare(bad,err));REQUIRE(devices==0);
    REQUIRE(rx.prepare(a,err)); REQUIRE(rx.antennas().size()==3 && rx.controls().size()==2);
    bad=a;bad.antenna="missing";REQUIRE(!rx.apply(bad,err));
    bad=a;bad.gains["IFGR"]=60;REQUIRE(!rx.apply(bad,err));
    bad=a;bad.settings["device:agc_setpoint"]="bad";REQUIRE(!rx.apply(bad,err));
    bad=a;bad.settings["device:agc_setpoint"]="-100";REQUIRE(!rx.apply(bad,err));
    bad=a;bad.settings["device:agc_setpoint"]="-25.5";REQUIRE(!rx.apply(bad,err));
    a.rate=2000001;REQUIRE(rx.apply(a,err));REQUIRE(rx.sampleRate()==2000000);
    REQUIRE(rx.bandwidth()==1536000);
    a.bandwidth=200000; REQUIRE(rx.apply(a,err)); REQUIRE(rx.bandwidth()==200000);
    a.bandwidth=0; REQUIRE(rx.apply(a,err)); REQUIRE(rx.bandwidth()==1536000);
    failSetup=true;REQUIRE(!rx.start(0,{},err));REQUIRE(streams==0);failSetup=false;
    failActivate=true;REQUIRE(!rx.start(0,{},err));REQUIRE(streams==0);failActivate=false;
    std::atomic<int> countA{0},countB{0};
    REQUIRE(rx.start(0,[&](const float* iq,int n){REQUIRE(n==1 && iq[0]==0.25f && iq[1]==-0.25f);++countA;},err));
    REQUIRE(!rx.apply(a,err)); REQUIRE(!rx.prepare(a,err));
    RspConfig b=a;b.serial="002";REQUIRE(rxB.prepare(b,err)); REQUIRE(rxB.apply(b,err));
    REQUIRE(rxB.start(0,[&](const float* iq,int n){REQUIRE(n==1 && iq[0]==0.5f);++countB;},err));
    waitUntil([&]{return countA>3 && countB>3;});REQUIRE(rx.overflows()==1 && rxB.overflows()==1);
    const int beforeGain=countA;
    REQUIRE(rx.setGainElement("RFGR",7));
    REQUIRE(gainSerial=="001" && gainKey=="RFGR" && gainValue==7);
    REQUIRE(rxB.setGainElement("RFGR",8));
    REQUIRE(gainSerial=="002" && gainValue==8);
    REQUIRE(!rx.setGainElement("RFGR",60)); REQUIRE(!rx.setGainElement("missing",2));
    failGain=true; REQUIRE(!rx.setGainElement("RFGR",9)); failGain=false;
    REQUIRE(rx.running() && rxB.running() && streams==2);
    waitUntil([&]{return countA>beforeGain+3;});
    rxB.stop();int stopped=countB;rxB.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); REQUIRE(countB==stopped && rx.running());
    failRead=true;waitUntil([&]{return !rx.running();}); REQUIRE(rx.streamFailed() && !rx.error().empty());
    rx.close();REQUIRE(devices==0 && streams==0);failRead=false;
    b.mode="SL";REQUIRE(rxB.prepare(b,err));REQUIRE(rxB.apply(b,err));rxB.close();
    b.agc=true;b.mode="ST";REQUIRE(rxB.prepare(b,err));REQUIRE(rxB.apply(b,err));
    REQUIRE(rxB.setGainElement("RFGR",5));REQUIRE(!rxB.setGainElement("IFGR",25));rxB.close();
    REQUIRE(devices==0 && streams==0);
    std::cout << "PASS: all eight RSP models, model-specific open arguments, discovery deduplication, config persistence, capabilities, validation, startup rollback, independent streams, overflow, disconnect, stop and slave PPM\n";
}
