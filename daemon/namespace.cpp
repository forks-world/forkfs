#include "namespace.h"
#include "metadata_view.h"
#include "metadata_tree.h"
#include "compaction.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <set>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <unistd.h>

namespace forkfs {
namespace {
using ObjectId=std::array<unsigned char,32>;
const std::string history("\0forkfs-history/",16);
std::string hex_id(const ObjectId& id) {
    std::string result;result.reserve(64);const char* hex="0123456789abcdef";
    for(auto c:id){result+=hex[c>>4];result+=hex[c&15];}return result;
}
void valid_view_name(const std::string& name) {
    if(name.empty() || name.size()>64 || !std::all_of(name.begin(),name.end(),[](unsigned char c){
        return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';
    }) || name=="." || name=="..")throw std::runtime_error("invalid World/revision name");
}
ObjectId revision_manifest(const Journal& j,const std::string& name) {
    valid_view_name(name);auto b=j.get(history+"revision/"+name);
    if(b.size()!=80 || memcmp(b.data(),"FFREV001",8))throw std::runtime_error("invalid revision record");
    ObjectId id{};std::copy_n(b.data()+8,32,id.begin());return id;
}
}
namespace {
using Bytes=std::vector<unsigned char>;
const std::string prefix("\0forkfs/",8);
const std::string root_key=prefix+"root";
void need(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
uint64_t load(const unsigned char* p,size_t n) {
    uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(p[i])<<(8*i);return v;
}
void store(unsigned char* p,uint64_t v,size_t n) {
    for(size_t i=0;i<n;++i)p[i]=static_cast<unsigned char>(v>>(8*i));
}
uint64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string inode_key(const std::string& id) {return prefix+"inode/"+id;}
std::string allocated_key(const std::string& id) {return prefix+"allocated/"+id;}
std::string xattr_prefix(const std::string& id) {return prefix+"xattr/"+id+"/";}
std::string content_key(const std::string& id) {return prefix+"content/"+id;}
std::string prefix_end(std::string value) {
    for(size_t i=value.size();i>0;--i)if(static_cast<unsigned char>(value[i-1])!=255){
        value[i-1]=static_cast<char>(static_cast<unsigned char>(value[i-1])+1);value.resize(i);return value;}
    throw std::runtime_error("invalid unbounded prefix");
}
std::string directory_prefix(const std::string& id) {return prefix+"entry/"+id+"/";}
std::string entry_key(const std::string& id,const std::string& name) {return directory_prefix(id)+name;}
bool valid_id(const std::string& id) {
    return id.size()==32 && std::all_of(id.begin(),id.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
Bytes id_bytes(const std::string& id) {need(valid_id(id),"invalid inode identity");return Bytes(id.begin(),id.end());}
std::string decode_id(const Bytes& b) {std::string id(b.begin(),b.end());need(valid_id(id),"invalid inode reference");return id;}
std::string root(const NamespaceView& journal) {
    need(journal.contains(root_key),"filesystem not initialized; use fs-init");return decode_id(journal.get(root_key));
}
InodeInfo decode_inode(const Bytes& b,const std::string& id,bool orphan=false) {
    bool large=b.size()==64 && !memcmp(b.data(),"FFINOD03",8);
    bool flagged=b.size()==72 && !memcmp(b.data(),"FFINOD04",8);
    bool extended=flagged || large || (b.size()==64 && !memcmp(b.data(),"FFINOD02",8));
    need(extended || (b.size()==48 && !memcmp(b.data(),"FFINOD01",8)),"invalid inode record");
    auto type=load(b.data()+8,4),encoded_mode=load(b.data()+12,4);
    // Older readers reject this mode bit instead of silently losing xattrs.
    bool xattrs=(encoded_mode&0x80000000U)!=0;auto mode=encoded_mode&~uint64_t(0x80000000U);
    need((type==1||type==2||type==3||type==4) && mode<=0777,"unsupported inode attributes");
    InodeInfo n{id,type==2,static_cast<uint32_t>(mode),load(b.data()+16,8),load(b.data()+24,8),
                load(b.data()+32,8),load(b.data()+40,8),type==3,type==4,xattrs};
    need(n.size<=maximum_file_bytes && (flagged?(n.size<=legacy_file_bytes || n.chunked):large?(n.chunked && n.size>legacy_file_bytes):n.size<=legacy_file_bytes),"invalid inode size/version");
    if(flagged){n.flags=load(b.data()+64,4);need(load(b.data()+68,4)==0 && !(n.flags&~supported_file_flags),"unsupported file flags");}
    n.accessed_ns=extended?load(b.data()+48,8):n.modified_ns;n.birth_ns=extended?load(b.data()+56,8):n.changed_ns;
    need(n.accessed_ns<=INT64_MAX && n.birth_ns<=INT64_MAX,"inode time exceeds supported nanosecond range");
    need((orphan?(!n.directory && n.links==0):n.links>0) && (!n.directory || (n.links>=2 && n.size==0)),"invalid inode state");return n;
}
InodeInfo inode(const NamespaceView& journal,const std::string& id) {return decode_inode(journal.get(inode_key(id)),id);}
std::string orphan_prefix(const std::string& world,const std::string& id) {return history+"orphan/"+world+"/"+id+"/";}
std::unique_ptr<NamespaceView> retained_orphan(const Journal& journal,const std::string& pre) {
    if(!journal.contains(pre+"tree"))return {};
    auto b=journal.get(pre+"tree");need(b.size()==40 && !memcmp(b.data(),"FFTREE01",8),"invalid orphan tree");
    return std::make_unique<NamespaceView>(journal,journal.root_object(pre+"tree"));
}
Bytes encode(const InodeInfo& n) {
    Bytes b(n.flags?72:64);memcpy(b.data(),n.flags?"FFINOD04":n.size>legacy_file_bytes?"FFINOD03":"FFINOD02",8);store(b.data()+8,n.symlink?3:n.directory?2:n.chunked?4:1,4);
    store(b.data()+12,uint64_t(n.mode)|(n.xattrs?uint64_t(0x80000000U):0),4);store(b.data()+16,n.links,8);store(b.data()+24,n.size,8);
    store(b.data()+32,n.modified_ns,8);store(b.data()+40,n.changed_ns,8);
    store(b.data()+48,n.accessed_ns,8);store(b.data()+56,n.birth_ns,8);if(n.flags)store(b.data()+64,n.flags,4);return b;
}
void valid_xattr_name(const std::string& name);
std::string chunk_suffix(uint64_t index) {
    need(index<maximum_file_bytes/file_chunk_bytes,"invalid file chunk index");
    auto digits=std::to_string(index);
    // Preserve every old key. Extended keys sort after the old 0000..1023 range
    // and remain numerically ordered without rewriting existing content leaves.
    return index<legacy_file_bytes/file_chunk_bytes?std::string(4-digits.size(),'0')+digits:
        "x"+std::string(8-digits.size(),'0')+digits;
}
bool parse_chunk_suffix(std::string_view suffix,uint64_t& index) {
    bool extended=suffix.size()==9 && suffix[0]=='x';
    if(!extended && suffix.size()!=4)return false;
    if(extended)suffix.remove_prefix(1);
    index=0;for(char c:suffix){if(c<'0'||c>'9')return false;index=index*10+uint64_t(c-'0');}
    return index<maximum_file_bytes/file_chunk_bytes &&
        (extended?index>=legacy_file_bytes/file_chunk_bytes:index<legacy_file_bytes/file_chunk_bytes);
}
bool valid_orphan_field(const std::string& field) {
    if(field=="inode" || field=="content" || field=="tree")return true;
    if(field.starts_with("xattr/")){valid_xattr_name(field.substr(6));return true;}
    uint64_t index;return field.starts_with("chunk/") && parse_chunk_suffix(std::string_view(field).substr(6),index);
}
std::string new_id(const NamespaceView& journal) {
    for(int attempt=0;attempt<8;++attempt) {
        unsigned char bytes[16];int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
        if(fd<0)throw std::system_error(errno,std::generic_category(),"open inode entropy");
        size_t at=0;
        while(at<sizeof(bytes)) {
            auto n=::read(fd,bytes+at,sizeof(bytes)-at);
            if(n<0 && errno==EINTR)continue;
            if(n<=0){int error=n<0?errno:EIO;close(fd);throw std::system_error(error,std::generic_category(),"inode entropy");}
            at+=n;
        }
        close(fd);std::string id;const char* hex="0123456789abcdef";
        for(auto c:bytes){id+=hex[c>>4];id+=hex[c&15];}
        if(!journal.contains(allocated_key(id)) && !journal.contains(inode_key(id)))return id;
    }
    throw std::runtime_error("inode identity collision");
}
void valid_namespace_text(const std::string& value) {
    // Keep the original bytes as identity. Decode only to reject malformed
    // UTF-8 and the C0/DEL/C1 controls excluded by the naming policy.
    for(size_t at=0;at<value.size();) {
        const auto lead=static_cast<unsigned char>(value[at++]);
        uint32_t scalar=lead,minimum=0;size_t remaining=0;
        if(lead<0x80){}
        else if(lead>=0xc2 && lead<=0xdf){scalar=lead&0x1f;remaining=1;minimum=0x80;}
        else if(lead>=0xe0 && lead<=0xef){scalar=lead&0x0f;remaining=2;minimum=0x800;}
        else if(lead>=0xf0 && lead<=0xf4){scalar=lead&0x07;remaining=3;minimum=0x10000;}
        else need(false,"invalid UTF-8 namespace text");
        need(remaining<=value.size()-at,"invalid UTF-8 namespace text");
        for(size_t i=0;i<remaining;++i) {
            const auto next=static_cast<unsigned char>(value[at++]);
            need((next&0xc0)==0x80,"invalid UTF-8 namespace text");
            scalar=(scalar<<6)|(next&0x3f);
        }
        need(scalar>=minimum && scalar<=0x10ffff && !(scalar>=0xd800 && scalar<=0xdfff),"invalid UTF-8 namespace text");
        need(scalar>=0x20 && !(scalar>=0x7f && scalar<=0x9f),"invalid control character in namespace text");
    }
}
void valid_xattr_name(const std::string& name) {
    need(!name.empty(),"invalid empty extended attribute name");need(name.size()<=255,"extended attribute name exceeds 255 bytes");valid_namespace_text(name);
}
std::vector<std::string> components(const std::string& path) {
    need(!path.empty() && path[0]=='/' && path.size()<=4096,"path must be absolute and at most 4096 bytes");
    std::vector<std::string> result;
    if(path=="/")return result;
    size_t begin=1;
    while(begin<path.size()) {
        auto end=path.find('/',begin);if(end==std::string::npos)end=path.size();
        auto name=path.substr(begin,end-begin);
        need(!name.empty() && name!="." && name!=".." && name.size()<=255,"invalid path component");
        valid_namespace_text(name);
        result.push_back(name);begin=end+1;
        need(end==path.size() || begin<path.size(),"trailing slash is unsupported");
    }
    return result;
}
std::vector<std::string> target_components(const std::string& target) {
    need(!target.empty() && target.size()<=4096,"invalid symlink target length");
    valid_namespace_text(target);
    std::vector<std::string> result;size_t start=0;
    while(start<target.size()) {
        auto end=target.find('/',start);if(end==std::string::npos)end=target.size();auto name=target.substr(start,end-start);
        if(!name.empty()){need(name.size()<=255,"invalid symlink component");result.push_back(name);}start=end+1;
    }
    // Preserve a target's trailing slash requirement during traversal.
    if(target.back()=='/' && !result.empty())result.push_back(".");return result;
}
struct Parent {std::string id,name,key;};
std::string walk(const NamespaceView& journal,const std::vector<std::string>& names,size_t count,
                 std::vector<std::string>* ancestry=nullptr,bool follow_final=true,Parent* missing=nullptr) {
    std::vector<std::string> stack{root(journal)};std::deque<std::string> pending(names.begin(),names.begin()+count);
    unsigned follows=0;size_t expanded=0;std::string id=stack.back();
    while(!pending.empty()) {
        auto name=pending.front();pending.pop_front();id=stack.back();need(inode(journal,id).directory,"path component is not a directory");
        if(name==".")continue;
        if(name==".."){if(stack.size()>1)stack.pop_back();id=stack.back();continue;}
        auto key=entry_key(id,name);
        if(!journal.contains(key)) {
            need(missing && pending.empty(),"path not found");*missing={id,name,key};return {};
        }
        auto child=decode_id(journal.get(key));auto n=inode(journal,child);
        if(n.symlink && (follow_final || !pending.empty())) {
            need(++follows<=40,"too many symbolic links");auto b=journal.get(content_key(child));need(b.size()==n.size,"symlink size mismatch");
            std::string target(b.begin(),b.end());auto parts=target_components(target);expanded+=target.size();need(expanded<=65536,"symlink expansion limit");
            if(target[0]=='/')stack.resize(1);
            for(auto i=parts.rbegin();i!=parts.rend();++i)pending.push_front(*i);id=stack.back();
        } else {id=child;stack.push_back(child);}
    }
    if(ancestry)*ancestry=stack;return id;
}
Parent parent(const NamespaceView& journal,const std::string& path,std::vector<std::string>* ancestry=nullptr) {
    auto names=components(path);need(!names.empty(),"cannot modify filesystem root");
    auto id=walk(journal,names,names.size()-1,ancestry);need(inode(journal,id).directory,"parent is not a directory");
    return {id,names.back(),entry_key(id,names.back())};
}
std::string resolve(const NamespaceView& journal,const std::string& path,bool follow=true,Parent* missing=nullptr) {
    auto names=components(path);return walk(journal,names,names.size(),nullptr,follow,missing);
}
InodeInfo created(InodeInfo n){n.accessed_ns=n.modified_ns;n.birth_ns=n.changed_ns;return n;}
void touch(InodeInfo& n) {n.modified_ns=n.changed_ns=now();}
}
// Type 1 remains the original inline regular file. Type 4 stores sparse 64 KiB
// chunks as ordinary CoW namespace leaves, so revision retention and GC follow
// the same immutable object graph as directory metadata. No payload heuristics.
class FileContent {
    using Changes=Namespace::Changes;
    static constexpr uint64_t chunk_size=file_chunk_bytes;
    InodeInfo& n_;
    std::function<bool(const std::string&)> contains_;
    std::function<Bytes(const std::string&)> get_;
    std::function<std::vector<std::string>(const std::string&)> keys_;
    std::string inline_,chunks_;
    Changes* changes_;
    const NamespaceView* view_=nullptr;
    bool contains(const std::string& key) const {
        if(changes_){auto it=changes_->find(key);if(it!=changes_->end())return bool(it->second);}
        if(changes_)for(const auto& [lower,upper]:changes_->erased)if(lower<=key && key<upper)return false;
        return contains_(key);
    }
    Bytes get(const std::string& key) const {
        if(changes_){auto it=changes_->find(key);if(it!=changes_->end()){need(bool(it->second),"missing file content");return *it->second;}}
        if(changes_)for(const auto& [lower,upper]:changes_->erased)need(key<lower || key>=upper,"missing file content");
        return get_(key);
    }
    std::string chunk_key(uint64_t index) const {
        return chunks_+chunk_suffix(index);
    }
    void put_chunk(uint64_t index,Bytes data) {
        while(!data.empty() && data.back()==0)data.pop_back();
        (*changes_)[chunk_key(index)]=data.empty()?std::optional<Bytes>{}:std::move(data);
    }
    void promote() {
        if(n_.chunked)return;
        auto data=get(inline_);need(data.size()==n_.size,"inode size mismatch");
        for(size_t at=0;at<data.size();at+=chunk_size)
            put_chunk(at/chunk_size,Bytes(data.begin()+at,data.begin()+std::min<uint64_t>(data.size(),at+chunk_size)));
        (*changes_)[inline_]=std::nullopt;n_.chunked=true;
    }
public:
    FileContent(InodeInfo& n,const NamespaceView& view,Changes* changes=nullptr)
        :n_(n),contains_([&view](const auto& k){return view.contains(k);}),get_([&view](const auto& k){return view.get(k);}),
         keys_([&view](const auto& k){return view.keys(k);}),inline_(content_key(n.id)),chunks_(prefix+"chunk/"+n.id+"/"),changes_(changes),view_(&view) {}
    FileContent(InodeInfo& n,const Journal& journal,const std::string& pre,Changes* changes=nullptr)
        :n_(n),contains_([&journal](const auto& k){return journal.contains(k);}),get_([&journal](const auto& k){return journal.get(k);}),
         keys_([&journal](const auto& k){return journal.keys(k);}),inline_(pre+"content"),chunks_(pre+"chunk/"),changes_(changes) {}
    uint64_t size() const {return n_.size;}
    uint64_t allocated_bytes(AllocationCache& cache) const {
        if(!n_.chunked)return ((n_.size+511)/512)*512;
        if(view_)return view_->allocated_bytes(chunks_,cache);
        uint64_t result=0;for(const auto& key:keys_(chunks_)){auto length=get(key).size();result+=((length+511)/512)*512;}
        return result;
    }
    uint64_t seek_region(uint64_t offset,bool data) const {
        need(!n_.directory && !n_.symlink,"invalid seek region inode type");
        need(offset<=INT64_MAX,"invalid seek region offset");
        if(offset>=n_.size)throw std::system_error(ENXIO,std::generic_category(),"no file region at or after offset");
        if(!n_.chunked)return data?offset:n_.size;
        auto index=offset/chunk_size;auto key=chunk_key(index);bool present=contains(key);
        if(present==data)return offset;
        // Seek over metadata leaves, never over hole bytes or payload objects.
        auto page=[&](const std::string& after,size_t limit){
            if(view_)return view_->page(chunks_,after,limit);
            auto keys=keys_(chunks_);auto begin=std::upper_bound(keys.begin(),keys.end(),after);
            auto end=begin+std::min<size_t>(limit,keys.end()-begin);return std::vector<std::string>(begin,end);
        };
        if(data){auto keys=page(key,1);if(!keys.empty()){
                uint64_t next;need(parse_chunk_suffix(std::string_view(keys[0]).substr(chunks_.size()),next),"invalid file chunk index");
                if(next*chunk_size<n_.size)return next*chunk_size;
            }
            throw std::system_error(ENXIO,std::generic_category(),"no data region at or after offset");
        }
        uint64_t next=index+1;
        while(next*chunk_size<n_.size){
            auto keys=page(key,128);if(keys.empty())return next*chunk_size;
            for(const auto& candidate:keys){uint64_t found;
                need(parse_chunk_suffix(std::string_view(candidate).substr(chunks_.size()),found),"invalid file chunk index");
                if(found!=next)return next*chunk_size;
                ++next;key=candidate;if(next*chunk_size>=n_.size)return n_.size;
            }
        }
        return n_.size;
    }
    Bytes read(uint64_t offset,uint64_t count) const {
        if(offset>=n_.size || count==0)return {};
        auto length=std::min(count,n_.size-offset);
        need(length<=Namespace::maximum_io_size,"file I/O exceeds 256 KiB");
        if(!n_.chunked){auto data=get(inline_);need(data.size()==n_.size,"inode size mismatch");return Bytes(data.begin()+offset,data.begin()+offset+length);}
        Bytes result(length,0);
        for(uint64_t at=offset;at<offset+length;) {
            auto index=at/chunk_size,start=at%chunk_size,part=std::min(chunk_size-start,offset+length-at);
            auto key=chunk_key(index);
            if(contains(key)){auto data=get(key);need(!data.empty() && data.size()<=chunk_size,"invalid file chunk");
                if(start<data.size())std::copy_n(data.begin()+start,std::min<uint64_t>(part,data.size()-start),result.begin()+at-offset);}
            at+=part;
        }
        return result;
    }
    bool write(uint64_t offset,const Bytes& value) {
        need(value.size()<=Namespace::maximum_io_size,"file I/O exceeds 256 KiB");
        if(value.empty())return false;
        need(offset<=Namespace::maximum_file_size && value.size()<=Namespace::maximum_file_size-offset,"file exceeds 1 TiB");
        auto end=offset+value.size();
        if(!n_.chunked && end<=Namespace::maximum_io_size) {
            auto data=get(inline_);need(data.size()==n_.size,"inode size mismatch");if(end>data.size())data.resize(end,0);
            std::copy(value.begin(),value.end(),data.begin()+offset);(*changes_)[inline_]=std::move(data);
        } else {
            promote();
            for(uint64_t at=offset;at<end;) {
                auto index=at/chunk_size,start=at%chunk_size,part=std::min(chunk_size-start,end-at);auto key=chunk_key(index);
                auto data=contains(key)?get(key):Bytes{};need(data.size()<=chunk_size,"invalid file chunk");
                if(data.size()<start+part)data.resize(start+part,0);
                std::copy_n(value.begin()+at-offset,part,data.begin()+start);put_chunk(index,std::move(data));at+=part;
            }
        }
        n_.size=std::max(n_.size,end);return true;
    }
    bool resize(uint64_t size) {
        need(size<=Namespace::maximum_file_size,"file exceeds 1 TiB");if(size==n_.size)return false;
        if(!n_.chunked && size<=Namespace::maximum_io_size){auto data=get(inline_);need(data.size()==n_.size,"inode size mismatch");data.resize(size,0);(*changes_)[inline_]=std::move(data);}
        else {
            promote();
            if(size<n_.size) {
                if(chunks_.starts_with(prefix)) {
                    auto first=(size+chunk_size-1)/chunk_size;
                    if(first<maximum_file_bytes/chunk_size)changes_->erase_range(chunk_key(first),prefix_end(chunks_));
                } else for(const auto& key:keys_(chunks_)) {
                    uint64_t index;need(parse_chunk_suffix(std::string_view(key).substr(chunks_.size()),index),"invalid file chunk key");
                    if(index*chunk_size>=size)(*changes_)[key]=std::nullopt;
                }
                if(size%chunk_size){auto index=size/chunk_size;auto key=chunk_key(index);
                    if(contains(key)){auto data=get(key);if(data.size()>size%chunk_size){data.resize(size%chunk_size);put_chunk(index,std::move(data));}}}
            }
        }
        n_.size=size;return true;
    }
    std::vector<std::string> records() const {return n_.chunked?keys_(chunks_):std::vector<std::string>{inline_};}
    void verify() const {
        need(n_.size<=Namespace::maximum_file_size,"invalid file size");
        if(!n_.chunked){need(get(inline_).size()==n_.size && n_.size<=Namespace::maximum_io_size,"inode content size mismatch");return;}
        need(!contains(inline_),"chunked inode also has inline content");
        for(const auto& key:keys_(chunks_)) {
            uint64_t index;need(parse_chunk_suffix(std::string_view(key).substr(chunks_.size()),index),"invalid file chunk key");
            need(index*chunk_size<n_.size,"file chunk beyond EOF");
            auto data=get(key);need(!data.empty() && data.size()<=std::min<uint64_t>(chunk_size,n_.size-index*chunk_size) && data.back()!=0,"invalid file chunk");
        }
    }
};

namespace {
class Xattrs {
    using Changes=std::map<std::string,std::optional<Bytes>>;
    std::function<bool(const std::string&)> contains_;
    std::function<Bytes(const std::string&)> get_;
    std::function<uint64_t(const std::string&)> size_;
    std::function<std::vector<std::string>(const std::string&)> keys_;
    std::string prefix_;
    Changes* changes_;
public:
    Xattrs(const NamespaceView& view,const std::string& id,Changes* changes=nullptr)
        :contains_([&view](const auto& key){return view.contains(key);}),get_([&view](const auto& key){return view.get(key);}),size_([&view](const auto& key){return view.value_size(key);}),
         keys_([&view](const auto& key){return view.keys(key);}),prefix_(xattr_prefix(id)),changes_(changes) {}
    Xattrs(const Journal& journal,const std::string& orphan,Changes* changes=nullptr)
        :contains_([&journal](const auto& key){return journal.contains(key);}),get_([&journal](const auto& key){return journal.get(key);}),size_([&journal](const auto& key){return journal.object_size(journal.root_object(key));}),
         keys_([&journal](const auto& key){return journal.keys(key);}),prefix_(orphan+"xattr/"),changes_(changes) {}
    std::vector<std::string> records() const {return keys_(prefix_);}
    Bytes get(const std::string& name) const {
        valid_xattr_name(name);auto key=prefix_+name;need(contains_(key),"extended attribute not found");auto value=get_(key);
        need(value.size()<=maximum_xattr_bytes,"extended attribute exceeds 262143 bytes");return value;
    }
    std::vector<std::string> names() const {
        auto keys=records();need(keys.size()<=Namespace::maximum_xattr_count,"extended attribute count limit reached");std::vector<std::string> result;
        uint64_t total=0;
        for(const auto& key:keys){auto name=key.substr(prefix_.size());valid_xattr_name(name);result.push_back(name);
            auto length=size_(key);need(length<=Namespace::maximum_xattr_storage_bytes-total,"extended attribute storage limit reached");total+=length;}
        return result;
    }
    bool edit(const std::string& name,const std::optional<Bytes>& value,Namespace::XattrPolicy policy) {
        valid_xattr_name(name);need(policy==Namespace::XattrPolicy::Upsert || policy==Namespace::XattrPolicy::Create || policy==Namespace::XattrPolicy::Replace,"invalid extended attribute policy");
        auto key=prefix_+name;bool exists=contains_(key);auto keys=records();auto count=keys.size();
        if(value){need(value->size()<=maximum_xattr_bytes,"extended attribute exceeds 262143 bytes");
            need(policy!=Namespace::XattrPolicy::Create || !exists,"extended attribute already exists");
            need(policy!=Namespace::XattrPolicy::Replace || exists,"extended attribute not found");
            need(exists || count<Namespace::maximum_xattr_count,"extended attribute count limit reached");
            uint64_t total=value->size();
            for(const auto& other:keys)if(other!=key){auto length=size_(other);
                need(length<=Namespace::maximum_xattr_storage_bytes-total,"extended attribute storage limit reached");total+=length;}
        } else need(exists,"extended attribute not found");
        (*changes_)[key]=value;return value.has_value() || count>1;
    }
    void verify(bool flag) const {auto values=names();need(flag==!values.empty(),"invalid extended attribute inode flag");for(const auto& name:values)(void)get(name);}
};
}
Namespace::XattrPolicy Namespace::xattr_policy(const std::string& value) {
    if(value=="upsert")return XattrPolicy::Upsert;if(value=="create")return XattrPolicy::Create;if(value=="replace")return XattrPolicy::Replace;
    throw std::runtime_error("invalid extended attribute policy");
}
std::pair<InodeInfo,bool> Namespace::xattr_inode(const NamespaceView& view,const std::string& target,bool handle) const {
    if(!handle)return {inode(view,resolve(view,target,false)),true};
    const auto& info=find_handle(target);bool linked=view.contains(inode_key(info.inode));
    return {linked?inode(view,info.inode):decode_inode(container_.journal_->get(orphan_prefix(view_,info.inode)+"inode"),info.inode,true),linked};
}
Bytes Namespace::read_xattr(const std::string& target,const std::string& name,bool handle) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);auto [n,linked]=xattr_inode(view,target,handle);
    if(!linked)if(auto retained=retained_orphan(*container_.journal_,orphan_prefix(view_,n.id)))return Xattrs(*retained,n.id).get(name);
    return linked?Xattrs(view,n.id).get(name):Xattrs(*container_.journal_,orphan_prefix(view_,n.id)).get(name);
}
std::vector<std::string> Namespace::read_xattr_names(const std::string& target,bool handle) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);auto [n,linked]=xattr_inode(view,target,handle);
    if(!linked)if(auto retained=retained_orphan(*container_.journal_,orphan_prefix(view_,n.id)))return Xattrs(*retained,n.id).names();
    return linked?Xattrs(view,n.id).names():Xattrs(*container_.journal_,orphan_prefix(view_,n.id)).names();
}
void Namespace::edit_xattr(const std::string& target,const std::string& name,const std::optional<Bytes>& value,XattrPolicy policy,bool handle) {
    need(!revision_,"revision view is read-only");
    auto edit=[&](const NamespaceView& view,Changes& changes){auto [n,linked]=xattr_inode(view,target,handle);auto pre=orphan_prefix(view_,n.id);
        auto attrs=linked?Xattrs(view,n.id,&changes):Xattrs(*container_.journal_,pre,&changes);
        n.xattrs=attrs.edit(name,value,policy);n.changed_ns=now();changes[linked?inode_key(n.id):pre+"inode"]=encode(n);
    };
    if(!handle){update(edit);return;}
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);Changes changes;
    auto [n,linked]=xattr_inode(view,target,true);auto pre=orphan_prefix(view_,n.id);
    if(!linked)if(auto retained=retained_orphan(*container_.journal_,pre)) {
        n.xattrs=Xattrs(*retained,n.id,&changes).edit(name,value,policy);n.changed_ns=now();
        commit_orphan_changes(*retained,pre,n,changes);return;
    }
    edit(view,changes);commit_changes(view,changes);
}

