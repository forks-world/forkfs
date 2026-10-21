#pragma once
#include "container.h"
#include <csignal>
#include <optional>
#include <sys/types.h>
namespace forkfs {
// Kernel-authenticated control UID; callers must enforce endpoint permissions.
void serve_rpc(int listener,Container& container,const volatile sig_atomic_t& stopping,uid_t control_uid,std::optional<uid_t> frontend_uid=std::nullopt);
// Default pins the server to the calling effective UID. Separate services must
// supply an expected service UID from trusted deployment configuration.
std::string request_rpc(const std::string& socket,const std::vector<std::string>& fields);
std::string request_rpc(const std::string& socket,const std::vector<std::string>& fields,uid_t expected_server);
}
