#define NOMINMAX
#include "nvof/status_writer.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace std::chrono_literals;
void require(bool value,const char* what){if(!value)throw std::runtime_error(what);}
int main() {
    try {
        // A disk operation is deliberately blocked. Producers must finish
        // before it is unblocked; only the newest pending status may survive.
        std::promise<void> entered,unblock;
        auto released=unblock.get_future().share();
        std::vector<std::string> written;
        nvof::StatusWriter writer([&](const nvof::StatusSnapshot& s){
            if(s.text=="first"){entered.set_value();released.wait();}
            written.push_back(s.text);
        });
        writer.publish({},"first");require(entered.get_future().wait_for(2s)==std::future_status::ready,"writer did not start");
        auto producer=std::async(std::launch::async,[&]{for(int i=0;i<200;++i)writer.publish({},std::to_string(i));writer.publish({},"stopped");});
        const bool nonblocking=producer.wait_for(500ms)==std::future_status::ready;
        unblock.set_value();producer.get();writer.drain();
        require(nonblocking,"status producer waited for disk");
        require(written==std::vector<std::string>{"first","stopped"},"pending status backlog or stale final state");
        std::atomic<unsigned> calls=0;
        {nvof::StatusWriter failing([&](const nvof::StatusSnapshot&){++calls;throw std::runtime_error("disk failure");});
            failing.publish({},"error");failing.drain();failing.publish({},"stopped");}
        require(calls==2,"disk exception killed writer or destructor discarded latest status");
        wchar_t temporary[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH,temporary)>0,"temporary test path unavailable");
        const auto folder=std::filesystem::path(temporary)/(L"nvof-status-test-"+std::to_wstring(GetCurrentProcessId()));
        const nvof::StatusSnapshot older{nvof::status_detail::next_sequence.fetch_add(1),folder,"{\"state\":\"active\"}"};
        const nvof::StatusSnapshot newer{nvof::status_detail::next_sequence.fetch_add(1),folder,"{\"state\":\"stopped\"}"};
        nvof::status_detail::write_file(newer);nvof::status_detail::write_file(older);
        std::string actual;
        {std::ifstream saved(folder/L"status.json");actual.assign(std::istreambuf_iterator<char>(saved),{});}
        std::filesystem::remove(folder/L"status.json");std::filesystem::remove(folder);
        require(actual==newer.text,"retiring publisher replaced a newer status file");
        auto shared=nvof::acquire_status_writer();auto another=nvof::acquire_status_writer();
        require(shared&&shared==another,"filter instances do not share a writer");
        std::cout<<"PASS blocked disk does not block publish, latest-only bound, final state/drain, stale file rejection, failure recovery, shutdown/shared lifetime\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
