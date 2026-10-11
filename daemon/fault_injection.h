#pragma once
// Test executable only. Production builds do not parse configuration or emit events.
#ifdef FORKFS_TEST_FAULTS
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
namespace forkfs::faults {
enum class Action {None,IoError,NoSpace};
inline std::atomic<uint64_t> hits=0;
inline thread_local bool transaction_io=false;
inline void validate() {
    auto point=std::getenv("FORKFS_TEST_FAULT");if(!point)return;
    bool known=false;
    for(auto value:{"leveldb_before_write","leveldb_after_write","wal_append","wal_sync","wal_after_sync",
                    "compaction_stage_committed","compaction_before_publish","compaction_published"})
        if(!std::strcmp(point,value))known=true;
    if(!known)throw std::runtime_error("unknown fault point");
    if(auto action=std::getenv("FORKFS_TEST_FAULT_ACTION"))
        if(std::strcmp(action,"crash") && std::strcmp(action,"eio") && std::strcmp(action,"enospc"))throw std::runtime_error("invalid fault action");
    if(auto value=std::getenv("FORKFS_TEST_FAULT_HIT")) {
        uint64_t count=0;auto end=value+std::strlen(value);auto parsed=std::from_chars(value,end,count);
        if(parsed.ec!=std::errc{} || parsed.ptr!=end || !count)throw std::runtime_error("invalid fault hit count");
    }
}
inline Action hit(const char* point) {
    auto selected=std::getenv("FORKFS_TEST_FAULT");
    if(!selected || std::strcmp(selected,point))return Action::None;
    uint64_t target=1;
    if(auto value=std::getenv("FORKFS_TEST_FAULT_HIT")) {
        auto end=value+std::strlen(value);auto parsed=std::from_chars(value,end,target);
        if(parsed.ec!=std::errc{} || parsed.ptr!=end || !target)throw std::runtime_error("invalid fault hit count");
    }
    const char* action=std::getenv("FORKFS_TEST_FAULT_ACTION");if(!action)action="crash";
    if(std::strcmp(action,"crash") && std::strcmp(action,"eio") && std::strcmp(action,"enospc"))throw std::runtime_error("invalid fault action");
    auto count=++hits;if(count!=target)return Action::None;
    std::fprintf(stderr,"{\"fault_point\":\"%s\",\"hit\":%llu,\"action\":\"%s\"}\n",point,static_cast<unsigned long long>(count),action);
    std::fflush(stderr);
    if(!std::strcmp(action,"crash"))_exit(86);
    return !std::strcmp(action,"eio")?Action::IoError:Action::NoSpace;
}
inline void boundary(const char* point) {
    auto action=hit(point);
    if(action!=Action::None)throw std::system_error(action==Action::IoError?EIO:ENOSPC,std::generic_category(),"injected storage fault");
}
struct TransactionIo {
    bool previous=transaction_io;
    TransactionIo(){transaction_io=true;}
    ~TransactionIo(){transaction_io=previous;}
};
}
#endif
