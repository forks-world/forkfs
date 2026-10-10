#include "container.h"
#include "namespace.h"
#include "rpc.h"
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <string>
#include <system_error>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
[[noreturn]] void fail(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}
struct Socket {
    int fd = -1;
    std::string path;
    dev_t dev{};
    ino_t ino{};
    ~Socket() {
        if (fd >= 0) close(fd);
        struct stat st{};
        if (!path.empty() && lstat(path.c_str(), &st) == 0 &&
            st.st_dev == dev && st.st_ino == ino) unlink(path.c_str());
    }
};
uint64_t file_number(const std::string& text,int base=10) {
    uint64_t value=0;auto parsed=std::from_chars(text.data(),text.data()+text.size(),value,base);
    if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())throw std::runtime_error("invalid file offset/count");
    return value;
}
std::vector<unsigned char> load_file(const char* path) {
    std::ifstream input(path,std::ios::binary);
    if(!input)throw std::runtime_error("cannot open file input");
    std::vector<unsigned char> data(256*1024+1);input.read(reinterpret_cast<char*>(data.data()),data.size());
    if(input.bad() || input.gcount()>256*1024)throw std::runtime_error("invalid or oversized file input");
    data.resize(input.gcount());return data;
}
std::vector<forkfs::Mutation> load_batch(const char* path) {
    std::ifstream input(path,std::ios::binary);
    if(!input) throw std::runtime_error("cannot open batch input");
    std::vector<unsigned char> b(2*1024*1024+1);
    input.read(reinterpret_cast<char*>(b.data()),b.size());
    if(input.bad() || input.gcount()>2*1024*1024) throw std::runtime_error("invalid or oversized batch input");
    b.resize(static_cast<size_t>(input.gcount()));
    if(b.size()<12 || memcmp(b.data(),"FFBATCH1",8)) throw std::runtime_error("invalid batch input magic");
    size_t pos=8;
    auto u32=[&]() -> uint32_t {
        if(b.size()-pos<4) throw std::runtime_error("truncated batch input");
        uint32_t n=0; for(unsigned i=0;i<4;++i) n|=uint32_t(b[pos++])<<(8*i); return n;
    };
    auto count=u32();
    if(count==0 || count>64) throw std::runtime_error("batch requires 1..64 mutations");
    std::vector<forkfs::Mutation> result;
    for(uint32_t i=0;i<count;++i) {
        auto op=u32(),keylen=u32(),vallen=u32();
        if((op!=1 && op!=2) || keylen==0 || keylen>1024 || vallen>256*1024 ||
           (op==2 && vallen!=0) || b.size()-pos<uint64_t(keylen)+vallen)
            throw std::runtime_error("invalid batch input entry");
        forkfs::Mutation m;
        m.key.assign(b.begin()+pos,b.begin()+pos+keylen); pos+=keylen;
        if(op==1) m.value=std::vector<unsigned char>(b.begin()+pos,b.begin()+pos+vallen);
        pos+=vallen; result.push_back(std::move(m));
    }
    if(pos!=b.size()) throw std::runtime_error("trailing batch input bytes");
    return result;
}
void serve(forkfs::Container& container, const std::string& path,bool rpc=false) {
    // Use a private, pre-created runtime directory. Never unlink an existing
    // socket: it may be the control endpoint of a live service.
    auto parent = std::filesystem::path(path).parent_path();
    struct stat st{};
    if (parent.empty() || lstat(parent.c_str(), &st) < 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 077) != 0)
        throw std::runtime_error("socket parent must be a private directory owned by this user (0700)");
    sockaddr_un addr{};
    if (path.size() >= sizeof(addr.sun_path)) throw std::runtime_error("socket path too long");
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size()+1);
    Socket listener;
    listener.fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener.fd < 0) fail("socket");
    if (fcntl(listener.fd, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(listener.fd, F_SETFL, O_NONBLOCK) < 0) fail("socket flags");
    if (bind(listener.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) fail("bind");
    if (lstat(path.c_str(), &st) < 0) fail("stat socket");
    listener.path = path; listener.dev = st.st_dev; listener.ino = st.st_ino;
    if (listen(listener.fd, 16) < 0) fail("listen");
    container.start_compaction();
    std::cout << container.status() << std::flush;
    if(rpc){forkfs::serve_rpc(listener.fd,container,stopping);return;}
    while (!stopping) {
        pollfd p{listener.fd, POLLIN, 0};
        int r = poll(&p, 1, 100);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) fail("poll");
        if (!r) continue;
        if (!(p.revents & POLLIN)) throw std::runtime_error("listener failed");
        Socket client;
        client.fd = accept(listener.fd, nullptr, nullptr);
        if (client.fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (client.fd < 0) fail("accept");
        if (fcntl(client.fd, F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(client.fd, F_SETFL, O_NONBLOCK) < 0) fail("client flags");
        // Bootstrap protocol is a one-shot status greeting, no client input.
        // The reply is bounded; a slow/disconnected client never stalls shutdown.
        auto reply = container.status();
        size_t offset = 0;
        for (int attempts = 0; offset < reply.size() && attempts < 10 && !stopping; ++attempts) {
            auto n = send(client.fd, reply.data()+offset, reply.size()-offset, 0);
            if (n > 0) { offset += n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd out{client.fd, POLLOUT, 0};
                poll(&out, 1, 10);
                continue;
            }
            break;
        }
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "forkfsd init REPOSITORY_DIRECTORY\nforkfsd check CONTAINER\n"
                         "forkfsd serve CONTAINER --socket PATH [--rpc]\n"
                         "forkfsd request SOCKET COMMAND [ARGS...]\n"
                         "RPC sessions: session-open LEASE_MS, session-keepalive TOKEN, session-close TOKEN\n"
                         "RPC handle operations require --session TOKEN.\n"
                         "RPC handles: fs-open PATH MODE[,create][,exclusive][,truncate][,append][,nofollow], fs-close HANDLE, fs-hstat HANDLE\n"
                         "fs-hread HANDLE OFFSET COUNT, fs-hwrite HANDLE OFFSET INPUT_FILE\n"
                         "fs-happend HANDLE INPUT_FILE, fs-htruncate HANDLE SIZE\n"
                         "fs-dup HANDLE, fs-hseek HANDLE SIGNED_OFFSET set|cur|end\n"
                         "fs-hread-next HANDLE COUNT, fs-hwrite-next HANDLE INPUT_FILE\n"
                         "RPC: fs-dir-open PATH, fs-dir-page CURSOR LIMIT, fs-dir-entries CURSOR LIMIT, fs-dir-close CURSOR\n"
                         "forkfsd checkpoint CONTAINER\n"
                         "forkfsd object-put CONTAINER KEY INPUT_FILE\n"
                         "forkfsd object-get CONTAINER KEY\n"
                         "forkfsd object-batch CONTAINER EXPECTED_SEQUENCE BATCH_FILE\n"
                         "forkfsd fs-init CONTAINER\n"
                         "forkfsd fs-mkdir CONTAINER PATH\n"
                         "forkfsd fs-write CONTAINER PATH INPUT_FILE\n"
                         "forkfsd fs-save CONTAINER PATH INPUT_FILE\n"
                         "forkfsd fs-cat CONTAINER PATH\n"
                         "forkfsd fs-read-at CONTAINER PATH OFFSET COUNT\n"
                         "forkfsd fs-write-at CONTAINER PATH OFFSET INPUT_FILE\n"
                         "forkfsd fs-append CONTAINER PATH INPUT_FILE\n"
                         "forkfsd fs-truncate CONTAINER PATH SIZE\n"
                         "forkfsd fs-ls CONTAINER PATH\n"
                         "forkfsd fs-stat CONTAINER PATH\n"
                         "forkfsd fs-lstat CONTAINER PATH\n"
                         "forkfsd fs-symlink CONTAINER TARGET PATH\n"
                         "forkfsd fs-readlink CONTAINER PATH\n"
                         "forkfsd fs-chmod CONTAINER PATH OCTAL_MODE\n"
                         "forkfsd fs-setmtime CONTAINER PATH NANOSECONDS\n"
                         "RPC: fs-hlock HANDLE shared|exclusive|unlock\n"
                         "RPC: fs-hrange-lock HANDLE shared|exclusive|unlock START:LENGTH\n"
                         "RPC: fs-hchmod HANDLE OCTAL_MODE, fs-hsetmtime HANDLE NANOSECONDS\n"
                         "forkfsd fs-rename CONTAINER SOURCE TARGET\n"
                         "forkfsd fs-link CONTAINER SOURCE TARGET\n"
                         "forkfsd fs-unlink CONTAINER PATH\n"
                         "forkfsd fs-rmdir CONTAINER PATH\n"
                         "forkfsd fs-check CONTAINER\n"
                         "forkfsd fs-snapshot CONTAINER REVISION_NAME [--world WORLD]\n"
                         "forkfsd fs-fork CONTAINER REVISION_NAME NEW_WORLD\n"
                         "forkfsd fs-worlds CONTAINER\n"
                         "forkfsd fs-revisions CONTAINER\n"
                         "fs-* read commands accept --world NAME or --revision NAME.\n"
                         "fs-* write commands accept --world NAME.\n"
                         "forkfsd fs-compact CONTAINER [--world WORLD]\n"
                         "forkfsd fs-layout CONTAINER [--world WORLD | --revision REVISION]\n"
                         "LevelDB directory backend; experimental namespace, no OS mount yet.\n";
            return 0;
        }
        if (argc < 3) throw std::runtime_error("use forkfsd --help");
        std::string command = argv[1];
        if(command=="request") {
            if(argc<4)throw std::runtime_error("request requires socket and command");
            std::vector<std::string> fields(argv+3,argv+argc);
            if(fields[0]=="fs-write" || fields[0]=="fs-save" || fields[0]=="fs-write-at" || fields[0]=="fs-append" || fields[0]=="fs-hwrite" || fields[0]=="fs-happend" || fields[0]=="fs-hwrite-next") {
                size_t index=(fields[0]=="fs-write-at" || fields[0]=="fs-hwrite")?3:2;
                if(fields.size()!=index+1 && fields.size()!=index+3 && fields.size()!=index+5)throw std::runtime_error("write command requires input file");
                auto data=load_file(fields[index].c_str());fields[index].assign(data.begin(),data.end());
            }
            auto result=forkfs::request_rpc(argv[2],fields);
            std::cout.write(result.data(),result.size());
            if(!std::cout)throw std::runtime_error("cannot write RPC output");return 0;
        }
        std::string view="main";bool revision_view=false;
        int base_argc=4;
        if(command=="fs-init" || command=="fs-check" || command=="fs-worlds" || command=="fs-revisions" || command=="fs-compact" || command=="fs-layout")base_argc=3;
        if(command=="fs-write" || command=="fs-save" || command=="fs-link" || command=="fs-rename" || command=="fs-fork" || command=="fs-append" || command=="fs-truncate" || command=="fs-symlink" || command=="fs-chmod" || command=="fs-setmtime")base_argc=5;
        if(command=="fs-read-at" || command=="fs-write-at")base_argc=6;
        if(command.starts_with("fs-") && argc==base_argc+2) {
            std::string selector=argv[argc-2];
            if(selector=="--world" || selector=="--revision") {
                view=argv[argc-1];revision_view=selector=="--revision";argc-=2;
            }
        }
        if (command == "init" && argc == 3) {
            forkfs::Container::create(argv[2]);
            forkfs::Container container(argv[2]);
            std::cout << container.status();
        } else if (command == "check" && argc == 3) {
            forkfs::Container container(argv[2]);
            container.verify();
            std::cout << container.status();
        } else if (command == "checkpoint" && argc == 3) {
            forkfs::Container container(argv[2]);
            container.checkpoint();
            std::cout << container.status();
        } else if (command == "object-put" && argc == 5) {
            std::ifstream input(argv[4], std::ios::binary);
            if (!input) throw std::runtime_error("cannot open object input");
            std::vector<unsigned char> value(256*1024+1);
            input.read(reinterpret_cast<char*>(value.data()), value.size());
            auto count = input.gcount();
            if (input.bad()) throw std::runtime_error("cannot read object input");
            if (count > 256*1024) throw std::runtime_error("object exceeds 256 KiB");
            value.resize(static_cast<size_t>(count));
            forkfs::Container container(argv[2]);
            container.put(argv[3],value);
            std::cout << container.status();
        } else if (command == "object-batch" && argc == 5) {
            uint64_t expected=0;
            const char* end=argv[3]+strlen(argv[3]);
            auto parsed=std::from_chars(argv[3],end,expected);
            if(parsed.ec!=std::errc{} || parsed.ptr!=end) throw std::runtime_error("invalid expected sequence");
            auto mutations=load_batch(argv[4]);
            forkfs::Container container(argv[2]);
            container.transact(expected,mutations);
            std::cout << container.status();
        } else if (command == "object-get" && argc == 4) {
            forkfs::Container container(argv[2]);
            const auto value = container.get(argv[3]);
            std::cout.write(reinterpret_cast<const char*>(value.data()),value.size());
            if (!std::cout) throw std::runtime_error("cannot write object output");
        } else if (command.starts_with("fs-")) {
            forkfs::Container container(argv[2]);forkfs::Namespace fs(container,view,revision_view);
            if(command=="fs-compact" && argc==3) fs.compact();
            else if(command=="fs-layout" && argc==3) {
                if(container.storage_engine()=="leveldb") {fs.stat("/");std::cout<<"{\"layout\":\"cow-tree\",\"storage_engine\":\"leveldb\"}\n";return 0;}
                auto runs=fs.run_count();std::cout<<"{\"runs\":"<<runs<<",\"layout\":\""<<(runs?"sorted-runs":"flat")<<"\"}\n";return 0;
            } else if(command=="fs-snapshot" && argc==4) {
                auto id=fs.snapshot(argv[3]);std::cout<<"{\"revision_id\":\""<<id<<"\"}\n";return 0;
            } else if(command=="fs-fork" && argc==5) fs.fork(argv[3],argv[4]);
            else if(command=="fs-worlds" && argc==3) {for(const auto& name:fs.worlds())std::cout<<name<<'\n';return 0;}
            else if(command=="fs-revisions" && argc==3) {for(const auto& name:fs.revisions())std::cout<<name<<'\n';return 0;}
            else if(command=="fs-init" && argc==3) fs.initialize();
            else if(command=="fs-check" && argc==3) {container.verify();fs.verify();}
            else if((command=="fs-chmod" || command=="fs-setmtime") && argc==5) {
                forkfs::Namespace::Attributes a;
                if(command=="fs-chmod"){auto mode=file_number(argv[4],8);if(mode>0777)throw std::runtime_error("unsupported mode bits");a.mode=static_cast<uint32_t>(mode);}
                else a.modified_ns=file_number(argv[4]);fs.set_attributes(argv[3],a);
            }
            else if(command=="fs-symlink" && argc==5)fs.symlink(argv[3],argv[4]);
            else if(command=="fs-readlink" && argc==4){std::cout<<fs.readlink(argv[3]);return 0;}
            else if(command=="fs-mkdir" && argc==4) fs.mkdir(argv[3]);
            else if(command=="fs-unlink" && argc==4) fs.remove(argv[3],false);
            else if(command=="fs-rmdir" && argc==4) fs.remove(argv[3],true);
            else if(command=="fs-rename" && argc==5) fs.rename(argv[3],argv[4]);
            else if(command=="fs-link" && argc==5) fs.link(argv[3],argv[4]);
            else if(command=="fs-truncate" && argc==5)fs.truncate(argv[3],file_number(argv[4]));
            else if(command=="fs-write-at" && argc==6)fs.write_at(argv[3],file_number(argv[4]),load_file(argv[5]));
            else if(command=="fs-append" && argc==5) {
                auto data=load_file(argv[4]);auto offset=fs.append(argv[3],data);
                std::cout<<"{\"offset\":"<<offset<<",\"written\":"<<data.size()<<"}\n";return 0;
            } else if(command=="fs-read-at" && argc==6) {
                auto data=fs.read_at(argv[3],file_number(argv[4]),file_number(argv[5]));
                std::cout.write(reinterpret_cast<const char*>(data.data()),data.size());
                if(!std::cout)throw std::runtime_error("cannot write file output");return 0;
            } else if(command=="fs-save" && argc==5) {fs.replace_file(argv[3],load_file(argv[4]));
            } else if(command=="fs-write" && argc==5) {
                fs.write(argv[3],load_file(argv[4]));
            } else if(command=="fs-cat" && argc==4) {
                auto value=fs.read(argv[3]);
                std::cout.write(reinterpret_cast<const char*>(value.data()),value.size());
                if(!std::cout)throw std::runtime_error("cannot write file output");return 0;
            } else if(command=="fs-ls" && argc==4) {
                for(const auto& name:fs.list(argv[3]))std::cout<<name<<'\n';return 0;
            } else if((command=="fs-stat" || command=="fs-lstat") && argc==4) {
                auto n=command=="fs-lstat"?fs.lstat(argv[3]):fs.stat(argv[3]);
                std::cout<<"{\"inode_id\":\""<<n.id<<"\",\"directory\":"<<(n.directory?"true":"false")
                         <<",\"symlink\":"<<(n.symlink?"true":"false")<<",\"mode\":"<<n.mode<<",\"links\":"<<n.links<<",\"size\":"<<n.size
                         <<",\"modified_ns\":"<<n.modified_ns<<",\"changed_ns\":"<<n.changed_ns<<"}\n";
                return 0;
            } else throw std::runtime_error("invalid filesystem command arguments");
            std::cout<<container.status();
        } else if (command == "serve" && (argc == 5 || (argc==6 && std::string(argv[5])=="--rpc")) && std::string(argv[3]) == "--socket") {
            umask(0077);
            struct sigaction action{};
            action.sa_handler = stop;
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGINT, &action, nullptr) < 0 || sigaction(SIGTERM, &action, nullptr) < 0)
                fail("install signal handlers");
            signal(SIGPIPE, SIG_IGN);
            forkfs::Container container(argv[2]);
            serve(container, argv[4],argc==6);
        } else throw std::runtime_error("invalid arguments; use forkfsd --help");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "forkfsd: " << e.what() << '\n';
        return 1;
    }
}
