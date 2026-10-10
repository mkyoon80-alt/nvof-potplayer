#pragma once
#include <streams.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <vector>
#include "nvof/gpu_engine.hpp"

namespace nvof::transport {
// ABI-compatible declarations of the public LAV/MPC renderer interfaces.
// GUIDs/signatures: Nevcairiel/LAVFilters/include/ID3DVideoMemoryConfiguration.h
MIDL_INTERFACE("2BB66002-46B7-4F13-9036-7053328742BE") DecoderConfiguration : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE ActivateD3D11Decoding(ID3D11Device*,ID3D11DeviceContext*,HANDLE,UINT)=0;
    virtual UINT STDMETHODCALLTYPE GetD3D11AdapterIndex()=0;
};
MIDL_INTERFACE("1F084C92-2A5A-472C-BFA9-D30E40B80C00") TextureConfiguration : public IUnknown {
    virtual UINT STDMETHODCALLTYPE GetD3D11TextureBindFlags()=0;
    virtual UINT STDMETHODCALLTYPE GetD3D11TextureMiscFlags()=0;
};
MIDL_INTERFACE("BC8753F5-0AC8-4806-8E5F-A12B2AFE153E") TextureSample : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetD3D11Texture(int,ID3D11Texture2D**,UINT*)=0;
};

class SurfaceSample final : public CMediaSample, public TextureSample, public MediaSideData {
public:
    SurfaceSample(CBaseAllocator* owner,HRESULT* result,LONG size,LONG alignment,LONG prefix,bool gpu)
        : CMediaSample(NAME("NV12 CPU or D3D11 sample"),owner,result,nullptr,size),
          alignment_(alignment),prefix_(prefix),gpu_(gpu) {
            *result=S_OK;
            // IMediaSample2::GetProperties exposes m_pBuffer without invoking
            // GetPointer. CPU transport must already have a valid host buffer.
            if(!gpu_) {BYTE* data=nullptr;*result=GetPointer(&data);}
        }
    STDMETHODIMP QueryInterface(REFIID id,void** value) override {
        if(!value)return E_POINTER;
        if(id==__uuidof(TextureSample) && gpu_)
            return GetInterface(static_cast<TextureSample*>(this),value);
        if(id==__uuidof(MediaSideData))return GetInterface(static_cast<MediaSideData*>(this),value);
        return CMediaSample::QueryInterface(id,value);
    }
    STDMETHODIMP_(ULONG) AddRef() override {return CMediaSample::AddRef();}
    STDMETHODIMP_(ULONG) Release() override {return CMediaSample::Release();}
    STDMETHODIMP GetD3D11Texture(int view,ID3D11Texture2D** texture,UINT* slice) override {
        if(!texture || !slice)return E_POINTER;
        *texture=nullptr;*slice=0;
        if(view!=0 || !frame_.texture)return E_INVALIDARG;
        *texture=frame_.texture.Get();(*texture)->AddRef();*slice=frame_.array_slice;
        return S_OK;
    }
    STDMETHODIMP GetSideData(GUID id,const BYTE** data,size_t* size) override {
        if(!data || !size)return E_POINTER;
        *data=nullptr;*size=0;const int slot=hdr_slot(id);
        if(slot<0 || !frame_.hdr || frame_.hdr->data[slot].empty())return E_FAIL;
        *data=frame_.hdr->data[slot].data();*size=frame_.hdr->data[slot].size();return S_OK;
    }
    STDMETHODIMP SetSideData(GUID id,const BYTE* data,size_t size) override {
        const int slot=hdr_slot(id);
        if(slot<0)return E_INVALIDARG;
        if(!data)return E_POINTER;
        if(size!=hdr_size(slot))return E_INVALIDARG;
        if(hdr_history_.size()>=16)return E_OUTOFMEMORY;
        try {
            auto next=frame_.hdr?std::make_shared<HdrMetadata>(*frame_.hdr):std::make_shared<HdrMetadata>();
            next->data[slot].assign(data,data+size);
            if(frame_.hdr)hdr_history_.push_back(frame_.hdr); // Keep earlier returned pointers alive.
            frame_.hdr=std::move(next);return S_OK;
        } catch(...) {return E_OUTOFMEMORY;}
    }
    STDMETHODIMP GetPointer(BYTE** data) override {
        if(!data)return E_POINTER;
        *data=nullptr;
        if(gpu_)return E_NOTIMPL; // Never perform an implicit GPU readback.
        try {
            if(host_.empty()) {
                host_.resize(size_t(m_cbBuffer)+size_t(prefix_)+size_t(alignment_));
                const uintptr_t first=reinterpret_cast<uintptr_t>(host_.data())+size_t(prefix_);
                m_pBuffer=reinterpret_cast<BYTE*>((first+alignment_-1)&~(uintptr_t(alignment_)-1));
            }
            *data=m_pBuffer;
            return S_OK;
        } catch (...) {return E_OUTOFMEMORY;}
    }
    void assign(nvof::GpuFrame frame) {frame_=std::move(frame);m_pBuffer=nullptr;}
    void clear_surface() noexcept {
        frame_={};hdr_history_.clear();
        if(!host_.empty()) {
            const uintptr_t first=reinterpret_cast<uintptr_t>(host_.data())+size_t(prefix_);
            m_pBuffer=reinterpret_cast<BYTE*>((first+alignment_-1)&~(uintptr_t(alignment_)-1));
        }
    }
private:
    nvof::GpuFrame frame_;
    std::vector<std::shared_ptr<const HdrMetadata>> hdr_history_;
    LONG alignment_=1,prefix_=0;
    const bool gpu_;
    std::vector<uint8_t> host_; // Allocated only for a CPU transport sample.
};

