// Reuse the renderer's public D3D11 contract/lifetime fixture, not its GPU source.
#define wmain native_smoke_main
#include "directshow_gpu_smoke.cpp"
#undef wmain
#include "nvof/cpu_video.hpp"
#include "avc_samples.hpp"
#include <functional>

CMediaType cpu_media;
bool cpu_avc_sps=false,cpu_avc_interlaced=false;
std::vector<uint8_t> cpu_hevc_sequence;
std::wstring cpu_av1_case;
unsigned cpu_hdr_transfer=MFVideoTransFunc_2084;
bool cpu_weave=false,cpu_mixed=false;
bool cpu_hdr_fixture=false,cpu_hdr_reconnect=false;
bool cpu_offered_allocator=false;
bool cpu_deny_p010=false;
std::function<void(IMediaSample*,ID3D11Device*,ID3D11DeviceContext*)> inspect_cpu_output;
class CpuPin final : public CBaseOutputPin {
public:
    CpuPin(CBaseFilter* f,CCritSec* l,HRESULT* h):CBaseOutputPin(NAME("CPU decoded 10 bit"),f,l,h,L"Output"){}
    HRESULT GetMediaType(int i,CMediaType* t) override {return i<0?E_INVALIDARG:i?VFW_S_NO_MORE_ITEMS:t->Set(cpu_media);}
    HRESULT CheckMediaType(const CMediaType* t) override {
        if(cpu_deny_p010 && t->subtype==nvof::p010_subtype)return VFW_E_TYPE_NOT_ACCEPTED;
        return t->subtype==cpu_media.subtype || (cpu_hdr_reconnect && t->subtype==MEDIASUBTYPE_NV12)?S_OK:VFW_E_TYPE_NOT_ACCEPTED;
    }
    HRESULT DecideBufferSize(IMemAllocator* a,ALLOCATOR_PROPERTIES* p) override {
        *p={4,LONG(m_mt.lSampleSize),1,0};ALLOCATOR_PROPERTIES actual{};return a->SetProperties(p,&actual);
    }
    HRESULT DecideAllocator(IMemInputPin* receiver,IMemAllocator** allocator) override {
        if(!cpu_hdr_fixture || cpu_offered_allocator)return CBaseOutputPin::DecideAllocator(receiver,allocator);
        HRESULT hr=S_OK;auto* raw=new nvof::transport::SurfaceAllocator(&hr,false);
        if(FAILED(hr)){delete raw;return hr;}
        ComPtr<IMemAllocator> candidate;check(raw->QueryInterface(IID_PPV_ARGS(&candidate)),"CPU side-data allocator");
        ALLOCATOR_PROPERTIES props{};check(DecideBufferSize(candidate.Get(),&props),"CPU side-data buffers");
        check(receiver->NotifyAllocator(candidate.Get(),FALSE),"CPU side-data allocator notification");
        *allocator=candidate.Detach();return S_OK;
    }
    HRESULT push(const std::vector<uint8_t>& bytes,int index,bool disc=false) {
        ComPtr<IMediaSample> sample;HRESULT hr=GetDeliveryBuffer(&sample,nullptr,nullptr,0);if(FAILED(hr))return hr;
        BYTE* dst=nullptr;check(sample->GetPointer(&dst),"CPU decoder buffer");
        if(sample->GetSize()<LONG(bytes.size()))return E_UNEXPECTED;
        memcpy(dst,bytes.data(),bytes.size());sample->SetActualDataLength(LONG(bytes.size()));
        REFERENCE_TIME start=index*kDuration,end=start+kDuration;
        sample->SetTime(&start,&end);sample->SetSyncPoint(TRUE);sample->SetDiscontinuity(disc);sample->SetPreroll(FALSE);
        if(cpu_hdr_fixture && m_mt.subtype!=nvof::p010_subtype)return VFW_E_TYPE_NOT_ACCEPTED;
        if(cpu_hdr_fixture && !cpu_offered_allocator) {
            ComPtr<nvof::MediaSideData> side;check(sample.As(&side),"CPU sample side data");
            const double mastering[10]={.265,.15,.68,.69,.06,.32,.3127,.329,1000,.005};const unsigned light[2]={604,120};
            check(side->SetSideData(nvof::hdr_mastering_id,reinterpret_cast<const BYTE*>(mastering),sizeof(mastering)),"CPU mastering");
            check(side->SetSideData(nvof::hdr_light_id,reinterpret_cast<const BYTE*>(light),sizeof(light)),"CPU content light");
            for(int i=2;i<5;++i) {
                std::vector<BYTE> payload(i==2?128:i==3?5225:9353,BYTE(index));
                check(side->SetSideData(nvof::hdr_ids[i],payload.data(),payload.size()),"CPU Dolby metadata");
            }
        }
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
        if(!cpu_av1_case.empty()) {
            auto seq=av1_fixture::pq;
            if(cpu_av1_case==L"av1-truncated")seq.pop_back();
            if(cpu_av1_case==L"av1-missing")seq.clear();
            CMediaType type;type.SetType(&MEDIATYPE_Video);
            GUID codec=MEDIASUBTYPE_NV12;codec.Data1=MAKEFOURCC('A','V','0','1');
            if(cpu_av1_case==L"av1-wrong-codec")codec.Data1=MAKEFOURCC('V','P','9','0');
            if(cpu_av1_case==L"av1-wrong-guid")codec.Data4[7]^=1;
            type.SetSubtype(&codec);
            const bool vi1=cpu_av1_case==L"av1-vi1",vi2=cpu_av1_case==L"av1-vi2";
            const size_t offset=vi1?sizeof(VIDEOINFOHEADER):vi2?sizeof(VIDEOINFOHEADER2):offsetof(MPEG2VIDEOINFO,dwSequenceHeader);
            auto* bytes=type.AllocFormatBuffer(ULONG(offset+seq.size()));if(!bytes)return E_OUTOFMEMORY;
            memset(bytes,0,offset+seq.size());memcpy(bytes+offset,seq.data(),seq.size());
            VIDEOINFOHEADER2 h{};h.AvgTimePerFrame=kDuration;
            h.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);h.bmiHeader.biWidth=kWidth;h.bmiHeader.biHeight=kHeight;
            if(cpu_av1_case.find(L"av1-fields")==0)h.dwInterlaceFlags=AMINTERLACE_IsInterlaced;
            if(cpu_av1_case==L"av1-size-mismatch")h.bmiHeader.biHeight+=2;
            if(cpu_av1_case==L"av1-rate-mismatch")h.AvgTimePerFrame*=2;
            if(cpu_av1_case==L"av1-no-rate")h.AvgTimePerFrame=0;
            if(vi1) {
                auto* p=reinterpret_cast<VIDEOINFOHEADER*>(bytes);p->bmiHeader=h.bmiHeader;p->AvgTimePerFrame=h.AvgTimePerFrame;
                type.SetFormatType(&FORMAT_VideoInfo);
            } else {
                *reinterpret_cast<VIDEOINFOHEADER2*>(bytes)=h;
                type.SetFormatType(vi2?&FORMAT_VideoInfo2:&FORMAT_MPEG2Video);
                if(!vi2)reinterpret_cast<MPEG2VIDEOINFO*>(bytes)->cbSequenceHeader=DWORD(seq.size()+(cpu_av1_case==L"av1-bad-length"?1:0));
            }
            return CopyMediaType(media,&type);
        }
        if(cpu_hdr_fixture || !cpu_hevc_sequence.empty()) {
            auto seq=cpu_hevc_sequence.empty()?hevc_fixture::hvcc(cpu_hdr_transfer==MFVideoTransFunc_HLG?18:16):cpu_hevc_sequence;CMediaType type;type.SetType(&MEDIATYPE_Video);
            GUID codec=MEDIASUBTYPE_NV12;codec.Data1=MAKEFOURCC('H','V','C','1');type.SetSubtype(&codec);type.SetFormatType(&FORMAT_MPEG2Video);
            const size_t bytes=offsetof(MPEG2VIDEOINFO,dwSequenceHeader)+seq.size();
            auto* h=reinterpret_cast<MPEG2VIDEOINFO*>(type.AllocFormatBuffer(ULONG(bytes)));if(!h)return E_OUTOFMEMORY;
            memset(h,0,bytes);h->cbSequenceHeader=DWORD(seq.size());memcpy(h->dwSequenceHeader,seq.data(),seq.size());return CopyMediaType(media,&type);
        }
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
    int GetPinCount() override{return cpu_avc_sps || cpu_hdr_fixture || !cpu_hevc_sequence.empty() || !cpu_av1_case.empty()?2:1;}
    CBasePin* GetPin(int n) override{return n==0?static_cast<CBasePin*>(&pin_):n==1 && GetPinCount()==2?&encoded_:nullptr;}
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
        cpu_hdr_fixture=fmt.find(L"hdr")!=std::wstring::npos;cpu_hdr_reconnect=fmt.find(L"reconnect")!=std::wstring::npos;
        cpu_offered_allocator=fmt.find(L"offered")!=std::wstring::npos;
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
        CMediaType initial_media=cpu_media;
        if(cpu_hdr_reconnect) {
            initial_media.SetSubtype(&MEDIASUBTYPE_NV12);auto* initial=reinterpret_cast<VIDEOINFOHEADER2*>(initial_media.pbFormat);
            initial->bmiHeader.biCompression=MAKEFOURCC('N','V','1','2');initial->bmiHeader.biBitCount=12;
            initial->bmiHeader.biSizeImage=DWORD(bytes/2);initial_media.SetSampleSize(ULONG(bytes/2));
        }
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
        check(graph->ConnectDirect(&source->pin_,in.Get(),&initial_media),"CPU connection");
        check(graph->ConnectDirect(out.Get(),&sink->pin,nullptr),"renderer connection");
        if(cpu_hdr_fixture) {
            CMediaType actual;check(in->ConnectionMediaType(&actual),"negotiated CPU HDR input");
            if(actual.subtype!=nvof::p010_subtype)throw std::runtime_error("CPU HDR decoder remained 8 bit");
            check(out->ConnectionMediaType(&actual),"negotiated CPU HDR output");
            DXVA2_ExtendedFormat color{};color.value=reinterpret_cast<VIDEOINFOHEADER2*>(actual.pbFormat)->dwControlFlags;
            if(actual.subtype!=nvof::p010_subtype || color.VideoTransferFunction!=MFVideoTransFunc_2084 || color.VideoPrimaries!=MFVideoPrimaries_BT2020)
                throw std::runtime_error("CPU HDR color signalling lost");
        }
        ComPtr<IMediaControl> control;check(graph.As(&control),"control");check(control->Run(),"Run CPU upload");
        // Must keep an ordinary upstream allocator after activating GPU output.
        ComPtr<IMemInputPin> mem;check(in.As(&mem),"input memory");ComPtr<IMemAllocator> allocator;check(mem->GetAllocator(&allocator),"CPU allocator retained");
        std::vector<uint8_t> frame(bytes,0xcc);std::vector<uint16_t> expected;size_t exact=0,total=0,lowbits=0;
        ComPtr<IMediaSample> paused_sample;std::vector<uint16_t> paused_pixels;
        inspect_cpu_output=[&](IMediaSample* s,ID3D11Device* d,ID3D11DeviceContext* c) {
            if(performance){++total;return;}
            auto pixels=read_output(s,d,c);REFERENCE_TIME a,b;s->GetTime(&a,&b);
            if(cpu_hdr_fixture && !cpu_offered_allocator) {
                auto metadata=nvof::capture_hdr(s);const BYTE expected_index=BYTE((a+10000)/kDuration);
                if(!metadata || metadata->data[0].size()!=80 || metadata->data[1].size()!=8)throw std::runtime_error("CPU HDR metadata missing");
                for(int i=2;i<5;++i)if(metadata->data[i].size()!=size_t(i==2?128:i==3?5225:9353) ||
                    !std::all_of(metadata->data[i].begin(),metadata->data[i].end(),[&](BYTE v){return v==expected_index;}))
                    throw std::runtime_error("CPU Dolby metadata ownership/timing failed");
            }
            if(cpu_offered_allocator && nvof::capture_hdr(s))throw std::runtime_error("CPU received unprovided HDR side data");
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
        if(fmt.find(L"toggle")!=std::wstring::npos) {
            // Keep the HDR renderer connected while a decoder switches from
            // host P010 to legacy NV12 media types carrying native P010.
            recover_transfer=16;hdr_transfer=MFVideoTransFunc_2084;dovi_fixture=true;
            color_fixture=1;p010_media=false;p010_fixture=true;
            reject_p010_media=fmt.find(L"nv12only")!=std::wstring::npos;
            auto* hw=new Source(&hr);check(hr,"toggle HW source");ComPtr<IBaseFilter> hw_ref=hw;
            check(graph->AddFilter(hw_ref.Get(),L"HW decoder"),"add toggle HW source");
            const CMediaType legacy=input_type();
            CMediaType retained_output;check(out->ConnectionMediaType(&retained_output),"toggle original output");
            bool hardware=false;size_t delivered=0;
            inspect_cpu_output=[&](IMediaSample* sample,ID3D11Device* device,ID3D11DeviceContext* context) {
                auto pixels=read_output(sample,device,context);
                auto side=nvof::capture_hdr(sample);REFERENCE_TIME a,b;check(sample->GetTime(&a,&b),"toggle PTS");
                if(!hardware && cpu_offered_allocator) {
                    if(side)throw std::runtime_error("native metadata leaked into CPU session");
                } else {
                    if(!side || side->data[0].size()!=80 || side->data[1].size()!=8)throw std::runtime_error("toggle HDR metadata lost");
                    const BYTE index=BYTE((a+10000)/kDuration);
                    for(int i=2;i<5;++i)if(side->data[i].size()!=size_t(i==2?128:i==3?5225:9353) ||
                        !std::all_of(side->data[i].begin(),side->data[i].end(),[&](BYTE v){return v==index;}))
                        throw std::runtime_error("toggle Dolby metadata stale/missing");
                }
                if(!hardware && std::llabs(a-((a+kDuration/2)/kDuration)*kDuration)<20 && pixels!=expected)
                    throw std::runtime_error("toggle CPU 10-bit original changed");
                ++delivered;
            };
            const bool deferred_native=fmt.find(L"deferred")!=std::wstring::npos;
            const int cycles=deferred_native?12:3;
            for(int cycle=0;cycle<cycles;++cycle) {
                check(graph->Disconnect(&source->pin_),"toggle disconnect CPU");
                check(graph->Disconnect(in.Get()),"toggle disconnect CPU input");
                // A missing source header must not inherit the old HDR tag.
                recover_transfer=0;
                if(in->ReceiveConnection(&hw->pin(),&legacy)!=VFW_E_TYPE_NOT_ACCEPTED)
                    throw std::runtime_error("untagged reconnect without HDR evidence accepted");
                recover_transfer=16;
                const bool early_native=cycle==1 && fmt.find(L"early")!=std::wstring::npos;
                if(!early_native)check(graph->ConnectDirect(&hw->pin(),in.Get(),&legacy),"SW -> HW HDR input reconnect");
                if(deferred_native && !early_native) {
                    // QueryAccept(P010) can succeed before the decoder makes
                    // its native-surface choice. Keep the proposed NV12 media
                    // contract intact until that choice; actual GPU samples
                    // below are still P010. hdrsw.3 fails this assertion.
                    if(!reject_p010_media && hw->pin().QueryAccept(&cpu_media)!=S_OK)
                        throw std::runtime_error("deferred native fixture must also accept P010");
                    CMediaType actual;check(in->ConnectionMediaType(&actual),"deferred native input");
                    if(actual.subtype!=MEDIASUBTYPE_NV12)
                        throw std::runtime_error("pending native format was renegotiated before HW selection");
                }
                ComPtr<IMFGetService> services;check(in.As(&services),"toggle MF service");
                ComPtr<IMFDXGIDeviceManager> manager;check(services->GetService(MR_VIDEO_ACCELERATION_SERVICE,IID_PPV_ARGS(&manager)),"toggle manager");
                HANDLE handle=nullptr;check(manager->OpenDeviceHandle(&handle),"toggle device handle");
                ComPtr<ID3D11Device> device;const HRESULT locked=manager->LockDevice(handle,IID_PPV_ARGS(&device),TRUE);
                if(SUCCEEDED(locked))check(manager->UnlockDevice(handle,FALSE),"toggle unlock");
                check(manager->CloseDeviceHandle(handle),"toggle close handle");check(locked,"toggle device");
                ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
                ComPtr<IDirectXVideoMemoryConfiguration> configuration;
                check(services->GetService(MR_VIDEO_ACCELERATION_SERVICE,IID_PPV_ARGS(&configuration)),"toggle configuration");
                check(configuration->SetSurfaceType(DXVA2_SurfaceType_DecoderRenderTarget),"toggle HW activation");
                if(early_native) {
                    check(graph->ConnectDirect(&hw->pin(),in.Get(),&legacy),"early native HDR reconnect");
                    CMediaType actual;check(in->ConnectionMediaType(&actual),"early native input");
                    if(actual.subtype!=MEDIASUBTYPE_NV12)throw std::runtime_error("committed native format was renegotiated");
                }
                check(hw->pin().set_gpu_mode(true),"toggle GPU samples");check(hw->pin().renotify_allocator(),"toggle GPU allocator");
                hardware=true;size_t before=delivered;check(control->Run(),"toggle HW Run");
                for(int n=0;n<6;++n)check(hw->pin().push_gpu(n,device.Get(),context.Get(),n==0,true),"toggle HW frame");
                check(hw->pin().DeliverEndOfStream(),"toggle HW EOS");check(control->Stop(),"toggle HW Stop");
                if(delivered<=before+6)throw std::runtime_error("toggle HW interpolation absent");
                check(graph->Disconnect(&hw->pin()),"toggle disconnect HW");check(graph->Disconnect(in.Get()),"toggle disconnect HW input");
                const bool legacy_cpu=fmt.find(L"legacy")!=std::wstring::npos;
                if(legacy_cpu && cycle==0 && (fmt.find(L"deny")!=std::wstring::npos || fmt.find(L"custom")!=std::wstring::npos)) {
                    cpu_deny_p010=fmt.find(L"deny")!=std::wstring::npos;
                    const bool offered=cpu_offered_allocator;
                    if(fmt.find(L"custom")!=std::wstring::npos)cpu_offered_allocator=false;
                    check(graph->ConnectDirect(&source->pin_,in.Get(),&initial_media),"conservative CPU reconnect");
                    CMediaType untouched;check(in->ConnectionMediaType(&untouched),"conservative CPU input");
                    if(untouched.subtype!=MEDIASUBTYPE_NV12)
                        throw std::runtime_error("unknown allocator or declined P010 was forcibly reconnected");
                    check(graph->Disconnect(&source->pin_),"disconnect conservative CPU source");
                    check(graph->Disconnect(in.Get()),"disconnect conservative CPU input");
                    cpu_deny_p010=false;cpu_offered_allocator=offered;
                }
                check(graph->ConnectDirect(&source->pin_,in.Get(),legacy_cpu?&initial_media:&cpu_media),"HW -> SW HDR input reconnect");
                CMediaType cpu_actual;check(in->ConnectionMediaType(&cpu_actual),"toggle CPU actual input");
                if(cpu_actual.subtype!=nvof::p010_subtype)throw std::runtime_error("HW -> SW reconnect lost 10-bit decoder output");
                hardware=false;before=delivered;check(control->Run(),"toggle SW Run");
                for(int n=0;n<6;++n)check(source->pin_.push(frame,n,n==0),"toggle SW frame");
                check(source->pin_.DeliverEndOfStream(),"toggle SW EOS");check(control->Stop(),"toggle SW Stop");
                if(delivered<=before+6)throw std::runtime_error("toggle SW interpolation absent");
                CMediaType output;check(out->ConnectionMediaType(&output),"toggle retained output");
                if(output!=retained_output)throw std::runtime_error("toggle changed renderer media type");
            }
            inspect_cpu_output={};
            std::cout<<"PASS SW P010 HDR -> HW legacy NV12/P010 -> SW: "<<cycles<<" input-only cycles; retained renderer, interpolation, HDR/Dolby metadata, CPU precision\n";
        }
        std::cout<<"PASS CPU input -> P010 GPU: frames="<<frames<<" output="<<total<<" exact originals="<<exact<<" readback="<<!performance<<" seconds="<<elapsed<<" seek/restart=OK\n";
        }
        auto unload=reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(module,"DllCanUnloadNow"));if(unload()!=S_OK)throw std::runtime_error("COM leak");
        FreeLibrary(module);CoUninitialize();return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
#endif