void Namespace::update(const std::function<void(const NamespaceView&,Changes&)>& operation) {
    need(!revision_,"revision view is read-only");
    bool offline_compacted=false;
    for(;;) {
    std::unique_lock lock(container_.mutex_);auto& journal=*container_.journal_;
    NamespaceView view(journal,view_,revision_);
    Changes changes;operation(view,changes);if(changes.empty())return;
    bool background=container_.compaction_running_ && !container_.compaction_stop_;
    if(!background && !offline_compacted && view.run_count()>=NamespaceView::soft_run_limit) {
        lock.unlock();compact();offline_compacted=true;continue; // Offline CLI fallback; no merge under the write lock.
    }
    if(background && view.run_count()>=NamespaceView::hard_run_limit) {
        container_.schedule_compaction_locked(view_);
        auto before=journal.root_object(history+"world/"+view_);
        auto failures=container_.compaction_world_failures_[view_];
        bool progress=container_.compaction_cv_.wait_for(lock,std::chrono::seconds(10),[&] {
            return container_.compaction_stop_ || container_.compaction_world_failures_[view_]!=failures ||
                journal.root_object(history+"world/"+view_)!=before;
        });
        need(progress,"compaction backpressure timeout");
        need(container_.compaction_world_failures_[view_]==failures,"background compaction failed; inspect service status");
        continue;
    }
    need(view.run_count()<NamespaceView::hard_run_limit,"namespace run capacity exhausted");
    commit_changes(view,changes);
    if(background) {
        NamespaceView updated(journal,view_,false,false);
        if(updated.run_count()>=NamespaceView::soft_run_limit)container_.schedule_compaction_locked(view_);
    }
    container_.compaction_cv_.notify_all();return;
    }
}

