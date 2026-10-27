#include "namespace.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

using Bytes=std::vector<unsigned char>;
namespace fs=std::filesystem;
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}

uint64_t field(const std::string& json,const std::string& name) {
    auto key="\""+name+"\":";auto at=json.find(key);
    check(at!=std::string::npos,("missing report field "+name).c_str());
    at+=key.size();size_t end=at;while(end<json.size()&&json[end]>='0'&&json[end]<='9')++end;
    check(end>at,("invalid report field "+name).c_str());return std::stoull(json.substr(at,end-at));
}
bool truth(const std::string& json,const std::string& name) {
    auto key="\""+name+"\":";auto at=json.find(key);
    check(at!=std::string::npos,("missing report field "+name).c_str());
    return json.compare(at+key.size(),4,"true")==0;
}
uint64_t data_bytes(const fs::path& dir) {
    uint64_t total=0;
    for(const auto& entry:fs::directory_iterator(dir)) {
        auto name=entry.path().filename().string();
        if(entry.is_regular_file()&&name.starts_with("DATA-")&&entry.path().extension()==".seg")total+=entry.file_size();
    }
    return total;
}
uint64_t percentile(std::vector<uint64_t> values,double p) {
    std::sort(values.begin(),values.end());return values[static_cast<size_t>((values.size()-1)*p)];
}

int main() {
    char pattern[]="/tmp/ff-physical-gc-XXXXXX";char* temp=mkdtemp(pattern);if(!temp)return 1;
    const fs::path root(temp),store=root/"store";
    try {
        forkfs::Container::create(store.string());
        uint64_t initial=0,peak=0,final=0;size_t steps=0;std::vector<uint64_t> timings,write_times,read_times;
        bool mutation_mix_done=false,physical_mutation_done=false;
        {
            forkfs::Container container(store.string());forkfs::Namespace ns(container);ns.initialize();ns.mkdir("/rows");
            for(int i=0;i<160;++i) {
                Bytes payload(4096,static_cast<unsigned char>(i));
                ns.write("/rows/r"+std::to_string(i),payload);
            }
            for(int i=0;i<90;++i) {
                Bytes payload(4096,static_cast<unsigned char>(i+1));
                ns.write("/rows/r"+std::to_string(i),payload);
            }
            initial=data_bytes(store);
            // Tiny mark and physical budgets must make visible progress in bounded increments.
            auto first=container.collect_step(65580,1,65536,1);
            check(field(first,"physical_work_done")<=1,"physical work exceeded budget=1");
            check(truth(first,"physical_pending"),"one-unit call hid the physical scan");
            check(truth(first,"mark_pending"),"one-unit call reported collection complete");
            for(size_t guard=0;guard<200000;++guard) {
                auto start=std::chrono::steady_clock::now();
                auto report=container.collect_step(65580,1,65536,1);
                timings.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
                check(field(report,"physical_work_done")<=1,"physical work exceeded budget=1 during resume");
                ++steps;peak=std::max(peak,data_bytes(store));
                // Keep foreground writes and reads interleaved with continuation calls.
                if(steps<=128 && (steps%7)==0) {
                    unsigned char v=static_cast<unsigned char>(steps);Bytes value(4096,v);
                    auto began=std::chrono::steady_clock::now();ns.write("/rows/r0",value);
                    write_times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-began).count());
                    began=std::chrono::steady_clock::now();auto actual=ns.read("/rows/r0");
                    read_times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-began).count());
                    check(actual==value,"interleaved foreground write was lost");
                }
                if(!mutation_mix_done&&steps>=10) {
                    ns.remove("/rows/r1",false);Bytes value(4096,0xe7);ns.write("/rows/new-live",value);
                    check(ns.read("/rows/new-live")==value,"new live reference was lost during collection");
                    mutation_mix_done=true;
                }
                if(!physical_mutation_done && report.find("\"physical_phase\":\"source_c\"")!=std::string::npos){
                    ns.write("/after-seal",Bytes(4096,0xfa));
                    check(ns.read("/after-seal")==Bytes(4096,0xfa),"publication during sealed scan was lost");
                    physical_mutation_done=true;
                }
                if(!truth(report,"mark_pending")&&!truth(report,"physical_pending"))break;
                check(guard+1<200000,"bounded collection failed to converge");
            }
            check(steps>10,"collection did not resume across bounded calls");
            check(physical_mutation_done,"fixture did not exercise publication during sealed scan");
            check(ns.read("/after-seal")==Bytes(4096,0xfa),"sealed-scan publication was reclaimed");
            check(mutation_mix_done,"collection ended before interleaved delete/new-reference mutation");
            bool deleted=false;try{(void)ns.read("/rows/r1");}catch(...){deleted=true;}check(deleted,"deleted row was resurrected");
            check(ns.read("/rows/new-live")==Bytes(4096,0xe7),"new live row was lost");
            for(int i=0;i<90;++i)if(i!=1)check(ns.read("/rows/r"+std::to_string(i)).size()==4096,"live row was lost");
            for(int i=90;i<160;++i)check(ns.read("/rows/r"+std::to_string(i))==Bytes(4096,static_cast<unsigned char>(i)),"untouched row was lost");
            final=data_bytes(store);check(final<initial,"physical GC did not reduce DATA bytes");
            check(peak<=2*initial+2*1024*1024,"bounded fixture caused excessive DATA growth");

            // Cancellation discards cursors; reopen safely rebuilds collection progress.
            auto cancelled=container.collect_step(65580,1,65536,1);check(field(cancelled,"physical_work_done")<=1,"cancel setup exceeded budget");
            container.stop_collection();
            ns.write("/after-cancel",Bytes{9});check(ns.read("/after-cancel")==Bytes{9},"write after partial collection failed");
            ns.verify();container.verify();
        }
        {
            forkfs::Container reopened(store.string());forkfs::Namespace ns(reopened);
            check(ns.read("/after-cancel")==Bytes{9},"cancel/reopen lost acknowledged data");
            check(ns.read("/after-seal")==Bytes(4096,0xfa),"reopen lost sealed-scan publication");
            for(int i=0;i<90;++i)if(i!=1)check(ns.read("/rows/r"+std::to_string(i)).size()==4096,"reopen lost a live row");
            std::vector<uint64_t> resume_times;
            for(size_t guard=0;guard<200000;++guard) {
                auto start=std::chrono::steady_clock::now();auto report=reopened.collect_step(65580,2,65536,2);
                resume_times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
                check(field(report,"physical_work_done")<=2,"physical work exceeded resumed budget");
                if(!truth(report,"mark_pending")&&!truth(report,"physical_pending"))break;
                check(guard+1<200000,"reopened collection failed to converge");
            }
            reopened.verify();
            if(!resume_times.empty())timings.insert(timings.end(),resume_times.begin(),resume_times.end());
        }
        std::cout<<"physical GC budget regression passed; steps="<<steps<<" DATA initial/peak/final="<<initial<<'/'<<peak<<'/'<<final
                 <<" bytes; collect_step us p50/p95/p99="<<percentile(timings,.50)<<'/'<<percentile(timings,.95)<<'/'<<percentile(timings,.99)<<'\n';
        std::cout<<"foreground interleaved samples="<<write_times.size()<<" write us p50/p95/p99="
                 <<percentile(write_times,.50)<<'/'<<percentile(write_times,.95)<<'/'<<percentile(write_times,.99)
                 <<" read us p50/p95/p99="<<percentile(read_times,.50)<<'/'<<percentile(read_times,.95)<<'/'<<percentile(read_times,.99)<<'\n';
        fs::remove_all(root);return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';fs::remove_all(root);return 1;}
}
