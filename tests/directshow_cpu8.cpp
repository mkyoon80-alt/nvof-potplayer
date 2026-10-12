// Exercise PotPlayer's input-only reconnect: the native renderer and its
// previously negotiated P010 output survive switching the decoder to software.
#define NVOF_CPU10_HELPERS_ONLY
#include "directshow_cpu10.cpp"
#include "nvof/mf_bridge.hpp"

void cpu8_case(IClassFactory* factory, bool reconnect, bool hardware_p010=true) {
    ComPtr<IBaseFilter> filter;
    check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&filter)),"create filter");
    ComPtr<IGraphBuilder> graph;
    check(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)),"graph");
    HRESULT hr=S_OK;
    auto* source=new Source(&hr);check(hr,"source");
    auto* sink=new Sink(&hr,true,false);check(hr,"sink");
    sink->pin().require_native_allocator();
    ComPtr<IBaseFilter> src=source,dst=sink;
    check(graph->AddFilter(src.Get(),L"Decoder"),"add source");
    check(graph->AddFilter(filter.Get(),L"NVOF"),"add filter");
    check(graph->AddFilter(dst.Get(),L"Renderer"),"add sink");
    ComPtr<IPin> in,out;
    check(filter->FindPin(L"In",&in),"input");check(filter->FindPin(L"Out",&out),"output");
    CMediaType media=input_type();
    check(graph->ConnectDirect(&source->pin(),in.Get(),&media),"connect input");
    ComPtr<IMediaControl> control;check(graph.As(&control),"control");
    ComPtr<nvof::transport::DecoderConfiguration> native;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if(reconnect) {
        // Do not assume adapter zero is NVIDIA on a hybrid system.
        nvof::MfD3d11Bridge bridge;
        device=bridge.device();context=bridge.context();
        ComPtr<ID3D10Multithread> protection;check(context.As(&protection),"context protection");
        protection->SetMultithreadProtected(TRUE);
        check(in.As(&native),"native contract");
        check(native->ActivateD3D11Decoding(device.Get(),context.Get(),nullptr,0),"native activate");
    }
    check(graph->ConnectDirect(out.Get(),&sink->pin(),nullptr),"connect renderer");
    for(int cycle=0;cycle<(reconnect?3:1);++cycle) {
        if(reconnect) {
            p010_fixture=hardware_p010;
            if(cycle)check(native->ActivateD3D11Decoding(device.Get(),context.Get(),nullptr,0),"return to HW");
            check(source->pin().set_gpu_mode(true),"HW samples");
            check(control->Run(),"HW Run");
            check(source->pin().push_gpu(0,device.Get(),context.Get(),true),"HW original");
            check(source->pin().push_gpu(1,device.Get(),context.Get()),"HW interpolation");
            check(control->Stop(),"HW Stop");
            CMediaType retained;check(out->ConnectionMediaType(&retained),"retained type");
            if(retained.subtype!=(hardware_p010?nvof::p010_subtype:MEDIASUBTYPE_NV12))
                throw std::runtime_error("fixture did not establish requested HW format");
            check(graph->Disconnect(&source->pin()),"disconnect decoder");
            check(graph->Disconnect(in.Get()),"disconnect input only");
            check(graph->ConnectDirect(&source->pin(),in.Get(),&media),"reconnect CPU decoder");
        }
        p010_fixture=false;
        check(source->pin().set_gpu_mode(false),"CPU samples");
        const size_t before=sink->pin().gpu_count();
        check(control->Run(),"SW Run");
        check(source->pin().push(0,true),"SW first original with retained renderer");
        for(int n=1;n<12;++n)check(source->pin().push(n),"SW interpolation");
        check(control->Pause(),"pause");check(control->Run(),"resume");
        check(source->pin().DeliverBeginFlush(),"seek flush");
        check(source->pin().DeliverEndFlush(),"seek end flush");
        check(source->pin().DeliverNewSegment(0,INT64_MAX,1),"seek segment");
        check(source->pin().push(1,true),"seek first");check(source->pin().push(2),"seek next");
        check(source->pin().DeliverEndOfStream(),"EOS");
        check(control->Stop(),"SW Stop");
        check(control->Run(),"SW restart");
        check(source->pin().push(0,true),"restart first");check(source->pin().push(1),"restart next");
        check(control->Stop(),"restart Stop");
        if(sink->pin().cpu_count() || sink->pin().gpu_count()<=before+16 || !sink->pin().valid)
            throw std::runtime_error("CPU NV12 must interpolate and deliver GPU surfaces with valid timestamps");
    }
    std::cout<<"PASS "<<(reconnect?(hardware_p010?"P010 HW -> CPU NV12 -> HW (same renderer, 3 cycles)":
        "NV12 HW -> CPU NV12 -> HW (same renderer, 3 cycles)"):"CPU NV12 cold start")
        <<(color_fixture==1?" (VideoInfo)":" (VideoInfo2)")
        <<"; GPU output, format, seek, pause, restart, timestamps\n";
}
std::vector<uint8_t> read_nv12(IMediaSample* sample,ID3D11Device* device,ID3D11DeviceContext* context) {
    ComPtr<nvof::transport::TextureSample> ts;check(sample->QueryInterface(IID_PPV_ARGS(&ts)),"NV12 GPU sample");
    ComPtr<ID3D11Texture2D> texture,staging;UINT slice=0;
    check(ts->GetD3D11Texture(0,&texture,&slice),"NV12 texture");
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if(desc.Format!=DXGI_FORMAT_NV12 || slice)throw std::runtime_error("Expected NV12 snapshot");
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.BindFlags=desc.MiscFlags=0;
    check(device->CreateTexture2D(&desc,nullptr,&staging),"NV12 staging");
    context->CopyResource(staging.Get(),texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"NV12 readback");
    std::vector<uint8_t> pixels(size_t(kWidth)*kHeight*3/2);
    for(int row=0;row<kHeight*3/2;++row)
        memcpy(pixels.data()+size_t(row)*kWidth,static_cast<const BYTE*>(mapped.pData)+size_t(row)*mapped.RowPitch,kWidth);
    context->Unmap(staging.Get(),0);return pixels;
}

