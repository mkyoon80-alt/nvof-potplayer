// Reuse the renderer's public D3D11 contract/lifetime fixture, not its GPU source.
#define wmain native_smoke_main
#include "directshow_gpu_smoke.cpp"
#undef wmain
#include "nvof/cpu_video.hpp"
#include "avc_samples.hpp"
#include <functional>

CMediaType cpu_media;
bool cpu_avc_sps=false,cpu_avc_interlaced=false;
bool cpu_weave=false,cpu_mixed=false;
std::function<void(IMediaSample*,ID3D11Device*,ID3D11DeviceContext*)> inspect_cpu_output;
class CpuPin final : public CBaseOutputPin {
public:
    CpuPin(CBaseFilter* f,CCritSec* l,HRESULT* h):CBaseOutputPin(NAME("CPU decoded 10 bit"),f,l,h,L"Output"){}
    HRESULT GetMediaType(int i,CMediaType* t) override {return i<0?E_INVALIDARG:i?VFW_S_NO_MORE_ITEMS:t->Set(cpu_media);}
    HRESULT CheckMediaType(const CMediaType* t) override {return t->subtype==cpu_media.subtype?S_OK:VFW_E_TYPE_NOT_ACCEPTED;}
    HRESULT DecideBufferSize(IMemAllocator* a,ALLOCATOR_PROPERTIES* p) override {
        *p={4,LONG(cpu_media.lSampleSize),1,0};ALLOCATOR_PROPERTIES actual{};return a->SetProperties(p,&actual);
    }
    HRESULT push(const std::vector<uint8_t>& bytes,int index,bool disc=false) {
        ComPtr<IMediaSample> sample;HRESULT hr=GetDeliveryBuffer(&sample,nullptr,nullptr,0);if(FAILED(hr))return hr;
        BYTE* dst=nullptr;check(sample->GetPointer(&dst),"CPU decoder buffer");
        if(sample->GetSize()<LONG(bytes.size()))return E_UNEXPECTED;
        memcpy(dst,bytes.data(),bytes.size());sample->SetActualDataLength(LONG(bytes.size()));
        REFERENCE_TIME start=index*kDuration,end=start+kDuration;
        sample->SetTime(&start,&end);sample->SetSyncPoint(TRUE);sample->SetDiscontinuity(disc);sample->SetPreroll(FALSE);
        if(cpu_weave || cpu_mixed) {
            ComPtr<IMediaSample2> sample2;check(sample.As(&sample2),"sample flags");
            AM_SAMPLE2_PROPERTIES props{};check(sample2->GetProperties(sizeof props,reinterpret_cast<BYTE*>(&props)),"read sample flags");
            props.dwTypeSpecificFlags=AM_VIDEO_FLAG_FIELD1FIRST|((cpu_weave || index%2==0)?AM_VIDEO_FLAG_WEAVE:0);
            check(sample2->SetProperties(sizeof props,reinterpret_cast<BYTE*>(&props)),"set sample flags");
        }
        return Deliver(sample.Get());
    }
};
class CpuEncodedPin final : public CBaseInputPin {
public:
    CpuEncodedPin(CBaseFilter* f,CCritSec* l,HRESULT* h):CBaseInputPin(NAME("AVC input facts"),f,l,h,L"Compressed"){}
    HRESULT CheckMediaType(const CMediaType*) override {return S_OK;}
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* media) override {
        const auto* data=cpu_avc_interlaced?avc_interlaced10:avc_progressive10;
        const size_t size=cpu_avc_interlaced?sizeof avc_interlaced10:sizeof avc_progressive10;
        CMediaType type;type.SetType(&MEDIATYPE_Video);
        const GUID codec={MAKEFOURCC('A','V','C','1'),0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
        type.SetSubtype(&codec);type.SetFormatType(&FORMAT_MPEG2Video);
        const size_t bytes=offsetof(MPEG2VIDEOINFO,dwSequenceHeader)+size;
        auto* h=reinterpret_cast<MPEG2VIDEOINFO*>(type.AllocFormatBuffer(ULONG(bytes)));if(!h)return E_OUTOFMEMORY;
        memset(h,0,bytes);h->cbSequenceHeader=DWORD(size);memcpy(h->dwSequenceHeader,data,size);
        return CopyMediaType(media,&type);
    }
};
class CpuSource final : public CBaseFilter {
public:
    CpuSource(HRESULT* h):CBaseFilter(NAME("Software decoder fixture"),nullptr,&lock_,CLSID_NULL),pin_(this,&lock_,h),encoded_(this,&lock_,h){}
    int GetPinCount() override{return cpu_avc_sps?2:1;}
    CBasePin* GetPin(int n) override{return n==0?static_cast<CBasePin*>(&pin_):n==1 && cpu_avc_sps?&encoded_:nullptr;}
    CpuPin pin_;
private:CCritSec lock_;CpuEncodedPin encoded_;
};
// The same native renderer interface, with staging readback only in this test.
class InspectPin final : public CBaseInputPin,public nvof::transport::DecoderConfiguration {
public:
    DECLARE_IUNKNOWN;
    InspectPin(CBaseFilter* f,CCritSec* l,HRESULT* h):CBaseInputPin(NAME("GPU renderer"),f,l,h,L"Input"){}
    STDMETHODIMP NonDelegatingQueryInterface(REFIID id,void** p) override {
        if(id==__uuidof(nvof::transport::DecoderConfiguration))return GetInterface(static_cast<nvof::transport::DecoderConfiguration*>(this),p);
        return CBaseInputPin::NonDelegatingQueryInterface(id,p);
    }
    STDMETHODIMP ActivateD3D11Decoding(ID3D11Device* d,ID3D11DeviceContext* c,HANDLE,UINT) override {device=d;context=c;return S_OK;}
    STDMETHODIMP_(UINT) GetD3D11AdapterIndex() override{return 0;}
    STDMETHODIMP NotifyAllocator(IMemAllocator* allocator,BOOL readonly) override {
        // Like the player, require GPU activation before buffer negotiation.
        return device?CBaseInputPin::NotifyAllocator(allocator,readonly):VFW_E_NO_ALLOCATOR;
    }
    HRESULT CheckMediaType(const CMediaType* t) override {
        return t->subtype==nvof::p010_subtype && t->formattype==FORMAT_VideoInfo2 &&
            reinterpret_cast<const VIDEOINFOHEADER2*>(t->pbFormat)->dwInterlaceFlags==0?S_OK:VFW_E_TYPE_NOT_ACCEPTED;
    }
    STDMETHODIMP Receive(IMediaSample* sample) override {
        HRESULT hr=CBaseInputPin::Receive(sample);if(hr!=S_OK)return hr;
        try {
            if(!device || !context)throw std::runtime_error("CPU upload renderer was not activated");
            BYTE* invalid=nullptr;if(sample->GetPointer(&invalid)!=E_NOTIMPL || invalid)throw std::runtime_error("Output used CPU transport");
            REFERENCE_TIME start,end;check(sample->GetTime(&start,&end),"output PTS");
            if(end<=start || (!times.empty() && sample->IsDiscontinuity()!=S_OK && start!=ends.back()))throw std::runtime_error("Output cadence gap/overlap");
            inspect_cpu_output(sample,device.Get(),context.Get());times.push_back(start);ends.push_back(end);return S_OK;
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return E_FAIL;}
    }
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    std::vector<int64_t> times,ends;
};
class InspectSink final : public CBaseFilter {
public:
    InspectSink(HRESULT* h):CBaseFilter(NAME("Inspect renderer"),nullptr,&lock_,CLSID_NULL),pin(this,&lock_,h){}
    int GetPinCount() override{return 1;}CBasePin* GetPin(int n)override{return n==0?&pin:nullptr;}
    InspectPin pin;
private:CCritSec lock_;
};
std::vector<uint16_t> read_output(IMediaSample* s,ID3D11Device* device,ID3D11DeviceContext* context) {
    ComPtr<nvof::transport::TextureSample> ts;check(s->QueryInterface(IID_PPV_ARGS(&ts)),"texture interface");
    ComPtr<ID3D11Texture2D> tex,staging;UINT slice=0;check(ts->GetD3D11Texture(0,&tex,&slice),"texture");
    D3D11_TEXTURE2D_DESC d{};tex->GetDesc(&d);if(d.Format!=DXGI_FORMAT_P010)throw std::runtime_error("10 bit lost");
    d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&d,nullptr,&staging),"staging");context->CopyResource(staging.Get(),tex.Get());
    D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"read output");
    std::vector<uint16_t> pixels(size_t(kWidth)*kHeight*3/2);
    for(int y=0;y<kHeight*3/2;++y)memcpy(pixels.data()+size_t(y)*kWidth,static_cast<BYTE*>(map.pData)+size_t(y)*map.RowPitch,kWidth*2);
    context->Unmap(staging.Get(),0);return pixels;
}
#ifndef NVOF_CPU10_HELPERS_ONLY
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc<3)return 2;
    try {
        check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
        const std::wstring fmt=argv[2];const bool fields=fmt.find(L"i")!=std::wstring::npos;
        cpu_avc_sps=fmt.find(L"sps")!=std::wstring::npos;cpu_avc_interlaced=fields;
        cpu_weave=fmt.find(L"weave")!=std::wstring::npos;cpu_mixed=fmt.find(L"mixed")!=std::wstring::npos;
        const bool y210=fmt.find(L"y210")==0,p210=fmt.find(L"p210")==0,full=y210||p210;
        const bool real=argc>3,performance=argc>6;
        kWidth=real?_wtoi(argv[4]):1024;kHeight=real?_wtoi(argv[5]):360;
        kDuration=fields?400000:417083;p010_media=true;p010_fixture=true;
        cpu_media=input_type();auto* h=reinterpret_cast<VIDEOINFOHEADER2*>(cpu_media.pbFormat);
        const int pitch=kWidth*(y210?4:2)+(real?0:32);
        const auto format=y210?nvof::CpuVideoFormat::y210:p210?nvof::CpuVideoFormat::p210:nvof::CpuVideoFormat::p010;
        const size_t bytes=nvof::cpu_video_bytes({kWidth,kHeight,pitch,format});
        cpu_media.SetSubtype(y210?&nvof::y210_subtype:p210?&nvof::p210_subtype:&nvof::p010_subtype);
        h->bmiHeader.biWidth=pitch/(y210?4:2);h->bmiHeader.biCompression=cpu_media.subtype.Data1;
        h->bmiHeader.biBitCount=full?32:24;h->bmiHeader.biSizeImage=DWORD(bytes);cpu_media.SetSampleSize(ULONG(bytes));
        h->dwInterlaceFlags=(fields || fmt.find(L"tag")!=std::wstring::npos)?AMINTERLACE_IsInterlaced|AMINTERLACE_Field1First:0;
        HMODULE module=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);if(!module)throw std::runtime_error("Load filter");
        auto get=reinterpret_cast<HRESULT(WINAPI*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));
        {
        ComPtr<IClassFactory> factory;check(get(kFilter,IID_PPV_ARGS(&factory)),"factory");
        ComPtr<IBaseFilter> filter;check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&filter)),"filter");
        ComPtr<IGraphBuilder> graph;check(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)),"graph");
        HRESULT hr=S_OK;auto* source=new CpuSource(&hr);check(hr,"source");auto* sink=new InspectSink(&hr);check(hr,"sink");
        ComPtr<IBaseFilter> src=source,dst=sink;check(graph->AddFilter(src.Get(),L"CPU decoder"),"add source");
        check(graph->AddFilter(filter.Get(),L"NVOF"),"add filter");check(graph->AddFilter(dst.Get(),L"D3D11 renderer"),"add sink");
        ComPtr<IPin> in,out;check(filter->FindPin(L"In",&in),"input");check(filter->FindPin(L"Out",&out),"output");
        check(graph->ConnectDirect(&source->pin_,in.Get(),&cpu_media),"CPU connection");
        check(graph->ConnectDirect(out.Get(),&sink->pin,nullptr),"renderer connection");
        ComPtr<IMediaControl> control;check(graph.As(&control),"control");check(control->Run(),"Run CPU upload");
        // Must keep an ordinary upstream allocator after activating GPU output.
        ComPtr<IMemInputPin> mem;check(in.As(&mem),"input memory");ComPtr<IMemAllocator> allocator;check(mem->GetAllocator(&allocator),"CPU allocator retained");
        std::vector<uint8_t> frame(bytes,0xcc);std::vector<uint16_t> expected;size_t exact=0,total=0,lowbits=0;
        ComPtr<IMediaSample> paused_sample;std::vector<uint16_t> paused_pixels;
        inspect_cpu_output=[&](IMediaSample* s,ID3D11Device* d,ID3D11DeviceContext* c) {
            if(performance){++total;return;}
            auto pixels=read_output(s,d,c);REFERENCE_TIME a,b;s->GetTime(&a,&b);
            if(!paused_sample){paused_sample=s;paused_pixels=pixels;}
            for(auto v:pixels){if(v&63)throw std::runtime_error("P010 low bits");if((v>>6)&3)++lowbits;}
            // Every original progressive frame must be bit-exact.
            if(!fields && std::llabs(a-((a+kDuration/2)/kDuration)*kDuration)<20){if(pixels!=expected)throw std::runtime_error("Original 10-bit pixels changed");++exact;}
            ++total;
        };
        std::ifstream raw;if(real)raw.open(std::filesystem::path(argv[3]),std::ios::binary);
        const auto begin=std::chrono::steady_clock::now();int frames=0;
        for(int index=0;index<(real?48:12);++index) {
            if(real){raw.read(reinterpret_cast<char*>(frame.data()),frame.size());if(raw.gcount()!=std::streamsize(frame.size()))break;}
            else {
                auto put=[&](size_t p,int v){uint16_t w=uint16_t(v<<6);memcpy(frame.data()+p,&w,2);};
                for(int y=0;y<kHeight;++y)for(int x=0;x<kWidth;x+=2) {
                    const int value=(x+index*8)%1024;
                    if(y210){size_t p=size_t(y)*pitch+x*4;put(p,value);put(p+2,301);put(p+4,(value+1)%1024);put(p+6,701);}
                    else{put(size_t(y)*pitch+x*2,value);put(size_t(y)*pitch+x*2+2,(value+1)%1024);}
                }
                if(!y210)for(int y=0;y<(full?kHeight:kHeight/2);++y)for(int x=0;x<kWidth;x+=2){size_t p=size_t(pitch)*kHeight+size_t(y)*pitch+x*2;put(p,301);put(p+2,701);}
            }
            auto planes=nvof::unpack_cpu_video(frame.data(),frame.size(),{kWidth,kHeight,pitch,format});expected=nvof::pack_cpu_p010(planes);
            check(source->pin_.push(frame,index,index==0),"CPU frame interpolation");++frames;
            if(index==4){check(control->Pause(),"pause retained GPU frame");check(control->Run(),"resume GPU frame");}
        }
        check(source->pin_.DeliverEndOfStream(),"EOS");
        if(paused_sample && read_output(paused_sample.Get(),sink->pin.device.Get(),sink->pin.context.Get())!=paused_pixels)
            throw std::runtime_error("Paused frame was overwritten by subsequent uploads");
        paused_sample.Reset();
        if(cpu_avc_sps && total!=size_t(frames*(fields?4:5)))throw std::runtime_error("Wrong progressive/field-rate multiplier");
        if(sink->pin.ends.back()!=int64_t(frames)*kDuration && (cpu_weave || cpu_mixed))throw std::runtime_error("Missing last progressive/field duration");
        const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        if(frames<2 || (!performance && !lowbits) || total<size_t(frames*(fields?2:1)) || (!performance && !fields && exact!=size_t(frames)))throw std::runtime_error("Missing precision or frames: "+std::to_string(frames)+"/"+std::to_string(total)+"/"+std::to_string(exact)+"/"+std::to_string(lowbits));
        check(source->pin_.DeliverBeginFlush(),"seek flush");check(source->pin_.DeliverEndFlush(),"seek end");
        check(source->pin_.DeliverNewSegment(0,INT64_MAX,1),"seek segment");check(source->pin_.push(frame,0,true),"seek restart");
        check(control->Stop(),"Stop");check(control->Run(),"restart");check(source->pin_.push(frame,0,true),"restart first");check(control->Stop(),"final Stop");
        paused_sample.Reset();
        std::cout<<"PASS CPU input -> P010 GPU: frames="<<frames<<" output="<<total<<" exact originals="<<exact<<" readback="<<!performance<<" seconds="<<elapsed<<" seek/restart=OK\n";
        }
        auto unload=reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(module,"DllCanUnloadNow"));if(unload()!=S_OK)throw std::runtime_error("COM leak");
        FreeLibrary(module);CoUninitialize();return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
#endif