class SurfaceAllocator final : public CBaseAllocator {
public:
    explicit SurfaceAllocator(HRESULT* result,bool gpu=false)
        : CBaseAllocator(NAME("Bounded NV12 texture samples"),nullptr,result),gpu_(gpu) { }
    ~SurfaceAllocator() override {Free();}
    HRESULT set_gpu_mode(bool gpu) {
        CAutoLock lock(this);
        if(gpu==gpu_)return S_OK;
        if(m_bCommitted)return VFW_E_ALREADY_COMMITTED;
        if(m_lAllocated!=m_lFree.GetCount())return VFW_E_BUFFERS_OUTSTANDING;
        Free();gpu_=gpu;m_bChanged=TRUE;return S_OK;
    }
    STDMETHODIMP SetProperties(ALLOCATOR_PROPERTIES* requested,ALLOCATOR_PROPERTIES* actual) override {
        if(!requested || !actual)return E_POINTER;
        CAutoLock lock(this);
        *actual={};
        if(m_bCommitted)return VFW_E_ALREADY_COMMITTED;
        if(m_lAllocated!=m_lFree.GetCount())return VFW_E_BUFFERS_OUTSTANDING;
        if(requested->cbBuffer<=0 || requested->cBuffers<=0 || requested->cBuffers>32 ||
            requested->cbAlign<=0 || requested->cbAlign>65536 ||
            (requested->cbAlign&(requested->cbAlign-1)) || requested->cbPrefix<0 || requested->cbPrefix>65536)
            return E_INVALIDARG;
        m_lSize=requested->cbBuffer;m_lCount=requested->cBuffers;
        m_lAlignment=requested->cbAlign;m_lPrefix=requested->cbPrefix;
        *actual=*requested;m_bChanged=TRUE;
        return S_OK;
    }
    STDMETHODIMP ReleaseBuffer(IMediaSample* sample) override {
        if(!sample)return E_POINTER;
        // Final sample Release is the only caller: no consumer can still read
        // the texture when it is dropped and the slot becomes available again.
        static_cast<SurfaceSample*>(static_cast<CMediaSample*>(sample))->clear_surface();
        return CBaseAllocator::ReleaseBuffer(sample);
    }
protected:
    HRESULT Alloc() override {
        CAutoLock lock(this);
        const HRESULT checked=CBaseAllocator::Alloc();
        if(FAILED(checked))return checked;
        if(checked==S_FALSE)return S_OK;
        Free();
        HRESULT result=S_OK;
        for(int i=0;i<m_lCount;++i) {
            auto* sample=new(std::nothrow) SurfaceSample(this,&result,m_lSize,m_lAlignment,m_lPrefix,gpu_);
            if(!sample || FAILED(result)){delete sample;Free();return E_OUTOFMEMORY;}
            m_lFree.Add(sample);++m_lAllocated;
        }
        m_bChanged=FALSE;
        return S_OK;
    }
    void Free() override {
        while(auto* sample=m_lFree.RemoveHead())delete sample;
        m_lAllocated=0;m_bChanged=TRUE;
    }
private:
    bool gpu_=false;
};

