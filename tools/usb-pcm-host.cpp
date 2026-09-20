// WASAPI exclusive-mode PCM source for the USB Audio benchmark.
// Never selects the default device: only the uniquely named lab endpoint.
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <algorithm>
#include <string>

static void check(HRESULT hr,const char *what) {
    if(FAILED(hr)) { std::fprintf(stderr,"ERROR %s HRESULT=0x%08lx\n",what,(unsigned long)hr); std::exit(1); }
}
static uint32_t get32(const uint8_t *p) { uint32_t v; std::memcpy(&v,p,4); return v; }
static uint16_t get16(const uint8_t *p) { uint16_t v; std::memcpy(&v,p,2); return v; }
static uint32_t crc32(const uint8_t *p,size_t n) {
    uint32_t table[256];
    for(unsigned i=0;i<256;i++) { uint32_t c=i; for(unsigned j=0;j<8;j++) c=(c>>1)^((c&1)?0xedb88320:0); table[i]=c; }
    uint32_t c=0xffffffff;
    for(size_t i=0;i<n;i++) c=(c>>8)^table[(c^p[i])&255];
    return ~c;
}
static void le24(std::vector<uint8_t>&v,uint32_t n) {
    v.push_back(n&255);v.push_back((n>>8)&255);v.push_back((n>>16)&255);
}
int wmain(int argc,wchar_t **argv) {
    check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"CoInitializeEx");
    IMMDeviceEnumerator *enumerator=nullptr;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"enumerator");
    IMMDeviceCollection *devices=nullptr;
    check(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&devices),"enumerate render devices");
    UINT count=0; devices->GetCount(&count);
    IMMDevice *selected=nullptr; unsigned matches=0;
    for(UINT i=0;i<count;i++) {
        IMMDevice *dev=nullptr; IPropertyStore *props=nullptr; PROPVARIANT name; PropVariantInit(&name);
        check(devices->Item(i,&dev),"device item");
        check(dev->OpenPropertyStore(STGM_READ,&props),"properties");
        check(props->GetValue(PKEY_Device_FriendlyName,&name),"friendly name");
        bool match=name.vt==VT_LPWSTR && std::wcsstr(name.pwszVal,L"nRF52840 PCM Bench");
        if(argc==1 || match) std::wprintf(L"AUDIO_ENDPOINT match=%u name=%ls\n",match,name.pwszVal);
        if(match) { matches++; if(selected) selected->Release(); selected=dev; dev=nullptr; }
        PropVariantClear(&name); props->Release(); if(dev) dev->Release();
    }
    if(argc==1) { std::printf("MATCHING_ENDPOINTS %u\n",matches); return matches==1?0:2; }
    if(argc!=5 || matches!=1) {
        std::fprintf(stderr,"Usage: usb-pcm-host.exe WAV RUN_ID BITRATE FRAMES (exactly one bench device required)\n"); return 2;
    }
    unsigned run=std::wcstoul(argv[2],nullptr,10),rate=std::wcstoul(argv[3],nullptr,10),frames=std::wcstoul(argv[4],nullptr,10);
    // The firmware accepts the tested 320..400 kb/s sweep. Keep the range
    // explicit so a malformed command cannot request an oversized packet.
    if(!run || run>0xffffff || rate<320000 || rate>400000 || rate%1000 || frames<100 || frames>100000) return 2;
    std::ifstream f(argv[1],std::ios::binary|std::ios::ate);
    if(!f) { std::fprintf(stderr,"WAV open failed\n"); return 2; }
    size_t length=(size_t)f.tellg(); std::vector<uint8_t> wav(length); f.seekg(0);f.read((char*)wav.data(),length);
    if(length<44 || memcmp(wav.data(),"RIFF",4) || memcmp(wav.data()+8,"WAVE",4)) return 2;
    std::vector<uint8_t> audio; bool valid=false;
    for(size_t pos=12;pos+8<=length;) {
        unsigned len=get32(wav.data()+pos+4); if(pos+8ull+len>length) return 2;
        const uint8_t *p=wav.data()+pos+8;
        if(!memcmp(wav.data()+pos,"fmt ",4) && len>=16) {
            unsigned tag=get16(p);
            valid=(tag==1 || (tag==0xfffe && len>=40 && get16(p+18)==24 && get32(p+24)==1)) &&
                get16(p+2)==2 && get32(p+4)==48000 && get16(p+12)==6 && get16(p+14)==24;
        }
        if(!memcmp(wav.data()+pos,"data",4)) audio.assign(p,p+len);
        pos+=8ull+len+(len&1);
    }
    if(!valid || audio.empty() || audio.size()%6) { std::fprintf(stderr,"Requires stereo 48 kHz packed PCM24 WAV\n"); return 2; }
    std::vector<uint8_t> payload((size_t)frames*720);
    for(size_t i=0;i<payload.size();i++) payload[i]=audio[i%audio.size()];
    uint32_t crc=crc32(payload.data(),payload.size());
    uint64_t hash=14695981039346656037ull;
    for(size_t i=0;i<payload.size();i+=4) hash=(hash^get32(payload.data()+i))*1099511628211ull;
    std::vector<uint8_t> stream(48000*6,0); // one second of startup silence
    const char marker[]="NRFPCM24nrf52840NRFPCM24nrf52840NRFPCM24nrf52840";
    stream.insert(stream.end(),marker,marker+48);
    uint32_t command[]={run,rate,frames,0x544553};
    for(auto n:command) le24(stream,n);
    for(auto n:command) le24(stream,n^0xffffff);
    stream.resize(stream.size()+100*720,0); // firmware's explicit codec warm-up
    stream.insert(stream.end(),payload.begin(),payload.end());
    stream.resize(stream.size()+48000*6,0); // allow receiver/encoder drain

    WAVEFORMATEXTENSIBLE format={};
    format.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels=2;format.Format.nSamplesPerSec=48000;
    format.Format.nAvgBytesPerSec=288000;format.Format.nBlockAlign=6;
    format.Format.wBitsPerSample=24;format.Format.cbSize=22;
    format.Samples.wValidBitsPerSample=24;
    format.dwChannelMask=SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT;
    format.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;
    IAudioClient *client=nullptr;
    check(selected->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&client),"activate");
    check(client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&format.Format,nullptr),"exclusive PCM24 format");
    REFERENCE_TIME period=100000; // 10 ms host buffer; device still gets 1 ms packets.
    HRESULT hr=client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 period,period,&format.Format,nullptr);
    if(hr==AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned=0;check(client->GetBufferSize(&aligned),"aligned buffer size");
        period=(REFERENCE_TIME)((10000000ull*aligned+24000)/48000);
        client->Release();client=nullptr;
        check(selected->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&client),"reactivate");
        hr=client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,period,period,&format.Format,nullptr);
    }
    check(hr,"exclusive initialize");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    check(client->SetEventHandle(event),"set audio event");
    IAudioRenderClient *render=nullptr;check(client->GetService(IID_PPV_ARGS(&render)),"render service");
    UINT32 block=0;check(client->GetBufferSize(&block),"buffer size");
    DWORD task=0;HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Pro Audio",&task);
    size_t pos=0;unsigned writes=0;
    auto fill=[&]() {
        BYTE *p=nullptr;check(render->GetBuffer(block,&p),"render GetBuffer");
        size_t n=std::min(stream.size()-pos,(size_t)block*6);
        memcpy(p,stream.data()+pos,n);memset(p+n,0,block*6-n);pos+=n;
        check(render->ReleaseBuffer(block,0),"render ReleaseBuffer");writes++;
    };
    std::printf("PCM_SOURCE id=%u bitrate=%u frames=%u pcm_crc=%08x pcm_hash=%016llx wav_frames=%zu source_bytes=%zu buffer_frames=%u period_100ns=%lld exclusive=1\n",
        run,rate,frames,crc,(unsigned long long)hash,audio.size()/6,payload.size(),block,(long long)period);
    std::fflush(stdout);
    fill();ULONGLONG start=GetTickCount64();check(client->Start(),"start");
    while(pos<stream.size()) {
        if(WaitForSingleObject(event,2000)!=WAIT_OBJECT_0) { std::fprintf(stderr,"Audio event timeout\n");client->Stop();return 3; }
        fill();
    }
    if(WaitForSingleObject(event,2000)!=WAIT_OBJECT_0) return 3;
    check(client->Stop(),"stop");
    std::printf("PCM_DONE id=%u writes=%u elapsed_ms=%llu bytes=%zu\n",run,writes,GetTickCount64()-start,stream.size());
    if(mmcss) AvRevertMmThreadCharacteristics(mmcss);
    render->Release();client->Release();selected->Release();devices->Release();enumerator->Release();CloseHandle(event);CoUninitialize();
    return 0;
}
