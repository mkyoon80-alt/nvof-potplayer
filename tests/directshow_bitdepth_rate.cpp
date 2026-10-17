#define NVOF_CPU10_HELPERS_ONLY
#include "directshow_cpu10.cpp"
#include "hevc_scan_fixture.hpp"
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
#include "frame_hash.hpp"

// Public DirectShow interfaces against the actual filter DLL. No registration
// or player settings are used; the caller supplies a staged filter and INI.
int wmain(int argc,wchar_t** argv) {try {
    require(argc>=4,"directshow_bitdepth_rate filter.ax case output.csv [raw.p210]");
    const std::wstring kind=argv[2];const bool real=argc==5;
    const bool avc=kind==L"avc-real";
    const bool av1=kind.find(L"av1-")==0;
    const bool fields=kind==L"fields" || kind==L"fields-bff" || kind==L"mixed" || avc || kind.find(L"av1-fields")==0;
    const bool rate48=kind==L"plain48" || kind==L"progressive48";
    kWidth=real?1920:640;kHeight=real?1080:360;kDuration=fields?400000:rate48?208542:417083;
    if(av1){cpu_av1_case=kind;kDuration=166667;}
    p010_fixture=true;p010_media=true;
    cpu_weave=!fields;cpu_mixed=kind==L"mixed";
    cpu_avc_sps=avc;cpu_avc_interlaced=avc;
    if(!avc && !av1) {
        using namespace hevc_scan_fixture;
        Scan scan;
        if(fields)scan={false,true,false,true};
        if(kind==L"unknown")scan={false,false,false,false};
        if(kind==L"mixed-sps")scan={true,true,false,false};
        const auto nal=sps(scan);
        cpu_hevc_sequence=kind==L"annex"?annex({nal}):hvcc({nal});
        if(kind==L"conflict")cpu_hevc_sequence=hvcc({sps(),sps({false,true,false,true})});
        if(kind==L"truncated")cpu_hevc_sequence.resize(cpu_hevc_sequence.size()-3);
        if(kind==L"contradiction")cpu_hevc_sequence=hvcc({sps({true,false,true,true})});
    }
    cpu_media=input_type();auto* h=reinterpret_cast<VIDEOINFOHEADER2*>(cpu_media.pbFormat);
    const int pitch=kWidth*2;const size_t bytes=size_t(pitch)*kHeight*(real?2:3)/ (real?1:2);
    if(real){cpu_media.SetSubtype(&nvof::p210_subtype);h->bmiHeader.biCompression=nvof::p210_subtype.Data1;h->bmiHeader.biBitCount=32;h->bmiHeader.biSizeImage=DWORD(bytes);cpu_media.SetSampleSize(ULONG(bytes));}
    h->dwInterlaceFlags=(kind==L"plain" || kind==L"plain48" || kind==L"av1-plain")?0:AMINTERLACE_IsInterlaced|(kind==L"fields-bff" || kind==L"av1-fields-bff"?0:AMINTERLACE_Field1First);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
    HMODULE module=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);require(module!=nullptr,"load filter");
    auto get=reinterpret_cast<HRESULT(WINAPI*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));require(get!=nullptr,"factory export");
    {
    ComPtr<IClassFactory> factory;check(get(kFilter,IID_PPV_ARGS(&factory)),"factory");
    ComPtr<IBaseFilter> filter;check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&filter)),"filter");
    ComPtr<IGraphBuilder> graph;check(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)),"graph");
    HRESULT hr=S_OK;auto* source=new CpuSource(&hr);check(hr,"source");auto* sink=new InspectSink(&hr);check(hr,"sink");
    ComPtr<IBaseFilter> src=source,dst=sink;
    check(graph->AddFilter(src.Get(),L"Test decoder"),"add source");check(graph->AddFilter(filter.Get(),L"NVOF"),"add filter");check(graph->AddFilter(dst.Get(),L"D3D11 renderer"),"add sink");
    ComPtr<IPin> in,out;check(filter->FindPin(L"In",&in),"input");check(filter->FindPin(L"Out",&out),"output");
    check(graph->ConnectDirect(&source->pin_,in.Get(),&cpu_media),"input connection");
    check(graph->ConnectDirect(out.Get(),&sink->pin,nullptr),"renderer connection");
    CMediaType mt;check(out->ConnectionMediaType(&mt),"output media");
    const auto duration=reinterpret_cast<VIDEOINFOHEADER2*>(mt.pbFormat)->AvgTimePerFrame;
    std::cout<<"NEGOTIATED outputFps="<<std::setprecision(10)<<10000000.0/duration<<std::endl;
    std::ofstream report(argv[3]);require(bool(report),"report");report<<"index,pts,stop,sha256\n";
    size_t count=0,exact=0;
    inspect_cpu_output=[&](IMediaSample* sample,ID3D11Device* d,ID3D11DeviceContext* c) {
        const auto pixels=read_output(sample,d,c);REFERENCE_TIME a,b;check(sample->GetTime(&a,&b),"PTS");
        for(auto value:pixels)require((value&63)==0,"P010 precision alignment");
        if(!fields && std::llabs(a-((a+kDuration/2)/kDuration)*kDuration)<20) {
            // The adjacent original may be delivered on the following push;
            // compare full output hashes against the untagged control below.
            ++exact;
        }
        std::vector<uint8_t> raw(reinterpret_cast<const uint8_t*>(pixels.data()),reinterpret_cast<const uint8_t*>(pixels.data()+pixels.size()));
        report<<count++<<','<<a<<','<<b<<','<<hash_bytes(raw)<<'\n';
    };
    ComPtr<IMediaControl> control;check(graph.As(&control),"control");check(control->Run(),"run");
    Sleep(150); // Isolate stable cadence; startup/seek windows have separate tests.
    std::ifstream raw;if(real){raw.open(std::filesystem::path(argv[4]),std::ios::binary);require(bool(raw),"real decoded frames");}
    std::vector<uint8_t> frame(bytes);unsigned frames=0;
    for(unsigned index=0;index<24;++index) {
        if(real){raw.read(reinterpret_cast<char*>(frame.data()),frame.size());require(raw.gcount()==std::streamsize(frame.size()),"real fixture too short");}
        else {
            auto* words=reinterpret_cast<uint16_t*>(frame.data());
            for(int y=0;y<kHeight;++y)for(int x=0;x<kWidth;++x)
                words[size_t(y)*kWidth+x]=uint16_t((64+(x+int(index)*12+(fields?(y%2)*6:0))%876)<<6);
            for(size_t i=size_t(kWidth)*kHeight;i<frame.size()/2;++i)words[i]=uint16_t((i%2?650:350)<<6);
        }
        check(source->pin_.push(frame,index,index==0),"frame");++frames;
    }
    check(source->pin_.DeliverEndOfStream(),"EOS");
    // Nominal NTSC timestamps recover the exact rational clock rather than
    // accumulating the rounded 100-ns duration (8 ticks over this fixture).
    require(count>=frames && !sink->pin.ends.empty() && std::llabs(sink->pin.ends.back()-int64_t(frames)*kDuration)<=frames,"output count/end time");
    report.flush();
    std::cout<<"RENDER frames="<<frames<<" output="<<count<<" originalSlots="<<exact<<std::endl;
    check(source->pin_.DeliverBeginFlush(),"seek begin");check(source->pin_.DeliverEndFlush(),"seek end");check(source->pin_.DeliverNewSegment(0,INT64_MAX,1),"seek segment");
    check(source->pin_.push(frame,0,true),"seek first frame");
    check(control->Stop(),"stop");check(control->Run(),"restart");check(source->pin_.push(frame,0,true),"restart first frame");check(control->Stop(),"final stop");
    inspect_cpu_output={};
    std::cout<<"PASS P010 output, contiguous timestamps, seek, restart\n";
    }
    FreeLibrary(module);CoUninitialize();return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