void cpu8_pixels(IClassFactory* factory,const std::filesystem::path& raw={}) {
    const bool real=!raw.empty();const int pitch=kWidth+64;
    p010_fixture=p010_media=false;cpu_media=input_type();
    auto* h=reinterpret_cast<VIDEOINFOHEADER2*>(cpu_media.pbFormat);
    h->bmiHeader.biWidth=pitch;h->bmiHeader.biSizeImage=pitch*kHeight*3/2;
    cpu_media.SetSampleSize(h->bmiHeader.biSizeImage);
    ComPtr<IBaseFilter> filter;check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&filter)),"pixel filter");
    ComPtr<IGraphBuilder> graph;check(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)),"pixel graph");
    HRESULT hr=S_OK;auto* source=new CpuSource(&hr);check(hr,"pixel source");
    auto* sink=new Sink(&hr,true,false);check(hr,"pixel sink");
    sink->pin().require_native_allocator();
    ComPtr<IBaseFilter> src=source,dst=sink;
    check(graph->AddFilter(src.Get(),L"Software decoder"),"pixel add source");
    check(graph->AddFilter(filter.Get(),L"NVOF"),"pixel add filter");
    check(graph->AddFilter(dst.Get(),L"Renderer"),"pixel add sink");
    ComPtr<IPin> in,out;check(filter->FindPin(L"In",&in),"pixel input");check(filter->FindPin(L"Out",&out),"pixel output");
    check(graph->ConnectDirect(&source->pin_,in.Get(),&cpu_media),"padded input");
    check(graph->ConnectDirect(out.Get(),&sink->pin(),nullptr),"pixel renderer");
    ComPtr<IMediaControl> control;check(graph.As(&control),"pixel control");check(control->Run(),"pixel Run");
    std::vector<uint8_t> expected(size_t(kWidth)*kHeight*3/2),bytes(size_t(pitch)*kHeight*3/2,0xe7),held_pixels;
    ComPtr<IMediaSample> held;ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    size_t exact=0;REFERENCE_TIME source_pts=0;
    inspect_gpu_output=[&](IMediaSample* s,ID3D11Device* d,ID3D11DeviceContext* c) {
        REFERENCE_TIME a,b;check(s->GetTime(&a,&b),"pixel PTS");
        if(!held){held=s;device=d;context=c;held_pixels=read_nv12(s,d,c);}
        if(std::llabs(a-source_pts)<20) {
            if(read_nv12(s,d,c)!=expected)throw std::runtime_error("NV12 original changed (pitch/UV/precision)");
            ++exact;
        }
    };
    std::ifstream input;if(real){input.open(raw,std::ios::binary);if(!input)throw std::runtime_error("raw input unavailable");}
    const int count=real?48:12;
    for(int i=0;i<count;++i) {
        if(real){input.read(reinterpret_cast<char*>(expected.data()),expected.size());if(input.gcount()!=std::streamsize(expected.size()))throw std::runtime_error("short raw input");}
        else for(size_t j=0;j<expected.size();++j)expected[j]=uint8_t((j+i*11)%256);
        for(int row=0;row<kHeight*3/2;++row)memcpy(bytes.data()+size_t(row)*pitch,expected.data()+size_t(row)*kWidth,kWidth);
        source_pts=i*kDuration;check(source->pin_.push(bytes,i,i==0),"padded CPU NV12");
    }
    check(source->pin_.DeliverEndOfStream(),"pixel EOS");
    if(exact!=size_t(count))throw std::runtime_error("Missing bit-exact originals");
    if(!held || read_nv12(held.Get(),device.Get(),context.Get())!=held_pixels)throw std::runtime_error("Paused snapshot overwritten");
    held.Reset();inspect_gpu_output={};
    check(control->Stop(),"pixel Stop");check(control->Run(),"truncated Run");
    bytes.pop_back();
    if(source->pin_.push(bytes,0,true)!=VFW_E_BUFFER_OVERFLOW)throw std::runtime_error("Truncated NV12 buffer accepted");
    check(control->Stop(),"truncated Stop");
    std::cout<<"PASS "<<(real?"decoded source":"synthetic")<<" CPU NV12: "<<count<<" bit-exact originals, padded pitch/UV, retained snapshot, truncated buffer rejected\n";
}