void Namespace::commit_changes(const NamespaceView& view,Changes& changes) {
    if(changes.empty())return;auto prepare_start=std::chrono::steady_clock::now();
    auto& journal=*container_.journal_;
    std::vector<Mutation> batch;
    // Initialization alone retains compatibility with the original flat format.
    if(view.legacy() && !view.contains(root_key)) {
        for(auto& [key,value]:changes)batch.push_back({key,std::move(value)});
    } else {
        NamespaceView::Changes delta;std::map<std::string,Bytes> new_objects;
        const auto allocated=prefix+"allocated/";
        for(const auto& [key,value]:changes) {
            if(key.starts_with(history+"orphan/")){
                if(!value && !journal.contains(key))continue;
                if(value){auto id=Journal::object_id(*value);if(journal.contains(history+"object/"+hex_id(id))){batch.push_back({key,std::nullopt,id});continue;}}
                batch.push_back({key,value});continue;}
            if(value) {
                auto id=Journal::object_id(*value);delta[key]=id;
                auto object_key=history+"object/"+hex_id(id);
                if(!journal.contains(object_key))new_objects.emplace(object_key,*value);
                if(key.starts_with(allocated))new_objects.emplace(history+"allocated/"+key.substr(allocated.size()),Bytes{});
            } else delta[key]=std::nullopt;
        }
        if(!delta.empty() || !changes.erased.empty()) {
            auto plan=view.plan(delta,false,changes.erased,container_.tree_height_cache_.get());
            for(auto& payload:plan.objects) {
                auto id=Journal::object_id(payload);auto key=history+"object/"+hex_id(id);
                if(!journal.contains(key))new_objects.emplace(key,std::move(payload));
            }
            for(auto& [key,value]:new_objects)batch.push_back({key,std::move(value)});
            batch.push_back({history+"world/"+view_,std::move(plan.manifest)});
        }
    }
    container_.metadata_prepare_ns_+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-prepare_start).count();
    journal.transact(journal.sequence(),batch);
}

