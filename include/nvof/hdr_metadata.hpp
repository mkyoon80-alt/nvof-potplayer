#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <memory>
#include <stdexcept>
namespace nvof {
// Public wire ABI only. Reference: LAVFilters/include/IMediaSideData.h.
// Independent storage implementation; no LAV filter or renderer dependency.
MIDL_INTERFACE("F940AE7F-48EB-4377-806C-8FC48CAB2292") MediaSideData : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetSideData(GUID,const BYTE*,size_t)=0;
    virtual HRESULT STDMETHODCALLTYPE GetSideData(GUID,const BYTE**,size_t*)=0;
};
inline const GUID hdr_mastering_id={0x53820dbc,0xa7b8,0x49c4,{0xb1,0x7b,0xe5,0x11,0x59,0x1a,0x79,0x0c}};
inline const GUID hdr_light_id={0xed6ae576,0x7cbe,0x41a6,{0x9d,0xc3,0x07,0xc3,0x5d,0xc1,0x3e,0xf9}};
inline int hdr_slot(const GUID& id) { return id==hdr_mastering_id?0:id==hdr_light_id?1:-1; }
inline size_t hdr_size(int slot) { return slot==0?80:8; }
struct HdrMetadata { std::array<std::vector<BYTE>,2> data; };
inline std::shared_ptr<const HdrMetadata> capture_hdr(IUnknown* sample) {
    Microsoft::WRL::ComPtr<MediaSideData> side;
    if(!sample || FAILED(sample->QueryInterface(IID_PPV_ARGS(&side))))return {};
    auto result=std::make_shared<HdrMetadata>();bool any=false;
    for(int i=0;i<2;++i) {
        const BYTE* data=nullptr;size_t size=0;
        const HRESULT hr=side->GetSideData(i==0?hdr_mastering_id:hdr_light_id,&data,&size);
        if(hr!=S_OK)continue; // Absence is common, especially in HLG.
        if(!data || size!=hdr_size(i))throw std::runtime_error("Malformed HDR static metadata");
        result->data[i].assign(data,data+size);any=true;
    }
    return any?result:nullptr;
}
}