class OutputPin final : public CTransformOutputPin {
public:
    OutputPin(CTransformFilter* owner,HRESULT* result)
        : CTransformOutputPin(NAME("NV12 CPU or D3D11 output"),owner,result,L"Output") { }
    bool surface_allocator() const noexcept {return surface_allocator_;}
    HRESULT set_gpu_mode(bool gpu) {
        if(gpu==gpu_ && (!m_pAllocator || surface_allocator_==gpu))return S_OK;
        if(m_pAllocator) {
            if(!m_pFilter->IsStopped())return VFW_E_NOT_STOPPED;
            const bool previous=gpu_,previous_surface=surface_allocator_;
            gpu_=gpu;
            Microsoft::WRL::ComPtr<IMemAllocator> replacement;
            const HRESULT result=DecideAllocator(m_pInputPin,&replacement);
            if(FAILED(result)){gpu_=previous;surface_allocator_=previous_surface;return result;}
            m_pAllocator->Decommit();m_pAllocator->Release();m_pAllocator=replacement.Detach();
        } else gpu_=gpu;
        return S_OK;
    }
    HRESULT DecideAllocator(IMemInputPin* receiver,IMemAllocator** allocator) override {
        if(!receiver || !allocator)return E_POINTER;
        *allocator=nullptr;
        // Preserve the original CPU renderer path until native activation.
        // Merely exposing a native configuration interface is insufficient.
        if(!gpu_) {
            if(!surface_allocator_)return CTransformOutputPin::DecideAllocator(receiver,allocator);
            // On native rejection/reconnection the renderer may offer back the
            // GPU allocator we previously notified. Install a fresh standard
            // allocator explicitly instead of accidentally reusing that pool.
            ALLOCATOR_PROPERTIES properties{};receiver->GetAllocatorRequirements(&properties);
            if(properties.cbAlign<=0)properties.cbAlign=1;
            Microsoft::WRL::ComPtr<IMemAllocator> ordinary;
            HRESULT result=InitAllocator(&ordinary);
            if(SUCCEEDED(result))result=DecideBufferSize(ordinary.Get(),&properties);
            if(SUCCEEDED(result))result=receiver->NotifyAllocator(ordinary.Get(),FALSE);
            if(FAILED(result))return result;
            *allocator=ordinary.Detach();surface_allocator_=false;return S_OK;
        }
        Microsoft::WRL::ComPtr<DecoderConfiguration> native;
        if(FAILED(receiver->QueryInterface(IID_PPV_ARGS(&native))))return E_NOINTERFACE;
        ALLOCATOR_PROPERTIES properties{};
        receiver->GetAllocatorRequirements(&properties);
        if(properties.cbAlign<=0)properties.cbAlign=1;
        HRESULT result=S_OK;
        auto* raw=new(std::nothrow) SurfaceAllocator(&result,gpu_);
        if(!raw)return E_OUTOFMEMORY;
        if(FAILED(result)){delete raw;return result;}
        Microsoft::WRL::ComPtr<IMemAllocator> candidate;
        result=raw->QueryInterface(IID_PPV_ARGS(&candidate));
        if(FAILED(result)){delete raw;return result;}
        result=DecideBufferSize(candidate.Get(),&properties);
        if(SUCCEEDED(result))result=receiver->NotifyAllocator(candidate.Get(),FALSE);
        if(FAILED(result))return result;
        *allocator=candidate.Detach();surface_allocator_=true;
        return S_OK;
    }
    void cancel_allocation_waits() noexcept {if(m_pAllocator)m_pAllocator->Decommit();}
    HRESULT resume_allocation() {return m_pAllocator ? m_pAllocator->Commit() : VFW_E_NO_ALLOCATOR;}
private:
    bool surface_allocator_=false;
    bool gpu_=false;
};
} // namespace nvof::transport