void Namespace::commit_orphan_changes(const NamespaceView& retained,const std::string& pre,const InodeInfo& n,Changes& changes) {
    auto& journal=*container_.journal_;std::vector<Mutation> batch;
    if(!changes.empty()) {
        NamespaceView::Changes delta;std::map<std::string,Bytes> objects;
        for(const auto& [key,value]:changes) {
            if(value){auto id=Journal::object_id(*value);delta[key]=id;
                auto name=history+"object/"+hex_id(id);if(!journal.contains(name))objects.emplace(name,*value);}
            else delta[key]=std::nullopt;
        }
        auto plan=retained.plan(delta,false,changes.erased,container_.tree_height_cache_.get());
        for(auto& value:plan.objects){auto name=history+"object/"+hex_id(Journal::object_id(value));if(!journal.contains(name))objects.emplace(name,std::move(value));}
        for(auto& [key,value]:objects)batch.push_back({key,std::move(value)});
        batch.push_back({pre+"tree",std::move(plan.manifest)});
    }
    batch.push_back({pre+"inode",encode(n)});journal.transact(journal.sequence(),batch);
}

std::string Namespace::diff(const std::string& before,const std::string& after,const std::string& after_path,size_t limit) const {
    need(limit>=1 && limit<=64,"invalid diff page limit; expected 1..64");
    if(!after_path.empty())components(after_path);
    valid_view_name(before);valid_view_name(after);
    std::lock_guard lock(container_.mutex_);const auto& journal=*container_.journal_;
    auto before_manifest=revision_manifest(journal,before),after_manifest=revision_manifest(journal,after);
    NamespaceView left(journal,before_manifest),right(journal,after_manifest);
    auto quote=[](const std::string& value){std::string result="\"";for(char c:value){if(c=='"' || c=='\\')result+='\\';result+=c;}return result+'"';};
    auto type=[&](const std::optional<InodeInfo>& n){return n?quote(n->directory?"directory":n->symlink?"symlink":"file"):std::string("null");};
    auto identity=[&](const std::optional<InodeInfo>& n){return n?quote(n->id):std::string("null");};
    auto same_metadata=[](const InodeInfo& a,const InodeInfo& b){return a.id==b.id && a.directory==b.directory && a.symlink==b.symlink &&
        a.mode==b.mode && a.flags==b.flags && a.links==b.links && a.size==b.size && a.modified_ns==b.modified_ns && a.changed_ns==b.changed_ns && a.accessed_ns==b.accessed_ns && a.birth_ns==b.birth_ns;};
    auto attribute_ids=[](const NamespaceView& view,const std::optional<InodeInfo>& n){
        std::map<std::string,ObjectId> result;if(n){auto pre=xattr_prefix(n->id);for(const auto& key:view.keys(pre))result.emplace(key.substr(pre.size()),*view.reference(key));}return result;
    };
    auto same_content=[&](InodeInfo& a,InodeInfo& b){
        if(a.directory || b.directory)return a.directory==b.directory;
        if(a.symlink!=b.symlink || a.size!=b.size)return false;
        need(a.size<=maximum_file_size,"invalid file size");
        // Equal object IDs prove byte equality, independently of inode metadata.
        if(!a.chunked && !b.chunked){auto a_ref=left.reference(content_key(a.id)),b_ref=right.reference(content_key(b.id));
            need(a_ref.has_value() && b_ref.has_value(),"inode content missing");if(a_ref==b_ref)return true;}
        FileContent a_data(a,left),b_data(b,right);
        if(a.chunked && b.chunked) {
            // Holes have no records. Compare only the union of allocated chunk
            // positions, so a 1 TiB sparse file does not require 16M lookups.
            std::map<uint64_t,std::pair<std::optional<ObjectId>,std::optional<ObjectId>>> chunks;
            auto collect=[&](const NamespaceView& view,const InodeInfo& n,bool old){auto pre=prefix+"chunk/"+n.id+"/";
                for(const auto& key:view.keys(pre)){uint64_t index;need(parse_chunk_suffix(std::string_view(key).substr(pre.size()),index),"invalid file chunk key");
                    need(index*file_chunk_bytes<n.size,"file chunk beyond EOF");auto& refs=chunks[index];(old?refs.first:refs.second)=view.reference(key);}};
            collect(left,a,true);collect(right,b,false);
            for(const auto& [index,refs]:chunks)if(refs.first!=refs.second && a_data.read(index*file_chunk_bytes,file_chunk_bytes)!=b_data.read(index*file_chunk_bytes,file_chunk_bytes))return false;
            return true;
        }
        for(uint64_t offset=0;offset<a.size;offset+=file_chunk_bytes) {
            // Inline and chunked representations (or distinct chunk objects)
            // are compared as bounded logical byte ranges, including zero holes.
            if(a_data.read(offset,file_chunk_bytes)!=b_data.read(offset,file_chunk_bytes))return false;
        }
        return true;
    };
    std::vector<std::string> changes;std::string last;bool eof=true;
    if(before_manifest!=after_manifest) {
        using Pair=std::pair<std::string,std::string>;
        std::map<std::string,Pair> pending{{"/",{root(left),root(right)}}};std::set<std::string> left_dirs,right_dirs;
        while(!pending.empty()) {
            auto entry=pending.extract(pending.begin());auto path=std::move(entry.key());auto [a_id,b_id]=std::move(entry.mapped());
            std::optional<InodeInfo> a=a_id.empty()?std::nullopt:std::optional(inode(left,a_id));
            std::optional<InodeInfo> b=b_id.empty()?std::nullopt:std::optional(inode(right,b_id));
            if(path>after_path){bool attrs_changed=attribute_ids(left,a)!=attribute_ids(right,b);bool metadata=attrs_changed || !a || !b || !same_metadata(*a,*b);
                bool content=!a? !b->directory:!b? !a->directory:!same_content(*a,*b);
                if(metadata || content) {
                    if(changes.size()==limit){eof=false;break;}
                    changes.push_back("{\"path\":"+quote(path)+",\"change\":"+quote(!a?"added":!b?"removed":"modified")+
                        ",\"before_inode\":"+identity(a)+",\"after_inode\":"+identity(b)+
                        ",\"before_type\":"+type(a)+",\"after_type\":"+type(b)+
                        ",\"xattrs_changed\":"+(attrs_changed?"true":"false")+",\"metadata_changed\":"+(metadata?"true":"false")+",\"content_changed\":"+(content?"true":"false")+"}");last=path;
                }
            }
            auto children=[&](const NamespaceView& view,const std::optional<InodeInfo>& n,std::set<std::string>& visited,bool old){
                if(!n || !n->directory)return;
                need(visited.insert(n->id).second,"directory cycle or hardlink");auto pre=directory_prefix(n->id);
                for(const auto& key:view.keys(pre)) {
                    auto name=key.substr(pre.size());auto parsed=components("/"+name);need(parsed.size()==1,"invalid directory name");
                    auto child_path=path=="/"?path+name:path+"/"+name;need(child_path.size()<=4096,"path exceeds mount protocol limit");
                    auto& pair=pending[child_path];(old?pair.first:pair.second)=decode_id(view.get(key));
                }
            };
            children(left,a,left_dirs,true);children(right,b,right_dirs,false);
        }
    }
    std::string result="{\"before_revision_id\":"+quote(hex_id(journal.root_object(history+"revision/"+before)))+
        ",\"after_revision_id\":"+quote(hex_id(journal.root_object(history+"revision/"+after)))+",\"changes\":[";
    for(size_t i=0;i<changes.size();++i){if(i)result+=',';result+=changes[i];}
    return result+"],\"next_after\":"+quote(eof?"":last)+",\"eof\":"+(eof?"true":"false")+"}\n";
}

std::string Namespace::revision_history(const std::string& tip,const std::string& after_id,size_t limit) const {
    valid_view_name(tip);need(limit>=1 && limit<=64,"invalid history page limit; expected 1..64");
    need(after_id.empty() || (after_id.size()==64 && std::all_of(after_id.begin(),after_id.end(),[](char c){
        return (c>='0' && c<='9') || (c>='a' && c<='f');})),"invalid history cursor");
    std::lock_guard lock(container_.mutex_);const auto& journal=*container_.journal_;
    auto tip_id=journal.root_object(history+"revision/"+tip),current=tip_id;
    std::set<ObjectId> visited;std::optional<uint64_t> newer_sequence;
    bool found=after_id.empty(),eof=true;std::vector<std::string> entries;std::string last;
    while(current!=ObjectId{}) {
        need(visited.insert(current).second,"invalid revision ancestry cycle");
        auto descriptor=journal.get_object(current);
        need(descriptor.size()==80 && !memcmp(descriptor.data(),"FFREV001",8),"invalid revision record");
        ObjectId manifest{},parent{};std::copy_n(descriptor.data()+8,32,manifest.begin());std::copy_n(descriptor.data()+40,32,parent.begin());
        auto sequence=load(descriptor.data()+72,8);
        need(manifest!=ObjectId{} && (!newer_sequence || sequence<*newer_sequence),"invalid revision ancestry sequence");newer_sequence=sequence;
        auto id=hex_id(current);
        if(!found){if(id==after_id)found=true;current=parent;continue;}
        if(entries.size()==limit){eof=false;break;}
        entries.push_back("{\"revision_id\":\""+id+"\",\"parent_revision_id\":"+(parent==ObjectId{}?"null":"\""+hex_id(parent)+"\"")+
            ",\"manifest_id\":\""+hex_id(manifest)+"\",\"captured_sequence\":"+std::to_string(sequence)+"}");
        last=id;current=parent;
    }
    need(found,"invalid history cursor; revision is not in tip ancestry");
    std::string result="{\"tip_revision_id\":\""+hex_id(tip_id)+"\",\"entries\":[";
    for(size_t i=0;i<entries.size();++i){if(i)result+=',';result+=entries[i];}
    return result+"],\"next_after\":\""+(eof?"":last)+"\",\"eof\":"+(eof?"true":"false")+"}\n";
}

