#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
int wmain(int argc,wchar_t** wide) {
    if(argc<7 || (argc-5)%2) return 2;
    try {
        std::vector<std::string> args;
        for(int i=0;i<argc;++i) { std::wstring text=wide[i]; args.emplace_back(text.begin(),text.end()); }
        std::vector<const char*> argv; for(auto& arg:args)argv.push_back(arg.c_str());
        std::ifstream input(std::filesystem::path(wide[1]),std::ios::binary);
        if(!input) throw std::runtime_error("Cannot read HLSL source");
        std::string source((std::istreambuf_iterator<char>(input)),{});
        std::string generated="#pragma once\nnamespace nvof::shaders::"+std::string(argv[3])+" {\n";
        for(int i=5;i<argc;i+=2) {
            Microsoft::WRL::ComPtr<ID3DBlob> code,errors;
            HRESULT hr=D3DCompile(source.data(),source.size(),argv[4],nullptr,nullptr,argv[i],argv[i+1],D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
            if(FAILED(hr)) { if(errors)std::cerr<<static_cast<const char*>(errors->GetBufferPointer()); return 3; }
            generated+="inline constexpr unsigned char "+std::string(argv[i])+"[] = {\n";
            auto bytes=static_cast<const unsigned char*>(code->GetBufferPointer());
            for(size_t n=0;n<code->GetBufferSize();++n) {
                generated+=std::to_string(bytes[n])+",";
                if(n%32==31)generated+='\n';
            }
            generated+="\n};\n";
            std::cout<<argv[3]<<":"<<argv[i]<<" "<<code->GetBufferSize()<<" bytes\n";
        }
        generated+="}\n";
        std::filesystem::path output(wide[2]);
        std::filesystem::create_directories(output.parent_path());
        std::ofstream file(output,std::ios::binary|std::ios::trunc);
        file<<generated;
        if(!file)throw std::runtime_error("Cannot write shader header");
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 4; }
    return 0;
}
