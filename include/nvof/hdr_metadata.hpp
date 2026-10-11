#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <memory>
#include <stdexcept>
#include <atomic>
namespace nvof {
// Public wire ABI only. Reference: LAVFilters/include/IMediaSideData.h.
// Independent storage implementation; no LAV filter or renderer dependency.
MIDL_INTERFACE("F940AE7F-48EB-4377-806C-8FC48CAB2292") MediaSideData : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetSideData(GUID,const BYTE*,size_t)=0;
    virtual HRESULT STDMETHODCALLTYPE GetSideData(GUID,const BYTE**,size_t*)=0;
};
inline const GUID hdr_mastering_id={0x53820dbc,0xa7b8,0x49c4,{0xb1,0x7b,0xe5,0x11,0x59,0x1a,0x79,0x0c}};
inline const GUID hdr_light_id={0xed6ae576,0x7cbe,0x41a6,{0x9d,0xc3,0x07,0xc3,0x5d,0xc1,0x3e,0xf9}};
inline const GUID dovi_rpu_id={0xbae40e6c,0x5b93,0x4170,{0x90,0xcc,0x5d,0x5f,0x02,0xa2,0x96,0x38}};
inline const GUID dovi_metadata_id={0x277ee779,0x13f4,0x434e,{0xbd,0xec,0x3d,0x6f,0x8c,0x0e,0x15,0xd2}};
inline const GUID dovi_metadata_v2_id={0xf1949f1c,0x7474,0x4f0f,{0xb1,0x10,0xfb,0x73,0x4c,0x08,0x85,0x1d}};
inline const std::array<GUID,5> hdr_ids={hdr_mastering_id,hdr_light_id,dovi_rpu_id,dovi_metadata_id,dovi_metadata_v2_id};
inline int hdr_slot(const GUID& id) {
    for(size_t i=0;i<hdr_ids.size();++i)if(id==hdr_ids[i])return static_cast<int>(i);
    return -1;
}
inline bool valid_hdr_size(int slot,size_t size) {
    if(slot==0)return size==80;
    if(slot==1)return size==8;
    // Opaque, independently owned payloads. Do not reinterpret or manufacture
    // Dolby metadata. The public packed ABI permits trailing future fields.
    if(slot==2)return size>0 && size<=65536;
    if(slot==3)return size>=5225 && size<=65536;
    if(slot==4)return size>=9353 && size<=65536;
    return false;
}
struct HdrMetadata { std::array<std::vector<BYTE>,5> data; };
// Shared with outstanding samples, so asynchronous renderer reads cannot
// reference a destroyed filter. Counters describe transport, not DV rendering.
struct HdrDeliveryTrace {
    std::atomic<unsigned> input_mask{0},requested_mask{0},returned_mask{0};
};
inline std::shared_ptr<const HdrMetadata> capture_hdr(IUnknown* sample) {
    Microsoft::WRL::ComPtr<MediaSideData> side;
    if(!sample || FAILED(sample->QueryInterface(IID_PPV_ARGS(&side))))return {};
    auto result=std::make_shared<HdrMetadata>();bool any=false;
    for(int i=0;i<static_cast<int>(hdr_ids.size());++i) {
        const BYTE* data=nullptr;size_t size=0;
        const HRESULT hr=side->GetSideData(hdr_ids[i],&data,&size);
        if(hr!=S_OK)continue; // Absence is common, especially in HLG.
        if(!data || !valid_hdr_size(i,size))throw std::runtime_error("Malformed HDR side metadata");
        result->data[i].assign(data,data+size);any=true;
    }
    return any?result:nullptr;
}
}