int wmain(int argc,wchar_t** argv) {
    if(argc<2)return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
    int result=0;
    HMODULE module=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!module)return 2;
    try {
        auto get=reinterpret_cast<HRESULT(WINAPI*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));
        ComPtr<IClassFactory> factory;check(get(kFilter,IID_PPV_ARGS(&factory)),"factory");
        if(argc==3 && std::wstring(argv[2])==L"--adapter-nv12-regression") {
            renderer_adapter_hint=0xfffffffeu;
            cpu8_case(factory.Get(),true,false);
            renderer_adapter_hint=0;
        } else if(argc==3 && std::wstring(argv[2])==L"--adapter-hint-regression") {
            ComPtr<IDXGIFactory1> dxgi;check(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi)),"DXGI");
            UINT non_nvidia=UINT_MAX;
            for(UINT i=0;;++i) {
                ComPtr<IDXGIAdapter1> adapter;const HRESULT next=dxgi->EnumAdapters1(i,&adapter);
                if(next==DXGI_ERROR_NOT_FOUND)break;
                check(next,"adapter");DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"adapter description");
                if(desc.VendorId!=0x10de || (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {non_nvidia=i;break;}
            }
            if(non_nvidia==UINT_MAX)throw std::runtime_error("Regression needs a non-NVIDIA or software adapter");
            renderer_adapter_hint=non_nvidia;
            std::cout<<"Renderer advertises non-NVIDIA adapter index "<<non_nvidia<<'\n';
            cpu8_case(factory.Get(),true);
            cpu8_case(factory.Get(),false);
            color_fixture=1;cpu8_case(factory.Get(),false);color_fixture=0;
            // Also check stale/out-of-range renderer hints without ever using
            // that index to construct a device on the corrected path.
            renderer_adapter_hint=0xfffffffeu;
            cpu8_case(factory.Get(),true);
            cpu8_case(factory.Get(),false);
            renderer_adapter_hint=0;
        } else if(argc>4) {
            kWidth=_wtoi(argv[3]);kHeight=_wtoi(argv[4]);kDuration=420000;
            cpu8_pixels(factory.Get(),argv[2]);
        } else {
            cpu8_case(factory.Get(),true);
            cpu8_case(factory.Get(),false);
            color_fixture=1; // PotPlayer's SW decoder negotiates legacy VideoInfo.
            cpu8_case(factory.Get(),false);
            color_fixture=0;
            cpu8_pixels(factory.Get());
        }
    } catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';result=1;}
    auto unload=reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(module,"DllCanUnloadNow"));
    if(unload && unload()==S_OK)FreeLibrary(module);else{std::cerr<<"live COM references\n";result=1;}
    CoUninitialize();return result;
}