ObjectId Namespace::prepare_snapshot_locked(const NamespaceView& view,const std::string& name,std::vector<Mutation>& batch) {
    auto& j=*container_.journal_;
    need(!j.contains(history+"revision/"+name),"revision name already exists");
    root(view);
    ObjectId manifest_id{};
    if(view.legacy()) {
        auto plan=view.plan({});
        for(auto& payload:plan.objects) {
            auto id=Journal::object_id(payload);auto key=history+"object/"+hex_id(id);
            if(!j.contains(key))batch.push_back({key,std::move(payload)});
        }
        manifest_id=Journal::object_id(plan.manifest);auto key=history+"object/"+hex_id(manifest_id);
        if(!j.contains(key))batch.push_back({key,std::move(plan.manifest)});
    } else manifest_id=j.root_object(history+"world/"+view_);
    ObjectId parent_id{};auto base=history+"base/"+view_;
    if(j.contains(base)) {
        auto parent=j.get(base);need(parent.size()==32,"invalid revision ancestry");
        std::copy_n(parent.data(),32,parent_id.begin());
        auto descriptor=j.get_object(parent_id);
        need(descriptor.size()==80 && !memcmp(descriptor.data(),"FFREV001",8),"invalid parent revision");
    }
    Bytes descriptor(80);memcpy(descriptor.data(),"FFREV001",8);
    memcpy(descriptor.data()+8,manifest_id.data(),32);memcpy(descriptor.data()+40,parent_id.data(),32);
    store(descriptor.data()+72,j.sequence(),8);auto revision_id=Journal::object_id(descriptor);
    batch.push_back({history+"revision/"+name,std::move(descriptor)});
    batch.push_back({base,Bytes(revision_id.begin(),revision_id.end())});
    return revision_id;
}
std::string Namespace::snapshot(const std::string& name) {
    valid_view_name(name);need(!revision_,"revision view is read-only");
    std::lock_guard lock(container_.mutex_);auto& j=*container_.journal_;NamespaceView view(j,view_,false);std::vector<Mutation> batch;
    auto id=prepare_snapshot_locked(view,name,batch);j.transact(j.sequence(),batch);return hex_id(id);
}
std::string Namespace::restore(const std::string& revision,const std::string& safety_revision) {
    valid_view_name(revision);valid_view_name(safety_revision);need(!revision_,"revision view is read-only");
    std::lock_guard lock(container_.mutex_);auto& j=*container_.journal_;
    for(const auto& [owner,count]:container_.open_counts_)
        need(owner.first!=view_ || count==0,"World restore has outstanding handles; close them first");
    // Verify and publish under one lock. Directory cursors retain their captured
    // immutable trees; mutable file handles must close before a World reset.
    Namespace target(container_,revision,true);target.verify_locked();auto manifest=revision_manifest(j,revision);
    auto target_id=j.root_object(history+"revision/"+revision);NamespaceView current(j,view_,false);std::vector<Mutation> batch;
    auto safety_id=prepare_snapshot_locked(current,safety_revision,batch);
    batch.push_back({history+"world/"+view_,std::nullopt,manifest});
    // prepare_snapshot_locked advances base to the safety revision, preserving
    // the World timeline. A later snapshot records restored data as its child.
    j.transact(j.sequence(),batch);container_.compaction_cv_.notify_all();
    return "{\"safety_revision_id\":\""+hex_id(safety_id)+"\",\"target_revision_id\":\""+hex_id(target_id)+"\",\"restored_manifest_id\":\""+hex_id(manifest)+"\"}\n";
}
void Namespace::fork(const std::string& revision,const std::string& world) {
    valid_view_name(revision);valid_view_name(world);need(world!="main","main World already exists");
    need(!revision_,"revision view is read-only");
    std::lock_guard lock(container_.mutex_);auto& j=*container_.journal_;
    need(!j.contains(history+"world/"+world) && !j.contains(history+"base/"+world),"World already exists");
    auto manifest=revision_manifest(j,revision);NamespaceView validated(j,revision,true);root(validated);
    auto revision_id=j.root_object(history+"revision/"+revision);
    j.transact(j.sequence(),{{history+"world/"+world,std::nullopt,manifest},
                            {history+"base/"+world,Bytes(revision_id.begin(),revision_id.end())}});
}
void Namespace::require_offline_locked() const {
    need(view_=="main" && !revision_,"retention commands take explicit names, not view selectors");
    need(container_.handles_.empty() && !container_.compaction_running_,"retention requires an idle offline repository");
    for(const auto& pin:container_.directory_pins_)need(pin.expired(),"retention requires closed directory cursors");
}
std::string Namespace::drop_world(const std::string& name) {
    valid_view_name(name);need(name!="main","cannot drop main World");
    std::lock_guard lock(container_.mutex_);require_offline_locked();auto& j=*container_.journal_;
    auto head=history+"world/"+name,base=history+"base/"+name;
    need(j.contains(head) && j.contains(base),"World does not exist or has incomplete roots");
    auto manifest=j.root_object(head);auto parent=j.get(base);need(parent.size()==32,"invalid World base");
    ObjectId parent_id{};std::copy(parent.begin(),parent.end(),parent_id.begin());
    j.transact(j.sequence(),{{head,std::nullopt},{base,std::nullopt}});
    return "{\"world\":\""+name+"\",\"manifest_id\":\""+hex_id(manifest)+"\",\"base_revision_id\":\""+hex_id(parent_id)+"\",\"collected_objects\":0}\n";
}
std::string Namespace::forget_revision(const std::string& name) {
    valid_view_name(name);
    std::lock_guard lock(container_.mutex_);require_offline_locked();auto& j=*container_.journal_;
    auto key=history+"revision/"+name;need(j.contains(key),"revision does not exist");auto id=j.root_object(key);
    // Only the label is removed. Immutable parent IDs and World bases remain
    // intact, and the collector continues tracing all surviving roots.
    j.transact(j.sequence(),{{key,std::nullopt}});
    return "{\"revision\":\""+name+"\",\"revision_id\":\""+hex_id(id)+"\",\"collected_objects\":0}\n";
}
std::vector<std::string> Namespace::worlds() const {
    std::lock_guard lock(container_.mutex_);const auto& j=*container_.journal_;std::vector<std::string> result;
    if(j.contains(root_key))result.push_back("main");auto p=history+"world/";
    for(const auto& key:j.keys(p)){auto name=key.substr(p.size());valid_view_name(name);if(name!="main")result.push_back(name);}
    return result;
}
std::vector<std::string> Namespace::revisions() const {
    std::lock_guard lock(container_.mutex_);const auto& j=*container_.journal_;std::vector<std::string> result;
    auto p=history+"revision/";
    for(const auto& key:j.keys(p)){auto name=key.substr(p.size());valid_view_name(name);result.push_back(name);}
    return result;
}
void Namespace::compact() {
    need(!revision_,"revision view is read-only");
    {
        std::lock_guard lock(container_.mutex_);
        if(container_.journal_->is_leveldb()) {
            NamespaceView view(*container_.journal_,view_,false);root(view);
            container_.journal_->compact();return;
        }
    }
    for(unsigned retry=0;retry<8;++retry) {
        Compaction task(container_,view_);task.prepare();task.build();
        if(task.install())return;
    }
    throw std::runtime_error("compaction busy: World head keeps changing");
}
size_t Namespace::run_count() const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);root(view);return view.run_count();
}
void Namespace::initialize() {
    need(view_=="main" && !revision_,"fs-init initializes main only");
    update([](const NamespaceView& j,Changes& changes) {
        need(!j.contains(root_key),"filesystem already initialized");
        need(j.keys(prefix).empty(),"orphan filesystem records prevent initialization");
        auto id=new_id(j);InodeInfo n=created({id,true,0755,2,0,now(),now()});
        changes[root_key]=id_bytes(id);changes[inode_key(id)]=encode(n);changes[allocated_key(id)]=Bytes{};
    });
}
void Namespace::mkdir(const std::string& path,uint32_t mode) {
    need(mode<=0777,"invalid creation mode");
    update([&](const NamespaceView& j,Changes& c) {
        auto p=parent(j,path);need(!j.contains(p.key),"path already exists");
        auto id=new_id(j);InodeInfo n=created({id,true,mode,2,0,now(),now()}),pn=inode(j,p.id);
        need(pn.links<UINT64_MAX,"directory link count exhausted");++pn.links;touch(pn);
        c[p.key]=id_bytes(id);c[inode_key(id)]=encode(n);c[inode_key(p.id)]=encode(pn);c[allocated_key(id)]=Bytes{};
    });
}
void Namespace::write(const std::string& path,const Bytes& value) {
    need(value.size()<=256*1024,"file I/O exceeds 256 KiB");
    update([&](const NamespaceView& j,Changes& c) {
        auto p=parent(j,path);bool exists=j.contains(p.key);auto id=exists?decode_id(j.get(p.key)):new_id(j);
        if(exists && inode(j,id).symlink) {
            Parent destination;id=resolve(j,path,true,&destination);
            if(id.empty()){p=std::move(destination);exists=false;id=new_id(j);}
        }
        auto n=exists?inode(j,id):created({id,false,0644,1,0,now(),now()});
        need(!n.directory && !n.symlink,"cannot write non-regular file");if(n.chunked){for(const auto& key:FileContent(n,j).records())c[key]=std::nullopt;n.chunked=false;}n.size=value.size();touch(n);
        c[inode_key(id)]=encode(n);c[content_key(id)]=value;
        if(!exists){auto pn=inode(j,p.id);touch(pn);c[inode_key(p.id)]=encode(pn);c[p.key]=id_bytes(id);c[allocated_key(id)]=Bytes{};}
    });
}
void Namespace::replace_file(const std::string& path,const Bytes& value) {
    need(value.size()<=256*1024,"file I/O exceeds 256 KiB");
    update([&](const NamespaceView& view,Changes& changes) {
        auto p=parent(view,path);uint32_t mode=0644;
        if(view.contains(p.key)) {
            auto old=inode(view,decode_id(view.get(p.key)));need(!old.directory,"cannot replace directory with a file");
            if(!old.symlink)mode=old.mode;
            if(old.links==1)retire(view,changes,old);
            else {--old.links;old.changed_ns=now();changes[inode_key(old.id)]=encode(old);}
        }
        auto id=new_id(view);InodeInfo n=created({id,false,mode,1,value.size(),now(),now()});auto pn=inode(view,p.id);touch(pn);
        changes[inode_key(id)]=encode(n);changes[content_key(id)]=value;changes[allocated_key(id)]=Bytes{};
        changes[p.key]=id_bytes(id);changes[inode_key(p.id)]=encode(pn);
    });
}
Bytes Namespace::read(const std::string& path) const {return read_at(path,0,UINT64_MAX);}
void Namespace::edit_content(const std::string& path,const std::function<bool(FileContent&)>& edit) {
    update([&](const NamespaceView& view,Changes& changes) {
        auto id=resolve(view,path);auto n=inode(view,id);need(!n.directory && !n.symlink,"cannot modify directory content");
        FileContent data(n,view,&changes);if(!edit(data))return;
        touch(n);changes[inode_key(id)]=encode(n);
    });
}
void Namespace::write_at(const std::string& path,uint64_t offset,const Bytes& value) {
    need(offset<=INT64_MAX,"file offset exceeds signed 64-bit range");
    edit_content(path,[&](FileContent& data){return data.write(offset,value);});
}
uint64_t Namespace::append(const std::string& path,const Bytes& value) {
    uint64_t offset=0;edit_content(path,[&](FileContent& data){offset=data.size();return data.write(offset,value);});return offset;
}
void Namespace::truncate(const std::string& path,uint64_t size) {
    need(size<=maximum_file_size,"file exceeds 1 TiB");
    edit_content(path,[&](FileContent& data){return data.resize(size);});
}
Bytes Namespace::read_at(const std::string& path,uint64_t offset,uint64_t count) const {
    need(offset<=INT64_MAX,"file offset exceeds signed 64-bit range");
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);auto n=inode(view,resolve(view,path));
    need(!n.directory,"cannot read a directory");return FileContent(n,view).read(offset,count);
}
uint64_t Namespace::seek_region(const std::string& path,uint64_t offset,bool data) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);
    auto n=inode(view,resolve(view,path));return FileContent(n,view).seek_region(offset,data);
}
uint64_t Namespace::handle_seek_region(const std::string& token,uint64_t offset,bool data) const {
    std::lock_guard lock(container_.mutex_);const auto& info=find_handle(token);const auto& j=*container_.journal_;
    NamespaceView view(j,view_,revision_);
    if(view.contains(inode_key(info.inode))){auto n=inode(view,info.inode);return FileContent(n,view).seek_region(offset,data);}
    need(!revision_,"revision inode missing");auto pre=orphan_prefix(view_,info.inode);
    auto n=decode_inode(j.get(pre+"inode"),info.inode,true);
    if(auto retained=retained_orphan(j,pre))return FileContent(n,*retained).seek_region(offset,data);
    return FileContent(n,j,pre).seek_region(offset,data);
}
std::vector<std::string> Namespace::list(const std::string& path) const {
    std::lock_guard lock(container_.mutex_);const auto& journal=*container_.journal_;NamespaceView j(journal,view_,revision_);auto id=resolve(j,path);
    need(inode(j,id).directory,"path is not a directory");auto pre=directory_prefix(id);std::vector<std::string> result;
    for(const auto& key:j.keys(pre))result.push_back(key.substr(pre.size()));return result;
}
Namespace::DirectoryCursor Namespace::open_directory(const std::string& path) const {
    std::lock_guard lock(container_.mutex_);const auto& j=*container_.journal_;need(j.is_leveldb(),"directory cursors require LevelDB");
    NamespaceView view(j,view_,revision_);auto id=resolve(view,path);need(inode(view,id).directory,"path is not a directory");
    DirectoryCursor cursor;cursor.lifetime_=container_.lifetime_;cursor.view_=view_;cursor.revision_=revision_;cursor.prefix_=directory_prefix(id);
    if(view.legacy()){need(view.keys(cursor.prefix_).empty(),"flat namespace directory is not empty");cursor.done_=true;}
    else cursor.manifest_=revision_?revision_manifest(j,view_):j.root_object(history+"world/"+view_);
    cursor.gc_pin_=std::make_shared<const int>(0);
    std::erase_if(container_.directory_pins_,[](const auto& pin){return pin.expired();});
    container_.directory_pins_.push_back(cursor.gc_pin_);
    return cursor;
}
Namespace::DirectoryPage Namespace::read_directory(DirectoryCursor& cursor,size_t limit,bool attributes) const {
    need(limit>=1 && limit<=1024,"directory page limit must be 1..1024");
    std::lock_guard lock(container_.mutex_);need(cursor.lifetime_.lock()==container_.lifetime_ && cursor.view_==view_ && cursor.revision_==revision_,"directory cursor belongs to another repository/view");
    if(cursor.done_)return {{},true,{}};NamespaceView snapshot(*container_.journal_,cursor.manifest_);
    auto keys=snapshot.page(cursor.prefix_,cursor.after_,limit+1);bool eof=keys.size()<=limit;if(!eof)keys.resize(limit);
    std::vector<std::string> names;std::vector<DirectoryEntry> entries;
    for(const auto& key:keys) {
        auto name=key.substr(cursor.prefix_.size());names.push_back(name);
        if(attributes){auto n=inode(snapshot,decode_id(snapshot.get(key)));allocation_info(n,snapshot);entries.push_back({name,n});}
    }
    if(!keys.empty())cursor.after_=keys.back();cursor.done_=eof;return {std::move(names),eof,std::move(entries)};
}
void Namespace::rewind_directory(DirectoryCursor& cursor) const {
    std::lock_guard lock(container_.mutex_);need(cursor.lifetime_.lock()==container_.lifetime_ && cursor.view_==view_ && cursor.revision_==revision_,"directory cursor belongs to another repository/view");
    cursor.after_.clear();cursor.done_=cursor.manifest_==ObjectId{};
}
InodeInfo Namespace::stat(const std::string& path) const {
    std::lock_guard lock(container_.mutex_);const auto& journal=*container_.journal_;NamespaceView j(journal,view_,revision_);auto n=inode(j,resolve(j,path));allocation_info(n,j);remember_path(n.id,path);return n;
}
void Namespace::allocation_info(InodeInfo& n,const NamespaceView& view) const {
    n.allocated_size=n.directory?0:FileContent(n,view).allocated_bytes(*container_.allocation_cache_);
    need(n.allocated_size<=maximum_file_bytes,"invalid allocation size");
}
namespace {
void valid_attributes(const Namespace::Attributes& a) {
    need(!a.mode || *a.mode<=0777,"unsupported mode bits");
    need(!a.flags || !(*a.flags&~supported_file_flags),"unsupported file flags");
    need(!a.size || *a.size<=Namespace::maximum_file_size,"file exceeds 1 TiB");
    need(!a.modified_ns || *a.modified_ns<=INT64_MAX,"mtime exceeds supported nanosecond range");
    need(!a.accessed_ns || *a.accessed_ns<=INT64_MAX,"atime exceeds supported nanosecond range");
    need(!a.birth_ns || *a.birth_ns<=INT64_MAX,"birth time exceeds supported nanosecond range");
}
void apply_attributes(InodeInfo& n,const Namespace::Attributes& a) {
    if(a.flags)n.flags=*a.flags;if(a.mode)n.mode=*a.mode;if(a.modified_ns)n.modified_ns=*a.modified_ns;n.changed_ns=now();
    if(a.accessed_ns)n.accessed_ns=*a.accessed_ns;if(a.birth_ns)n.birth_ns=*a.birth_ns;
}
}
void Namespace::set_attributes(const std::string& path,const Attributes& a) {
    set_path_attributes(path,a,true);
}
void Namespace::set_link_attributes(const std::string& path,const Attributes& a) {
    set_path_attributes(path,a,false);
}
void Namespace::set_path_attributes(const std::string& path,const Attributes& a,bool follow_final) {
    valid_attributes(a);
    update([&](const NamespaceView& view,Changes& changes) {
        auto id=resolve(view,path,follow_final);auto n=inode(view,id);
        need(!n.symlink || !a.size,"invalid symbolic link size change");
        if(!a.mode && !a.modified_ns && !a.size && !a.accessed_ns && !a.birth_ns && !a.flags)return;
        if(a.size && *a.size==n.size && !a.mode && !a.modified_ns && !a.accessed_ns && !a.birth_ns && !a.flags)return;
        if(a.size){need(!n.directory && !n.symlink,"only regular files support size changes");
            FileContent data(n,view,&changes);if(data.resize(*a.size))touch(n);}
        apply_attributes(n,a);changes[inode_key(id)]=encode(n);
    });
}
void Namespace::handle_set_attributes(const std::string& token,const Attributes& a) {
    valid_attributes(a);need(!revision_,"revision view is read-only");
    std::lock_guard lock(container_.mutex_);const auto& info=find_handle(token);
    NamespaceView view(*container_.journal_,view_,revision_);bool linked=view.contains(inode_key(info.inode));
    auto key=linked?inode_key(info.inode):orphan_prefix(view_,info.inode)+"inode";
    auto n=linked?inode(view,info.inode):decode_inode(container_.journal_->get(key),info.inode,true);
    need(!n.symlink || !a.size,"invalid symbolic link size change");
    need(!a.size || info.writable,"handle is not writable");
    if(!a.mode && !a.modified_ns && !a.size && !a.accessed_ns && !a.birth_ns && !a.flags)return;
    if(a.size && *a.size==n.size && !a.mode && !a.modified_ns && !a.accessed_ns && !a.birth_ns && !a.flags)return;
    Changes changes;
    auto pre=orphan_prefix(view_,info.inode);auto retained=linked?nullptr:retained_orphan(*container_.journal_,pre);
    if(a.size){FileContent data=retained?FileContent(n,*retained,&changes):linked?FileContent(n,view,&changes):FileContent(n,*container_.journal_,pre,&changes);if(data.resize(*a.size))touch(n);}
    if(retained){apply_attributes(n,a);commit_orphan_changes(*retained,pre,n,changes);return;}
    apply_attributes(n,a);changes[key]=encode(n);commit_changes(view,changes);
}
InodeInfo Namespace::lstat(const std::string& path) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);auto n=inode(view,resolve(view,path,false));allocation_info(n,view);remember_path(n.id,path);return n;
}
void Namespace::symlink(const std::string& target,const std::string& path) {
    target_components(target);
    update([&](const NamespaceView& view,Changes& c) {
        auto p=parent(view,path);need(!view.contains(p.key),"path already exists");auto id=new_id(view);
        InodeInfo n=created({id,false,0777,1,target.size(),now(),now(),true});auto pn=inode(view,p.id);touch(pn);
        c[inode_key(id)]=encode(n);c[content_key(id)]=Bytes(target.begin(),target.end());c[allocated_key(id)]=Bytes{};
        c[p.key]=id_bytes(id);c[inode_key(p.id)]=encode(pn);
    });
}
std::string Namespace::readlink(const std::string& path) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);
    auto id=resolve(view,path,false);auto n=inode(view,id);need(n.symlink,"path is not a symbolic link");auto b=view.get(content_key(id));
    need(b.size()==n.size,"symlink size mismatch");std::string result(b.begin(),b.end());target_components(result);return result;
}
void Namespace::link(const std::string& source,const std::string& target) {
    update([&](const NamespaceView& j,Changes& c) {
        auto id=resolve(j,source,false);auto n=inode(j,id);need(!n.directory,"directory hard links are forbidden");
        auto p=parent(j,target);need(!j.contains(p.key),"target already exists");need(n.links<UINT64_MAX,"link count exhausted");
        ++n.links;n.changed_ns=now();auto pn=inode(j,p.id);touch(pn);
        c[p.key]=id_bytes(id);c[inode_key(id)]=encode(n);c[inode_key(p.id)]=encode(pn);c[allocated_key(id)]=Bytes{};
    });
}
void Namespace::retire(const NamespaceView& j,Changes& c,const InodeInfo& n) {
    c[inode_key(n.id)]=std::nullopt;auto attrs=Xattrs(j,n.id).records();for(const auto& key:attrs)c[key]=std::nullopt;if(n.directory)return;
    auto chunks=prefix+"chunk/"+n.id+"/";
    if(n.chunked)c.erase_range(chunks,prefix_end(chunks));else c[content_key(n.id)]=std::nullopt;
    auto count=container_.open_counts_.find({view_,n.id});
    if(count!=container_.open_counts_.end() && count->second) {
        auto orphan=n;orphan.links=0;orphan.changed_ns=now();auto pre=orphan_prefix(view_,n.id);
        c[pre+"inode"]=encode(orphan);
        if(!j.legacy()) {
            auto manifest=container_.journal_->get(history+"world/"+view_);
            if(manifest.size()==40 && !memcmp(manifest.data(),"FFTREE01",8)) {
                // Pin the immutable origin tree in one root. All orphan edits use
                // its private CoW tree, never the live World's inode or path.
                c[pre+"tree"]=std::move(manifest);return;
            }
        }
        auto copy=n;auto records=FileContent(copy,j).records();
        for(const auto& key:attrs)c[pre+"xattr/"+key.substr(xattr_prefix(n.id).size())]=j.get(key);
        for(const auto& key:records){auto suffix=n.chunked?"chunk/"+key.substr(chunks.size()):"content";c[pre+suffix]=j.get(key);}
    }
}
void Namespace::remove(const std::string& path,bool directory) {
    update([&](const NamespaceView& j,Changes& c) {
        auto p=parent(j,path);need(j.contains(p.key),"path not found");auto id=decode_id(j.get(p.key));auto n=inode(j,id),pn=inode(j,p.id);
        need(!directory || n.directory,"path is not a directory");
        need(directory || !n.directory,"cannot unlink a directory");
        if(directory){need(j.keys(directory_prefix(id)).empty(),"directory is not empty");--pn.links;}
        c[p.key]=std::nullopt;
        if(directory || n.links==1)retire(j,c,n);
        else {--n.links;n.changed_ns=now();c[inode_key(id)]=encode(n);}
        touch(pn);c[inode_key(p.id)]=encode(pn);
    });
}
void Namespace::rename(const std::string& source,const std::string& target) {
    update([&](const NamespaceView& j,Changes& c) {
        auto src=parent(j,source);need(j.contains(src.key),"source not found");auto id=decode_id(j.get(src.key));auto n=inode(j,id);
        std::vector<std::string> ancestry;auto dst=parent(j,target,&ancestry);
        if(src.key==dst.key)return;
        if(n.directory)need(std::find(ancestry.begin(),ancestry.end(),id)==ancestry.end(),"cannot move directory into itself");
        std::map<std::string,InodeInfo> parents;parents.emplace(src.id,inode(j,src.id));parents.emplace(dst.id,inode(j,dst.id));
        if(j.contains(dst.key)) {
            auto replaced=decode_id(j.get(dst.key));if(replaced==id)return;
            auto other=inode(j,replaced);need(!other.directory || n.directory,"rename target is a directory");
            need(other.directory || !n.directory,"rename target is not a directory");
            if(other.directory){need(j.keys(directory_prefix(replaced)).empty(),"target directory is not empty");--parents.at(dst.id).links;}
            if(other.directory || other.links==1)retire(j,c,other);
            else {--other.links;other.changed_ns=now();c[inode_key(replaced)]=encode(other);}
        }
        if(n.directory && src.id!=dst.id){--parents.at(src.id).links;need(parents.at(dst.id).links<UINT64_MAX,"directory link count exhausted");++parents.at(dst.id).links;}
        c[src.key]=std::nullopt;c[dst.key]=id_bytes(id);n.changed_ns=now();c[inode_key(id)]=encode(n);
        for(auto& [parent_id,pn]:parents){touch(pn);c[inode_key(parent_id)]=encode(pn);}
    });
}
void Namespace::verify() const {
    std::lock_guard lock(container_.mutex_);verify_locked();
}
void Namespace::verify_locked() const {
    const auto& journal=*container_.journal_;NamespaceView j(journal,view_,revision_);
    if(journal.is_leveldb())(void)j.references(); // Validate the full CoW tree before namespace traversal.
    auto root_id=root(j);need(inode(j,root_id).directory,"root inode is not a directory");std::set<std::string> expected;std::map<std::string,uint64_t> references;
    std::set<std::string> directories;expected.insert(root_key);
    std::function<void(const std::string&)> visit=[&](const std::string& id) {
        auto n=inode(j,id);expected.insert(inode_key(id));
        Xattrs attributes(j,id);attributes.verify(n.xattrs);for(const auto& key:attributes.records())expected.insert(key);
        need(j.contains(allocated_key(id)) && j.get(allocated_key(id)).empty(),"inode missing allocation record");
        if(!n.directory){FileContent content(n,j);content.verify();
            if(n.symlink){auto data=j.get(content_key(id));target_components(std::string(data.begin(),data.end()));}
            for(const auto& key:content.records())expected.insert(key);return;}
        need(directories.insert(id).second,"directory cycle or hardlink");uint64_t subdirs=0;
        auto pre=directory_prefix(id);
        for(const auto& key:j.keys(pre)) {
            auto name=key.substr(pre.size());auto parsed=components("/"+name);need(parsed.size()==1,"invalid directory name");
            expected.insert(key);auto child=decode_id(j.get(key));++references[child];
            if(inode(j,child).directory)++subdirs;visit(child);
        }
        need(n.links==2+subdirs,"directory link count mismatch");
    };
    visit(root_id);
    for(const auto& [id,count]:references){auto n=inode(j,id);if(!n.directory)need(n.links==count,"file link count mismatch");}
    for(const auto& key:j.keys(prefix+"allocated/")) {
        auto id=key.substr((prefix+"allocated/").size());
        need(valid_id(id) && j.get(key).empty(),"invalid allocation record");expected.insert(key);
    }
    auto actual=j.keys(prefix);need(actual.size()==expected.size(),"orphan namespace records");
    for(const auto& key:actual)need(expected.contains(key),"unexpected namespace record");
    if(!revision_) {
        auto pre=history+"orphan/"+view_+"/";auto orphan_keys=journal.keys(pre);std::set<std::string> checked;
        for(const auto& key:orphan_keys) {
            auto relative=key.substr(pre.size());need(relative.size()>=33 && valid_id(relative.substr(0,32)) && relative[32]=='/',"invalid orphan root");
            auto id=relative.substr(0,32);need(valid_orphan_field(relative.substr(33)),"invalid orphan field");
            if(!checked.insert(id).second)continue;
            auto count=container_.open_counts_.find({view_,id});need(count!=container_.open_counts_.end() && count->second>0,"orphan lacks an open handle");
            need(!j.contains(inode_key(id)),"orphan also appears in World tree");
            auto n=decode_inode(journal.get(orphan_prefix(view_,id)+"inode"),id,true);
            if(auto retained=retained_orphan(journal,orphan_prefix(view_,id))) {
                (void)retained->references();FileContent content(n,*retained);content.verify();Xattrs(*retained,id).verify(n.xattrs);
                if(n.symlink){auto data=retained->get(content_key(id));target_components(std::string(data.begin(),data.end()));}
                auto fields=journal.keys(orphan_prefix(view_,id));need(fields.size()==2,"unexpected orphan tree record");continue;
            }
            FileContent content(n,journal,orphan_prefix(view_,id));content.verify();
            if(n.symlink){auto data=journal.get(orphan_prefix(view_,id)+"content");target_components(std::string(data.begin(),data.end()));}
            Xattrs attributes(journal,orphan_prefix(view_,id));attributes.verify(n.xattrs);
            std::set<std::string> fields;for(const auto& record:content.records())fields.insert(record);for(const auto& record:attributes.records())fields.insert(record);fields.insert(orphan_prefix(view_,id)+"inode");
            for(const auto& record:journal.keys(orphan_prefix(view_,id)))need(fields.contains(record),"unexpected orphan record");
        }
    }
}
const Container::OpenHandle& Namespace::find_handle(const std::string& token) const {
    auto it=container_.handles_.find(token);need(it!=container_.handles_.end(),"invalid or expired file handle");
    need(it->second.view==view_ && it->second.revision==revision_,"file handle belongs to another view");return it->second;
}
std::string Namespace::open(const std::string& path,Access access) {return open(path,access,OpenOptions{});}
std::string Namespace::open(const std::string& path,Access access,const OpenOptions& options) {
    need(access==Access::Read || access==Access::Write || access==Access::ReadWrite,"invalid handle access mode");
    need(!options.exclusive || options.create,"exclusive open requires create");
    need(!options.truncate || access!=Access::Read,"truncate open requires writable access");
    need(!options.append || access!=Access::Read,"append open requires writable access");
    need(!options.symlink || (!options.create && !options.truncate && !options.append && !options.nofollow),"invalid symlink open options");
    need(options.mode<=0777,"invalid creation mode");
    need(!revision_ || (access==Access::Read && !options.create && !options.truncate),"revision view is read-only");
    std::lock_guard lock(container_.mutex_);auto& j=*container_.journal_;
    need(j.is_leveldb(),"file handles require the LevelDB backend");
    need(container_.handles_.size()<4096,"file handle limit reached");NamespaceView view(j,view_,revision_);
    Changes changes;std::string id;
    if(options.create) {
        auto p=parent(view,path);bool exists=view.contains(p.key);
        need(!exists || !options.exclusive,"path already exists");
        if(exists) {
            auto raw=inode(view,decode_id(view.get(p.key)));
            need(!options.nofollow || !raw.symlink,"final symbolic link is forbidden");
            Parent destination;id=resolve(view,path,true,&destination);
            if(id.empty()){p=std::move(destination);exists=false;}
        }
        if(!exists) {
            id=new_id(view);InodeInfo n=created({id,false,options.mode,1,0,now(),now()});auto pn=inode(view,p.id);touch(pn);
            changes[inode_key(id)]=encode(n);changes[content_key(id)]=Bytes{};changes[allocated_key(id)]=Bytes{};
            changes[p.key]=id_bytes(id);changes[inode_key(p.id)]=encode(pn);
        }
    } else {id=resolve(view,path,!(options.nofollow || options.symlink));need(!options.nofollow || !inode(view,id).symlink,"final symbolic link is forbidden");}
    if(changes.empty()) {
        auto n=inode(view,id);need(!n.directory,"cannot read a directory");need(!n.symlink || options.symlink,"symbolic link open is forbidden");
        if(options.truncate && n.size!=0){FileContent(n,view,&changes).resize(0);touch(n);changes[inode_key(id)]=encode(n);}
    }
    auto token=new_id(view);while(token==id || container_.handles_.contains(token))token=new_id(view);
    // Reserve runtime state before publication. Any failure rolls it back while
    // holding the same lock that protects create/truncate and inode pinning.
    container_.handles_.emplace(token,Container::OpenHandle{view_,id,revision_,access!=Access::Write,access!=Access::Read,options.append,std::make_shared<Container::Cursor>()});
    bool counted=false;
    try {
        if(!revision_){++container_.open_counts_[{view_,id}];counted=true;}
        commit_changes(view,changes);
    } catch(...) {
        if(counted){auto count=container_.open_counts_.find({view_,id});if(--count->second==0)container_.open_counts_.erase(count);}
        container_.handles_.erase(token);throw;
    }
    remember_path(id,path);container_.compaction_cv_.notify_all();return token;
}

