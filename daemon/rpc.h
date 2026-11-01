#pragma once
#include "container.h"
#include <csignal>
namespace forkfs {
// Experimental same-UID control protocol, not an OS mount or agent sandbox.
void serve_rpc(int listener,Container& container,const volatile sig_atomic_t& stopping);
std::string request_rpc(const std::string& socket,const std::vector<std::string>& fields);
}
