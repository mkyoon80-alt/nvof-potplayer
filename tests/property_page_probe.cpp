#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>
#include <iostream>
using Microsoft::WRL::ComPtr;
const CLSID kFilter={0xedeca044,0x78cd,0x40eb,{0x8f,0x37,0x63,0xd9,0x47,0xc5,0x01,0xa0}};
int wmain(){
 HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 std::cout<<"process_bits="<<sizeof(void*)*8<<"\n";
 if(FAILED(hr)) return 1;
 int result=0;
 {
 ComPtr<IBaseFilter> filter;
 hr=CoCreateInstance(kFilter,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&filter));
 std::cout<<"CoCreateFilter=0x"<<std::hex<<hr<<"\n";
 if(SUCCEEDED(hr)) {
 ComPtr<ISpecifyPropertyPages> pages;
 hr=filter.As(&pages);
 std::cout<<"ISpecifyPropertyPages=0x"<<std::hex<<hr<<"\n";
 CAUUID ids{};
 if(SUCCEEDED(hr)) {
 hr=pages->GetPages(&ids);
 std::cout<<"GetPages=0x"<<std::hex<<hr<<" count="<<std::dec<<ids.cElems<<"\n";
 if(SUCCEEDED(hr)&&ids.cElems) {
 ComPtr<IPropertyPage> page;
 hr=CoCreateInstance(ids.pElems[0],nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&page));
 std::cout<<"CoCreatePropertyPage=0x"<<std::hex<<hr<<"\n";
 if(SUCCEEDED(hr)) {
 IUnknown* object=filter.Get();
 hr=page->SetObjects(1,&object);
 std::cout<<"SetObjects=0x"<<std::hex<<hr<<"\n";
 PROPPAGEINFO info{};info.cb=sizeof(info);
 hr=page->GetPageInfo(&info);
 std::cout<<"GetPageInfo=0x"<<std::hex<<hr<<" size="<<std::dec<<info.size.cx<<"x"<<info.size.cy<<"\n";
 if(info.pszTitle) std::wcout<<L"title="<<info.pszTitle<<L"\n";
 CoTaskMemFree(info.pszTitle);CoTaskMemFree(info.pszDocString);CoTaskMemFree(info.pszHelpFile);
 page->SetObjects(0,nullptr);
 }
 }
 CoTaskMemFree(ids.pElems);
 }
 } else result=2;
 }
 CoUninitialize();return result;
}