std::string Namespace::dup(const std::string& token) {
    std::lock_guard lock(container_.mutex_);auto info=find_handle(token);
    need(container_.handles_.size()<4096,"file handle limit reached");NamespaceView view(*container_.journal_,view_,revision_);
    auto result=new_id(view);while(container_.handles_.contains(result))result=new_id(view);
    container_.handles_.emplace(result,info);
    try{if(!revision_)++container_.open_counts_[{view_,info.inode}];}catch(...){container_.handles_.erase(result);throw;}
    return result;
}
bool Namespace::try_lock(const std::string& token,Lock mode) {return try_lock_range(token,mode,0,0);}
bool Namespace::try_lock_range(const std::string& token,Lock mode,uint64_t start,uint64_t length) {
    need(mode==Lock::Shared || mode==Lock::Exclusive || mode==Lock::Unlock,"invalid lock mode");
    need(start<=INT64_MAX && (length==0 || length<=uint64_t(INT64_MAX)+1-start),"lock range exceeds signed offset space");
    auto end=length?start+length:uint64_t(INT64_MAX)+1;
    std::lock_guard lock(container_.mutex_);const auto& info=find_handle(token);
    unsigned char requested=mode==Lock::Shared?1:2;
    if(mode!=Lock::Unlock)for(const auto& [other_token,other]:container_.handles_) {
        if(other.cursor==info.cursor || other.view!=info.view || other.revision!=info.revision || other.inode!=info.inode)continue;
        for(const auto& range:other.cursor->locks)
            if(start<range.end && range.start<end && (requested==2 || range.mode==2))return false;
    }
    std::vector<Container::LockRange> next;
    for(const auto& range:info.cursor->locks) {
        if(start>=range.end || range.start>=end){next.push_back(range);continue;}
        if(range.start<start)next.push_back({range.start,start,range.mode});
        if(end<range.end)next.push_back({end,range.end,range.mode});
    }
    if(mode!=Lock::Unlock)next.push_back({start,end,requested});
    std::sort(next.begin(),next.end(),[](const auto& a,const auto& b){return a.start<b.start;});
    std::vector<Container::LockRange> normalized;
    for(const auto& range:next) {
        if(!normalized.empty() && normalized.back().end==range.start && normalized.back().mode==range.mode)normalized.back().end=range.end;
        else normalized.push_back(range);
    }
    need(normalized.size()<=128,"open description lock range limit reached");
    info.cursor->locks=std::move(normalized);return true;
}
uint64_t Namespace::seek(const std::string& token,int64_t offset,Seek origin) {
    std::lock_guard lock(container_.mutex_);const auto& info=find_handle(token);uint64_t base=0;
    if(origin==Seek::Current)base=info.cursor->position;
    else if(origin==Seek::End){NamespaceView view(*container_.journal_,view_,revision_);
        base=view.contains(inode_key(info.inode))?inode(view,info.inode).size:
            decode_inode(container_.journal_->get(orphan_prefix(view_,info.inode)+"inode"),info.inode,true).size;
    } else need(origin==Seek::Set,"invalid seek origin");
    need(base<=INT64_MAX,"file position overflow");uint64_t result;
    if(offset<0){auto magnitude=uint64_t(-(offset+1))+1;need(magnitude<=base,"negative file position");result=base-magnitude;}
    else {need(base<=INT64_MAX && uint64_t(offset)<=uint64_t(INT64_MAX)-base,"file position overflow");result=base+uint64_t(offset);}
    info.cursor->position=result;return result;
}
Bytes Namespace::handle_read_next(const std::string& token,uint64_t count) {
    std::lock_guard lock(container_.mutex_);const auto& info=find_handle(token);need(info.readable,"handle is not readable");
    auto data=read_handle_locked(info,info.cursor->position,count);info.cursor->position+=data.size();return data;
}
size_t Namespace::handle_write_next(const std::string& token,const Bytes& value) {
    uint64_t next=0;std::shared_ptr<Container::Cursor> cursor;
    edit_handle(token,[&](FileContent& data,bool append) {
        const auto& info=find_handle(token);cursor=info.cursor;next=cursor->position;
        auto actual=append?data.size():next;if(!data.write(actual,value))return false;next=actual+value.size();return true;
    },[&]{cursor->position=next;});return value.size();
}

