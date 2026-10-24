#pragma once
#import <Foundation/Foundation.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "namespace.h"

namespace forkfs {
struct MountCredentials {
    std::string socket,generation,session,name;
    uid_t service_uid;
    bool read_only;
    uint64_t lease_ms;
};
// Does not open any Container/backing path. The handoff is the only resource.
std::optional<MountCredentials> mount_credentials(NSURL *resource);
class MountTransport {
public:
    explicit MountTransport(MountCredentials credentials);
    ~MountTransport();
    std::string call(std::vector<std::string> fields);
    NSDictionary *json(std::vector<std::string> fields);
    bool healthy() const;
    void revoke();
    const MountCredentials& credentials() const {return credentials_;}
private:
    std::string request(std::vector<std::string> fields);
    void renew();
    MountCredentials credentials_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool dead_=false,stopping_=false;
    std::thread renewer_;
};
InodeInfo remote_inode(NSDictionary *value);
std::string remote_text(id value);
std::string remote_unhex(id value);
std::string attribute_patch(const Namespace::Attributes& value);
}
