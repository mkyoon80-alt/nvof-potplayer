#include "nvof/tuning.hpp"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <future>
#include <fstream>
using namespace nvof::tuning;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    auto dir=std::filesystem::temp_directory_path()/(L"nvof-tuning-test-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"NVOF_TUNING_DIR",dir.c_str());
    require(validate(defaults),"Invalid defaults");
    auto values=defaults;
    values[warp_photo_scale]=std::numeric_limits<float>::quiet_NaN();require(!validate(values),"Accepted NaN");
    values=defaults;values[evidence_fb_low]=values[evidence_fb_high];require(!validate(values),"Accepted reversed interval");
    std::filesystem::create_directories(dir);
    const auto profile=dir/L"startup.ini";
    require(load_startup_profile(profile.wstring())==defaults,"Missing profile changed defaults");
    {
        std::ofstream file(profile);
        file<<"[Tuning]\nEnabled=1\nSchemaHash="<<schema_hash()<<'\n';
#define NVOF_TUNABLE(name, value, lo, hi, group, unit, description) file<<#name "="<<value<<'\n';
#include "nvof/tuning_parameters.def"
#undef NVOF_TUNABLE
    }
    WritePrivateProfileStringW(L"Tuning",L"warp_photo_scale",L"18",profile.c_str());
    auto startup=defaults;startup[warp_photo_scale]=18;
    require(load_startup_profile(profile.wstring())==startup,"Valid startup profile not loaded");
    WritePrivateProfileStringW(L"Tuning",L"evidence_fb_low",L"3",profile.c_str());
    require(load_startup_profile(profile.wstring())==defaults,"Invalid startup interval applied partially");
    WritePrivateProfileStringW(L"Tuning",L"evidence_fb_low",L"0.4",profile.c_str());
    WritePrivateProfileStringW(L"Tuning",L"warp_photo_scale",L"18oops",profile.c_str());
    require(load_startup_profile(profile.wstring())==defaults,"Malformed startup number accepted");
    WritePrivateProfileStringW(L"Tuning",L"warp_photo_scale",L"18",profile.c_str());
    WritePrivateProfileStringW(L"Tuning",L"SchemaHash",L"1",profile.c_str());
    require(load_startup_profile(profile.wstring())==defaults,"Wrong startup schema accepted");
    WritePrivateProfileStringW(L"Tuning",L"SchemaHash",std::to_wstring(schema_hash()).c_str(),profile.c_str());
    WritePrivateProfileStringW(L"Tuning",L"Enabled",L"0",profile.c_str());
    require(load_startup_profile(profile.wstring())==defaults,"Disabled startup profile applied");
    WritePrivateProfileStringW(L"Tuning",L"Enabled",L"1",profile.c_str());
    WritePrivateProfileStringW(L"Tuning",L"warp_photo_scale",nullptr,profile.c_str());
    require(load_startup_profile(profile.wstring())==defaults,"Incomplete startup profile applied");
    std::filesystem::remove(profile);
    std::string identifier;
    {
        Channel channel,other;identifier=channel.id();require(identifier!=other.id(),"Session IDs collide");
        std::wstring id(identifier.begin(),identifier.end()),name=L"Local\\NvofTuning-"+id;
        HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());require(mapping!=nullptr,"No mapping");
        auto state=static_cast<Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
        HANDLE mutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,(name+L"-lock").c_str());
        require(state&&mutex,"No state or mutex");
        WaitForSingleObject(mutex,INFINITE);
        require(state->schema_hash==schema_hash(),"Schema mismatch");
        state->requested[warp_photo_scale]=6;state->requested_revision=2;
        ReleaseMutex(mutex);
        require(channel.values()[warp_photo_scale]==defaults[warp_photo_scale],"Applied before next pair");
        channel.begin_pair();require(channel.values()[warp_photo_scale]==6,"Did not latch next pair");
        channel.publish(320,180,false);require(state->applied_revision==0,"Acknowledged failed prepare");
        channel.publish(320,180,true);require(state->applied_revision==2,"Missing prepare acknowledgement");
        WaitForSingleObject(mutex,INFINITE);
        state->requested[evidence_fb_low]=3;state->requested_revision=3;
        ReleaseMutex(mutex);channel.begin_pair();
        require(channel.revision()==2&&state->rejected_revision==3,"Invalid configuration was not rejected atomically");
        std::promise<void> locked,release;auto releaseFuture=release.get_future();
        std::thread holder([&]{WaitForSingleObject(mutex,INFINITE);locked.set_value();releaseFuture.wait();ReleaseMutex(mutex);});
        locked.get_future().wait();auto start=GetTickCount64();
        channel.begin_pair();channel.publish(320,180,true);channel.rendered();
        auto elapsed=GetTickCount64()-start;release.set_value();holder.join();
        require(elapsed<100,"Playback blocked on controller lock");
        WaitForSingleObject(mutex,INFINITE);state->requested=defaults;state->requested_revision=4;ReleaseMutex(mutex);
        channel.begin_pair();channel.publish(320,180,true);
        require(channel.values()==defaults&&state->applied_revision==4,"Reset did not restore defaults");
        require(other.values()==defaults&&other.revision()==1,"Changed another instance");
        UnmapViewOfFile(state);CloseHandle(mapping);CloseHandle(mutex);
    }
    require(!std::filesystem::exists(dir/(identifier+".session")),"Manifest leaked");
    std::filesystem::remove(dir);
    std::cout<<"PASS startup profile, validation, pair latch, rejection, nonblocking contention, reset, instance isolation, cleanup\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