void Namespace::close(const std::string& token) {
    std::lock_guard lock(container_.mutex_);auto info=find_handle(token);
    if(!info.revision) {
        auto count=container_.open_counts_.find({info.view,info.inode});need(count!=container_.open_counts_.end() && count->second>0,"invalid open count");
        if(count->second==1) {
            auto& j=*container_.journal_;auto pre=orphan_prefix(info.view,info.inode);
            if(j.contains(pre+"inode")){std::vector<Mutation> batch;for(const auto& key:j.keys(pre))batch.push_back({key,std::nullopt});j.transact(j.sequence(),batch);}
            container_.open_counts_.erase(count);
        } else --count->second;
    }
    container_.handles_.erase(token);
}
InodeInfo Namespace::handle_stat(const std::string& token) const {
    std::lock_guard lock(container_.mutex_);const auto& info=find_handle(token);const auto& j=*container_.journal_;
    NamespaceView view(j,view_,revision_);auto key=inode_key(info.inode);
    if(view.contains(key)){auto n=inode(view,info.inode);allocation_info(n,view);return n;}
    need(!revision_,"revision inode missing");auto pre=orphan_prefix(view_,info.inode);auto n=decode_inode(j.get(pre+"inode"),info.inode,true);
    if(auto retained=retained_orphan(j,pre))allocation_info(n,*retained);
    else n.allocated_size=FileContent(n,j,pre).allocated_bytes(*container_.allocation_cache_);
    return n;
}
Bytes Namespace::handle_read(const std::string& token,uint64_t offset,uint64_t count) const {
    need(offset<=INT64_MAX,"file offset exceeds signed 64-bit range");
    std::lock_guard lock(container_.mutex_);const auto& info=find_handle(token);need(info.readable,"handle is not readable");
    return read_handle_locked(info,offset,count);
}
Bytes Namespace::read_handle_locked(const Container::OpenHandle& info,uint64_t offset,uint64_t count) const {
    const auto& j=*container_.journal_;NamespaceView view(j,view_,revision_);
    if(view.contains(inode_key(info.inode))){auto n=inode(view,info.inode);need(!n.symlink,"symbolic link handle forbids data I/O");return FileContent(n,view).read(offset,count);}
    need(!revision_,"revision inode missing");auto pre=orphan_prefix(view_,info.inode);
    auto n=decode_inode(j.get(pre+"inode"),info.inode,true);need(!n.symlink,"symbolic link handle forbids data I/O");
    if(auto retained=retained_orphan(j,pre))return FileContent(n,*retained).read(offset,count);
    return FileContent(n,j,pre).read(offset,count);
}
void Namespace::edit_handle(const std::string& token,const std::function<bool(FileContent&,bool)>& edit,const std::function<void()>& committed) {
    need(!revision_,"revision view is read-only");
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);Changes changes;
    const auto& info=find_handle(token);need(info.writable,"handle is not writable");
    bool linked=view.contains(inode_key(info.inode));auto pre=orphan_prefix(view_,info.inode);auto& j=*container_.journal_;
    auto n=linked?inode(view,info.inode):decode_inode(j.get(pre+"inode"),info.inode,true);
    need(!n.symlink,"symbolic link handle forbids data I/O");
    auto retained=linked?nullptr:retained_orphan(j,pre);
    FileContent data=retained?FileContent(n,*retained,&changes):linked?FileContent(n,view,&changes):FileContent(n,j,pre,&changes);
    if(retained) {
        if(edit(data,info.append)){touch(n);commit_orphan_changes(*retained,pre,n,changes);}
        if(committed)committed();return;
    }
    if(edit(data,info.append)){touch(n);changes[linked?inode_key(info.inode):pre+"inode"]=encode(n);}
    commit_changes(view,changes);if(committed)committed();
}
void Namespace::handle_write(const std::string& token,uint64_t offset,const Bytes& value) {
    need(offset<=INT64_MAX,"file offset exceeds signed 64-bit range");
    edit_handle(token,[&](FileContent& data,bool append){return data.write(append?data.size():offset,value);});
}
uint64_t Namespace::handle_append(const std::string& token,const Bytes& value) {
    uint64_t offset=0;edit_handle(token,[&](FileContent& data,bool){offset=data.size();return data.write(offset,value);});return offset;
}
void Namespace::handle_truncate(const std::string& token,uint64_t size) {
    need(size<=maximum_file_size,"file exceeds 1 TiB");
    edit_handle(token,[&](FileContent& data,bool){return data.resize(size);});
}
void Namespace::recover_orphans(Container& container) {
    // Previous-process handles have expired. Release retention roots, not objects.
    auto& j=*container.journal_;auto pre=history+"orphan/";auto keys=j.keys(pre);
    std::vector<Mutation> batch;
    for(const auto& key:keys) {
        auto relative=key.substr(pre.size());auto slash=relative.find('/');need(slash!=std::string::npos,"invalid orphan root");
        auto world=relative.substr(0,slash);valid_view_name(world);relative=relative.substr(slash+1);
        need(relative.size()>=33 && valid_id(relative.substr(0,32)) && relative[32]=='/' &&
            (valid_orphan_field(relative.substr(33))),"invalid orphan root");
        batch.push_back({key,std::nullopt});
        if(batch.size()==4096){j.transact(j.sequence(),batch);batch.clear();}
    }
    if(!batch.empty())j.transact(j.sequence(),batch);
}

}

