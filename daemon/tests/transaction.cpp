#include "container.h"
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

using Bytes=std::vector<unsigned char>;
void check(bool condition) { if(!condition) throw std::runtime_error("transaction concurrency check failed"); }
int main() {
    auto pattern=(std::filesystem::temp_directory_path()/"ff-concurrency-XXXXXX").string();
    char* directory=mkdtemp(pattern.data());
    if(!directory) return 1;
    std::string path=std::string(directory)+"/store";
    try {
        forkfs::Container::create(path);
        {
            forkfs::Container store(path);
            store.transact(0,{{"a",Bytes{0}},{"b",Bytes{0}}});
            std::barrier gate(3);
            std::atomic<int> success=0,conflicts=0,unexpected=0;
            auto writer=[&](unsigned char value) {
                gate.arrive_and_wait();
                try {store.transact(1,{{"a",Bytes{value}},{"b",Bytes{value}}});++success;}
                catch(const std::exception& e) {
                    if(std::string(e.what()).find("transaction conflict")!=std::string::npos) ++conflicts;
                    else ++unexpected;
                }
            };
            std::thread first(writer,1),second(writer,2);
            gate.arrive_and_wait();first.join();second.join();
            check(success==1 && conflicts==1 && unexpected==0);
            std::atomic<bool> stop=false,failed=false;
            std::atomic<int> reads=0;
            std::thread reader([&] {
                try {
                    do {
                        auto pair=store.get_many({"a","b"});
                        if(pair[0]!=pair[1]) failed=true;
                        ++reads;
                    } while(!stop.load());
                } catch(...) {failed=true;}
            });
            try {
                for(uint64_t seq=2;seq<22;++seq)
                    store.transact(seq,{{"a",Bytes(128,seq)},{"b",Bytes(128,seq)}});
            } catch(...) {stop=true;reader.join();throw;}
            stop=true;reader.join();check(!failed && reads>0);
            store.checkpoint();
            // Alias-only transaction has no new OBJECT records and must replay.
            auto original=forkfs::Journal::object_id(Bytes{0});
            store.transact(22,{{"shared",std::nullopt,original}});
            check(store.get("shared")==Bytes{0});
        }
        forkfs::Container reopened(path);
        auto pair=reopened.get_many({"a","b"});check(pair[0]==pair[1] && pair[0]==Bytes(128,21));
        check(reopened.get("shared")==Bytes{0});
        reopened.verify();
        std::filesystem::remove_all(directory);
        std::cout<<"transaction concurrency and restart checks passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';std::filesystem::remove_all(directory);return 1;
    }
}
