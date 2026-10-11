#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

namespace nvof {
struct Vp9Color { unsigned primaries=2, transfer=2, matrix=2; bool full_range=false; };

// Bounded VPCodecConfigurationRecord reader, independently implemented from
// https://www.webmproject.org/vp9/mp4/ . VP9 picture headers do not identify
// PQ versus HLG; never infer either from profile, bit depth or BT.2020 alone.
// DirectShow splitters also use a 'vpcC' + FullBox payload (without box size).
inline std::optional<Vp9Color> vp9_color(const uint8_t* data,size_t size) noexcept {
    if(!data || size<8 || size>65536)return {};
    const auto tag=[](const uint8_t* p){return p[0]=='v' && p[1]=='p' && p[2]=='c' && p[3]=='C';};
    size_t record=0;
    if(size>=20 && tag(data+4)) {
        const uint32_t box_size=(uint32_t(data[0])<<24)|(uint32_t(data[1])<<16)|(uint32_t(data[2])<<8)|data[3];
        if(box_size!=size)return {};
        record=12;
    } else if(size>=16 && tag(data))record=8;
    else if(size==12)record=4; // FullBox payload, including version/flags.
    else if(size!=8)return {};
    if(record && (data[record-4]!=1 || data[record-3] || data[record-2] || data[record-1]))return {};
    if(size-record!=8)return {}; // VP9 codecInitializationDataSize must be zero.
    const auto* p=data+record;
    const unsigned profile=p[0],depth=p[2]>>4,chroma=(p[2]>>1)&7;
    if(profile>3 || (profile<2?depth!=8:(depth!=10 && depth!=12)) || chroma>3 ||
       ((profile&1)?chroma<2:chroma>1) || (p[5]==0 && chroma!=3) || p[6] || p[7])return {};
    switch(p[1]) {
        case 0: case 10: case 11: case 20: case 21: case 30: case 31:
        case 40: case 41: case 50: case 51: case 52: case 60: case 61: case 62: break;
        default:return {};
    }
    return Vp9Color{p[3],p[4],p[5],bool(p[2]&1)};
}
}