namespace forkfs {
void Namespace::remember_path(const std::string& id,const std::string& path) const noexcept {
    // Caller holds the repository mutex. An optional hint must never turn an
    // already committed operation into a failure if allocation is unavailable.
    try {
        Container::InodePathKey key{view_,revision_,id};auto& paths=container_.inode_paths_;
        auto existing=paths.find(key);if(existing!=paths.end()){existing->second=path;return;}
        auto inserted=paths.emplace(key,path).first;
        try{container_.inode_path_order_.push_back(std::move(key));}catch(...){paths.erase(inserted);throw;}
        if(paths.size()>Container::maximum_inode_path_hints){paths.erase(container_.inode_path_order_.front());container_.inode_path_order_.pop_front();}
    }catch(...){} // Optimization only; resolution remains authoritative.
}
std::string Namespace::path_for_inode(const std::string& id) const {
    need(valid_id(id),"invalid inode identity");std::lock_guard lock(container_.mutex_);
    NamespaceView view(*container_.journal_,view_,revision_);auto rid=root(view);
    auto hint=container_.inode_paths_.find({view_,revision_,id});
    if(hint!=container_.inode_paths_.end()){
        try{if(resolve(view,hint->second,false)==id)return hint->second;}
        catch(const std::runtime_error& e){auto message=std::string_view(e.what());
            if(message!="path not found" && message!="path component is not a directory" &&
               message!="too many symbolic links" && message!="symlink expansion limit")throw;
        }
        // Rename, link removal, ancestor symlink retargeting and path reuse can
        // stale a hint. Fall back without changing the inode identity requested.
    }
    struct PendingPath {std::string inode,path;bool too_long;};
    // Keep overlong branches in the walk without retaining unbounded path strings.
    std::vector<PendingPath> pending{{rid,"/",false}};std::set<std::string> visited;bool found_too_long=false;
    while(!pending.empty()) {
        auto entry=std::move(pending.back());pending.pop_back();auto& current=entry.inode;
        // A file may have a short hardlink alias elsewhere, so defer the error.
        if(current==id){if(!entry.too_long){remember_path(id,entry.path);return entry.path;}found_too_long=true;}
        if(!inode(view,current).directory)continue;
        need(visited.insert(current).second,"invalid directory cycle or hardlink");
        auto pre=directory_prefix(current);
        for(const auto& key:view.keys(pre)) {
            auto name=key.substr(pre.size());auto child=decode_id(view.get(key));
            auto separator=entry.path=="/"?size_t(0):size_t(1);
            bool too_long=entry.too_long || entry.path.size()+separator>4096 || name.size()>4096-entry.path.size()-separator;
            if(too_long)pending.push_back({std::move(child),{},true});
            else {auto next=entry.path=="/"?entry.path+name:entry.path+"/"+name;pending.push_back({std::move(child),std::move(next),false});}
        }
    }
    if(found_too_long)throw std::runtime_error("path exceeds mount protocol limit");
    throw std::runtime_error("inode is no longer linked");
}
}
