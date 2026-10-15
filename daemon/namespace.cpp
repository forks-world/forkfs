#include "revision.h"
#include "namespace.h"
#include "metadata_view.h"
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
    return RevisionRecord::decode(b).manifest;
}
}
namespace {
using Bytes=std::vector<unsigned char>;
void valid_attributes(const Namespace::Attributes& a);
void apply_attributes(InodeInfo& n,const Namespace::Attributes& a);
const std::string prefix("\0forkfs/",8);
const std::string root_key=prefix+"root";
void need(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
uint64_t load(const unsigned char* p,size_t n) {
    uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(p[i])<<(8*i);return v;
}
void store(unsigned char* p,uint64_t v,size_t n) {
    for(size_t i=0;i<n;++i)p[i]=static_cast<unsigned char>(v>>(8*i));
}
uint64_t world_generation(const Journal& journal,const std::string& world) {
    auto key=history+"generation/"+world;
    if(!journal.contains(key))return 0;
    auto value=journal.get(key);need(value.size()==8,"invalid World generation");
    return load(value.data(),8);
}
uint64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string inode_key(const std::string& id) {return prefix+"inode/"+id;}
const std::string parent_index_key=prefix+"parent-index";
std::string parent_key(const std::string& id) {return prefix+"parent/"+id;}
std::string allocated_key(const std::string& id) {return prefix+"allocated/"+id;}
std::string content_key(const std::string& id) {return prefix+"content/"+id;}
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
    bool xattrs=b.size()>=84 && b.size()<=128*1024 && !memcmp(b.data(),"FFINOD04",8);
    bool extended=(b.size()==80 && !memcmp(b.data(),"FFINOD03",8)) || xattrs;
    need(extended || (b.size()==48 && (!memcmp(b.data(),"FFINOD01",8) || !memcmp(b.data(),"FFINOD02",8))),"invalid inode record");
    auto type=load(b.data()+8,4),mode=load(b.data()+12,4);
    need((type==1||type==2||type==3) && mode<=0777,"unsupported inode attributes");
    InodeInfo n{id,type==2,static_cast<uint32_t>(mode),load(b.data()+16,8),load(b.data()+24,8),
                load(b.data()+32,8),load(b.data()+40,8),type==3,!memcmp(b.data(),"FFINOD02",8)};
    if(extended) {
        auto flags=load(b.data()+48,4);need(flags<=1 && load(b.data()+60,4)==0,"unsupported inode flags");
        n.indexed_directory=flags&1;n.uid=load(b.data()+52,4);n.gid=load(b.data()+56,4);
        n.accessed_ns=load(b.data()+64,8);n.created_ns=load(b.data()+72,8);n.extended_attributes=true;
        need(n.accessed_ns<=INT64_MAX && n.created_ns<=INT64_MAX,"invalid extended inode timestamp");
    }
    if(xattrs) {
        auto values=std::make_shared<InodeInfo::Xattrs>();auto count=load(b.data()+80,4);need(count<=128,"too many xattrs");size_t at=84;std::string previous;
        for(uint64_t i=0;i<count;++i) {
            need(b.size()-at>=8,"truncated xattr lengths");auto name_size=load(b.data()+at,4),value_size=load(b.data()+at+4,4);at+=8;
            need(name_size>0 && name_size<=255 && value_size<=64*1024 && name_size+value_size<=b.size()-at,"invalid xattr size");
            std::string name(b.begin()+at,b.begin()+at+name_size);at+=name_size;
            need(std::all_of(name.begin(),name.end(),[](unsigned char c){return c>=33 && c<=126;}) && (i==0 || previous<name),"invalid xattr name/order");
            previous=name;values->emplace(name,Bytes(b.begin()+at,b.begin()+at+value_size));at+=value_size;
        }
        need(at==b.size(),"unexpected xattr suffix");n.xattrs=std::move(values);
    }
    need(!n.indexed_directory || n.directory,"indexed format is only valid for directories");
    need((orphan?(!n.directory && !n.symlink && n.links==0):n.links>0) && (!n.directory || (n.links>=2 && n.size==0)),"invalid inode state");return n;
}
InodeInfo inode(const NamespaceView& journal,const std::string& id) {return decode_inode(journal.get(inode_key(id)),id);}
InodeInfo frontend_inode(const NamespaceView& view,const std::string& id) {
    if(!valid_id(id))throw std::system_error(EINVAL,std::generic_category(),"invalid inode identity");
    if(!view.contains(inode_key(id)))throw std::system_error(ENOENT,std::generic_category(),"inode missing");
    return inode(view,id);
}
std::string orphan_prefix(const std::string& world,const std::string& id) {return history+"orphan/"+world+"/"+id+"/";}
Bytes encode(const InodeInfo& n) {
    Bytes b(n.xattrs?84:n.extended_attributes?80:48);memcpy(b.data(),n.xattrs?"FFINOD04":n.extended_attributes?"FFINOD03":n.indexed_directory?"FFINOD02":"FFINOD01",8);store(b.data()+8,n.symlink?3:n.directory?2:1,4);
    store(b.data()+12,n.mode,4);store(b.data()+16,n.links,8);store(b.data()+24,n.size,8);
    store(b.data()+32,n.modified_ns,8);store(b.data()+40,n.changed_ns,8);
    if(n.extended_attributes || n.xattrs){store(b.data()+48,n.indexed_directory?1:0,4);store(b.data()+52,n.uid,4);store(b.data()+56,n.gid,4);store(b.data()+64,n.accessed_ns,8);store(b.data()+72,n.created_ns,8);}
    if(n.xattrs) {
        if(n.xattrs->size()>128)throw std::system_error(E2BIG,std::generic_category(),"too many xattrs");store(b.data()+80,n.xattrs->size(),4);
        for(const auto& [name,value]:*n.xattrs){size_t at=b.size();
            if(8+name.size()+value.size()>128*1024-at)throw std::system_error(E2BIG,std::generic_category(),"xattr inode exceeds 128 KiB");
            b.resize(at+8+name.size()+value.size());store(b.data()+at,name.size(),4);store(b.data()+at+4,value.size(),4);
            memcpy(b.data()+at+8,name.data(),name.size());if(!value.empty())memcpy(b.data()+at+8+name.size(),value.data(),value.size());
        }
    }
    return b;
}
bool indexed_parents(const NamespaceView& view) {
    if(!view.contains(parent_index_key))return false;
    auto value=view.get(parent_index_key);need(value==Bytes({'F','F','P','I','D','X','0','1'}),"unsupported directory parent index");
    need(inode(view,root(view)).indexed_directory,"parent index requires versioned root inode");return true;
}
Bytes parent_record(const std::string& parent,const std::string& name) {
    auto result=id_bytes(parent);result.insert(result.begin(),{'F','F','P','A','R','0','0','1'});result.insert(result.end(),name.begin(),name.end());return result;
}
std::string indexed_parent(const NamespaceView& view,const std::string& id) {
    need(inode(view,id).indexed_directory,"directory parent index requires versioned inode");
    auto value=view.get(parent_key(id));need(value.size()>=40 && value.size()<=295 && !memcmp(value.data(),"FFPAR001",8),"invalid directory parent record");
    auto parent=decode_id(Bytes(value.begin()+8,value.begin()+40));std::string name(value.begin()+40,value.end());
    if(id==root(view)){need(parent==id && name.empty(),"invalid root parent");return parent;}
    need(!name.empty() && name!="." && name!=".." && std::all_of(name.begin(),name.end(),[](unsigned char c){return c>=32 && c<=126 && c!='/';}),"invalid parent entry name");
    need(view.contains(inode_key(parent)) && inode(view,parent).directory,"directory parent is missing");
    auto key=entry_key(parent,name);need(view.contains(key) && decode_id(view.get(key))==id,"directory parent backlink mismatch");return parent;
}
std::map<std::string,Bytes> directory_parent_records(const NamespaceView& view) {
    auto root_id=root(view);std::map<std::string,Bytes> result{{root_id,parent_record(root_id,"")}};std::vector<std::string> pending{root_id};
    while(!pending.empty()) {
        auto id=std::move(pending.back());pending.pop_back();need(inode(view,id).directory,"root is not a directory");
        auto pre=directory_prefix(id);std::string after;
        for(;;){auto entries=view.page(pre,after,128);if(entries.empty())break;
            for(const auto& key:entries){auto child=decode_id(view.get(key));if(inode(view,child).directory){
                need(result.emplace(child,parent_record(id,key.substr(pre.size()))).second,"directory cycle or hardlink");pending.push_back(child);
            }}after=entries.back();if(entries.size()<128)break;
        }
    }return result;
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
std::vector<std::string> components(const std::string& path) {
    need(!path.empty() && path[0]=='/' && path.size()<=4096,"path must be absolute and at most 4096 bytes");
    std::vector<std::string> result;
    if(path=="/")return result;
    size_t begin=1;
    while(begin<path.size()) {
        auto end=path.find('/',begin);if(end==std::string::npos)end=path.size();
        auto name=path.substr(begin,end-begin);
        need(!name.empty() && name!="." && name!=".." && name.size()<=255,"invalid path component");
        for(unsigned char c:name)need(c>=32 && c<=126,"prototype names must be printable ASCII");
        result.push_back(name);begin=end+1;
        need(end==path.size() || begin<path.size(),"trailing slash is unsupported");
    }
    return result;
}
std::vector<std::string> target_components(const std::string& target) {
    need(!target.empty() && target.size()<=4096,"invalid symlink target length");
    for(unsigned char c:target)need(c>=32 && c<=126,"prototype symlink targets must be printable ASCII");
    std::vector<std::string> result;size_t start=0;
    while(start<target.size()) {
        auto end=target.find('/',start);if(end==std::string::npos)end=target.size();auto name=target.substr(start,end-start);
        if(!name.empty()){need(name.size()<=255,"invalid symlink component");result.push_back(name);}start=end+1;
    }
    // Preserve a target's trailing slash requirement during traversal.
    if(target.back()=='/' && !result.empty())result.push_back(".");return result;
}
struct Parent {std::string id,name,key;};
Parent inode_parent(const NamespaceView& view,const std::string& id,const std::string& name) {
    if(!valid_id(id) || name.empty() || name=="." || name==".." ||
       name.find('/')!=std::string::npos || !std::all_of(name.begin(),name.end(),[](unsigned char c){return c>=32 && c<=126;}))
        throw std::system_error(EINVAL,std::generic_category(),"invalid inode entry name");
    if(name.size()>255)throw std::system_error(ENAMETOOLONG,std::generic_category(),"entry name too long");
    if(!view.contains(inode_key(id)))throw std::system_error(ENOENT,std::generic_category(),"parent inode missing");
    if(!inode(view,id).directory)throw std::system_error(ENOTDIR,std::generic_category(),"parent inode is not a directory");
    return {id,name,entry_key(id,name)};
}
// Indexed views walk destination ancestors; old views retain a paged subtree fallback.
bool directory_contains(const NamespaceView& view,const std::string& start,const std::string& target) {
    if(indexed_parents(view)) {
        auto current=target;std::set<std::string> visited;auto root_id=root(view);
        for(;;){if(current==start)return true;need(visited.insert(current).second,"directory parent cycle");
            auto next=indexed_parent(view,current);if(current==root_id)return false;current=std::move(next);
        }
    }
    std::vector<std::string> pending{start};std::set<std::string> seen;
    while(!pending.empty()) {
        auto id=std::move(pending.back());pending.pop_back();if(id==target)return true;
        need(seen.insert(id).second,"directory cycle in stored namespace");
        auto pre=directory_prefix(id);std::string after;
        for(;;){auto entries=view.page(pre,after,128);if(entries.empty())break;
            for(const auto& key:entries){auto child=decode_id(view.get(key));if(inode(view,child).directory)pending.push_back(child);}
            after=entries.back();if(entries.size()<128)break;
        }
    }
    return false;
}
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
void touch(InodeInfo& n) {n.modified_ns=n.changed_ns=now();}
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

std::vector<Mutation> Namespace::prepare_changes(const NamespaceView& view,const Changes& changes) {
    auto& journal=*container_.journal_;
    std::vector<Mutation> batch;
    // Initialization alone retains compatibility with the original flat format.
    if(view.legacy() && !view.contains(root_key)) {
        for(const auto& [key,value]:changes)batch.push_back({key,value});
    } else {
        NamespaceView::Changes delta;std::map<std::string,Bytes> new_objects;
        const auto allocated=prefix+"allocated/";
        for(const auto& [key,value]:changes) {
            if(key.starts_with(history+"orphan/")){batch.push_back({key,value});continue;}
            if(value) {
                auto id=Journal::object_id(*value);delta[key]=id;
                auto object_key=history+"object/"+hex_id(id);
                if(!journal.contains(object_key))new_objects.emplace(object_key,*value);
                if(key.starts_with(allocated))new_objects.emplace(history+"allocated/"+key.substr(allocated.size()),Bytes{});
            } else delta[key]=std::nullopt;
        }
        if(!delta.empty()) {
            auto plan=view.plan(delta);
            for(auto& payload:plan.objects) {
                auto id=Journal::object_id(payload);auto key=history+"object/"+hex_id(id);
                if(!journal.contains(key))new_objects.emplace(key,std::move(payload));
            }
            for(auto& [key,value]:new_objects)batch.push_back({key,std::move(value)});
            batch.push_back({history+"world/"+view_,std::move(plan.manifest)});
        }
    }
    return batch;
}
void Namespace::commit_changes(const NamespaceView& view,Changes& changes) {
    if(changes.empty())return;auto prepare_start=std::chrono::steady_clock::now();
    auto batch=prepare_changes(view,changes);
    container_.metadata_prepare_ns_+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-prepare_start).count();
    auto& journal=*container_.journal_;journal.transact(journal.sequence(),batch);
}

std::string Namespace::revision_point(const std::string& name) const {
    valid_view_name(name);
    // LevelStore captures roots, descriptor and log bounds in one snapshot.
    // Do not hold the namespace mutation lock throughout a cold log scan.
    return container_.journal_->revision_point(name);
}
std::string Namespace::snapshot(const std::string& name,std::optional<std::vector<std::string>> parents) {
    valid_view_name(name);need(!revision_,"revision view is read-only");
    std::lock_guard lock(container_.mutex_);auto& j=*container_.journal_;
    need(!j.contains(history+"revision/"+name),"revision name already exists");
    NamespaceView view(j,view_,false);root(view);
    ObjectId manifest_id{};std::vector<Mutation> batch;
    if(view.legacy()) {
        auto plan=view.plan({});
        for(auto& payload:plan.objects) {
            auto id=Journal::object_id(payload);auto key=history+"object/"+hex_id(id);
            if(!j.contains(key))batch.push_back({key,std::move(payload)});
        }
        manifest_id=Journal::object_id(plan.manifest);auto key=history+"object/"+hex_id(manifest_id);
        if(!j.contains(key))batch.push_back({key,std::move(plan.manifest)});
    } else manifest_id=j.root_object(history+"world/"+view_);
    auto base=history+"base/"+view_;RevisionRecord record;record.manifest=manifest_id;record.source_sequence=j.sequence();
    if(parents){need(parents->size()<=RevisionRecord::max_parents,"too many revision parents");
        for(const auto& parent:*parents){valid_view_name(parent);record.parents.push_back(j.root_object(history+"revision/"+parent));}
    }else if(j.contains(base)){auto bytes=j.get(base);need(bytes.size()==32,"invalid revision ancestry");ObjectId id{};std::copy(bytes.begin(),bytes.end(),id.begin());record.parents.push_back(id);}
    record.validate();
    if(!container_.revision_dag_){
        auto read=[journal=&j](const ObjectId& id){return journal->get_object(id);};
        container_.revision_dag_=std::make_unique<RevisionDag>(
            [read](const ObjectId& id){return RevisionRecord::read(read,id);},
            [read](const ObjectId& id){NamespaceView::verify_manifest(read,id);});
    }
    for(const auto& parent:record.parents)container_.revision_dag_->validate(parent);
    auto descriptor=record.encode();auto revision_id=Journal::object_id(descriptor);
    batch.push_back({history+"revision/"+name,std::move(descriptor)});
    batch.push_back({base,Bytes(revision_id.begin(),revision_id.end())});
    j.transact(j.sequence(),batch);return hex_id(revision_id);
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
void Namespace::restore(const std::string& revision) {
    valid_view_name(revision);valid_view_name(view_);need(!revision_,"revision view is read-only");
    std::lock_guard lock(container_.mutex_);auto& j=*container_.journal_;
    need(j.is_leveldb(),"restore requires LevelDB");
    NamespaceView current(j,view_,false);root(current);
    auto manifest=revision_manifest(j,revision);NamespaceView target(j,revision,true);root(target);
    auto generation_key=history+"generation/"+view_;
    auto generation=world_generation(j,view_);
    need(generation!=UINT64_MAX,"World generation exhausted");
    Bytes next(8);store(next.data(),generation+1,8);
    auto revision_id=j.root_object(history+"revision/"+revision);
    std::vector<Mutation> batch{{history+"world/"+view_,std::nullopt,manifest},
                              {history+"base/"+view_,Bytes(revision_id.begin(),revision_id.end())},
                              {generation_key,std::move(next)}};
    auto orphan_keys=j.keys(history+"orphan/"+view_+"/");
    need(orphan_keys.size()<=4093,"restore orphan batch limit exceeded");
    for(const auto& key:orphan_keys)batch.push_back({key,std::nullopt});
    // The durable publication precedes allocation-free runtime revocation, all
    // under the lock used by handle IO. A failed transaction preserves access.
    j.transact(j.sequence(),batch);
    std::erase_if(container_.handles_,[&](const auto& item){return !item.second.revision && item.second.view==view_;});
    std::erase_if(container_.open_counts_,[&](const auto& item){return item.first.first==view_;});
    container_.compaction_cv_.notify_all();
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
InodeInfo Namespace::root_inode() const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);
    return inode(view,root(view));
}
bool Namespace::has_parent_index() const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);return indexed_parents(view);
}
void Namespace::build_parent_index() {
    if(revision_)throw std::system_error(EROFS,std::generic_category(),"revision view is read-only");
    update([&](const NamespaceView& view,Changes& changes){
        if(indexed_parents(view))return;
        need(view.page(prefix+"parent/","",1).empty(),"unversioned parent records prevent index upgrade");
        auto records=directory_parent_records(view);
        need(records.size()<=1024,"parent index upgrade is limited to 1024 directories per atomic transaction");
        for(const auto& [id,value]:records){changes[parent_key(id)]=value;auto n=inode(view,id);n.indexed_directory=true;changes[inode_key(id)]=encode(n);}
        changes[parent_index_key]=Bytes{'F','F','P','I','D','X','0','1'};
        // A full run manifest must first pass update's compaction/wait/retry path.
        // Leave the changes pending so update knows this is a real mutation.
        if(view.run_count()>=NamespaceView::hard_run_limit)return;
        // Count actual objects, metadata plan and manifest once the view can plan
        // a publication. Directory count alone is not a transaction budget.
        const bool leveldb=container_.journal_->is_leveldb();
        const auto batch=prepare_changes(view,changes);
        if(batch.size()>(leveldb?4096u:64u))
            throw std::system_error(E2BIG,std::generic_category(),leveldb?
                "parent index upgrade exceeds LevelDB 4096-mutation limit":
                "parent index upgrade exceeds object-journal 64-mutation limit");
        std::set<ObjectId> values;size_t total=0;
        for(const auto& mutation:batch)if(mutation.value) {
            if(mutation.value->size()>256*1024)
                throw std::system_error(E2BIG,std::generic_category(),"parent index upgrade exceeds 256 KiB object limit");
            if(values.insert(Journal::object_id(*mutation.value)).second)total+=mutation.value->size();
        }
        if(total>(leveldb?16u*1024*1024:1024*1024))
            throw std::system_error(E2BIG,std::generic_category(),leveldb?
                "parent index upgrade exceeds LevelDB 16 MiB value limit":
                "parent index upgrade exceeds object-journal 1 MiB value limit");
    });
}
InodeInfo Namespace::parent_inode(const std::string& id) const {
    if(!valid_id(id))throw std::system_error(EINVAL,std::generic_category(),"invalid directory inode");
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);
    if(!view.contains(inode_key(id)))throw std::system_error(ENOENT,std::generic_category(),"directory inode missing");
    if(!inode(view,id).directory)throw std::system_error(ENOTDIR,std::generic_category(),"inode is not a directory");
    if(indexed_parents(view))return inode(view,indexed_parent(view,id));
    auto records=directory_parent_records(view);auto found=records.find(id);need(found!=records.end(),"directory is not reachable");
    return inode(view,decode_id(Bytes(found->second.begin()+8,found->second.begin()+40)));
}
InodeInfo Namespace::stat_inode(const std::string& id) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);return frontend_inode(view,id);
}
InodeInfo Namespace::lookup_child(const std::string& parent_id,const std::string& name) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);
    auto key=inode_parent(view,parent_id,name).key;
    if(!view.contains(key))throw std::system_error(ENOENT,std::generic_category(),"lookup child");
    return inode(view,decode_id(view.get(key)));
}
std::vector<unsigned char> Namespace::read_inode(const std::string& id,uint64_t offset,uint64_t count) const {
    if(offset>INT64_MAX)throw std::system_error(EINVAL,std::generic_category(),"file offset out of range");
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);auto n=frontend_inode(view,id);
    if(n.directory || n.symlink)throw std::system_error(n.directory?EISDIR:ELOOP,std::generic_category(),"inode is not a regular file");auto data=view.get(content_key(id));
    need(data.size()==n.size,"inode size mismatch");if(offset>=data.size() || !count)return {};
    auto length=static_cast<size_t>(std::min<uint64_t>(count,data.size()-offset));
    return Bytes(data.begin()+offset,data.begin()+offset+length);
}
std::string Namespace::readlink_inode(const std::string& id) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);auto n=frontend_inode(view,id);
    if(!n.symlink)throw std::system_error(EINVAL,std::generic_category(),"inode is not a symbolic link");auto data=view.get(content_key(id));need(data.size()==n.size,"symlink size mismatch");
    return std::string(data.begin(),data.end());
}
Namespace::DirectoryPage Namespace::list_inode(const std::string& id,const std::string& after,size_t limit) const {
    if(limit==0 || limit>1024)throw std::system_error(EINVAL,std::generic_category(),"invalid directory page limit");
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);
    if(!frontend_inode(view,id).directory)throw std::system_error(ENOTDIR,std::generic_category(),"inode is not a directory");
    if(!after.empty())(void)inode_parent(view,id,after);auto prefix=directory_prefix(id);
    auto keys=view.page(prefix,after.empty()?"":prefix+after,limit+1);
    DirectoryPage result;result.eof=keys.size()<=limit;if(!result.eof)keys.resize(limit);
    for(const auto& key:keys){auto name=key.substr(prefix.size());result.names.push_back(name);result.entries.push_back({name,inode(view,decode_id(view.get(key)))});}
    return result;
}
InodeInfo Namespace::create_entry(const NamespaceView& view,Changes& changes,const std::string& parent_id,
                                const std::string& name,bool directory,const Attributes& a) {
    auto p=inode_parent(view,parent_id,name);
    if(view.contains(p.key))throw std::system_error(EEXIST,std::generic_category(),"entry already exists");
    auto id=new_id(view);InodeInfo created{id,directory,a.mode.value_or(directory?0755:0644),directory?2u:1u,a.size.value_or(0),now(),now()};
    apply_attributes(created,a);created.indexed_directory=directory && indexed_parents(view);auto pn=inode(view,p.id);
    if(directory){need(pn.links<UINT64_MAX,"directory link count exhausted");++pn.links;}touch(pn);
    changes[p.key]=id_bytes(id);changes[inode_key(id)]=encode(created);changes[inode_key(p.id)]=encode(pn);changes[allocated_key(id)]=Bytes{};
    if(!directory)changes[content_key(id)]=Bytes(created.size,0);
    else if(indexed_parents(view))changes[parent_key(id)]=parent_record(p.id,p.name);
    return created;
}
InodeInfo Namespace::create_child(const std::string& parent_id,const std::string& name,bool directory,uint32_t mode) {
    Attributes a;a.mode=mode;return create_child(parent_id,name,directory,a);
}
InodeInfo Namespace::create_child(const std::string& parent_id,const std::string& name,bool directory,const Attributes& a,std::string* handle) {
    if(revision_)throw std::system_error(EROFS,std::generic_category(),"revision view is read-only");
    try{valid_attributes(a);}catch(const std::runtime_error&){throw std::system_error(EINVAL,std::generic_category(),"invalid creation attributes");}
    if(directory && a.size)throw std::system_error(EISDIR,std::generic_category(),"size requires a regular file");
    if(a.size && *a.size>256*1024)throw std::system_error(EFBIG,std::generic_category(),"prototype files are limited to 256 KiB");
    if(handle){
        if(directory || !handle->empty())throw std::system_error(EINVAL,std::generic_category(),"invalid creation handle");
        std::lock_guard lock(container_.mutex_);
        if(!container_.journal_->is_leveldb())throw std::system_error(ENOTSUP,std::generic_category(),"file handles require LevelDB");
        if(container_.handles_.size()>=4096)throw std::system_error(ENFILE,std::generic_category(),"file handle limit reached");
        NamespaceView view(*container_.journal_,view_,false);Changes changes;
        auto created=create_entry(view,changes,parent_id,name,directory,a);
        *handle=publish_open(view,created.id,Access::ReadWrite,OpenOptions{},changes);return created;
    }
    InodeInfo created;update([&](const NamespaceView& view,Changes& changes){created=create_entry(view,changes,parent_id,name,directory,a);});return created;
}
void Namespace::remove_child(const std::string& parent_id,const std::string& name,const std::string& expected,bool directory) {
    if(revision_)throw std::system_error(EROFS,std::generic_category(),"revision view is read-only");
    if(!valid_id(expected))throw std::system_error(EINVAL,std::generic_category(),"invalid expected inode");
    update([&](const NamespaceView& view,Changes& changes){auto p=inode_parent(view,parent_id,name);
        if(!view.contains(p.key))throw std::system_error(ENOENT,std::generic_category(),"entry missing");
        if(decode_id(view.get(p.key))!=expected)throw std::system_error(ESTALE,std::generic_category(),"entry identity changed");
        auto n=inode(view,expected);
        if(n.directory!=directory)throw std::system_error(n.directory?EISDIR:ENOTDIR,std::generic_category(),"wrong removal type");
        if(directory && !view.page(directory_prefix(expected),"",1).empty())throw std::system_error(ENOTEMPTY,std::generic_category(),"directory not empty");
        remove_entry(view,changes,p.id,p.name,directory);
    });
}
void Namespace::rename_child(const std::string& source_parent,const std::string& source_name,
    const std::string& destination_parent,const std::string& destination_name,const std::string& expected_source,
    const std::optional<std::string>& expected_target) {
    if(revision_)throw std::system_error(EROFS,std::generic_category(),"revision view is read-only");
    if(!valid_id(expected_source) || (expected_target && !valid_id(*expected_target)))throw std::system_error(EINVAL,std::generic_category(),"invalid expected inode");
    update([&](const NamespaceView& view,Changes& changes){auto src=inode_parent(view,source_parent,source_name),dst=inode_parent(view,destination_parent,destination_name);
        if(!view.contains(src.key))throw std::system_error(ENOENT,std::generic_category(),"source missing");
        if(decode_id(view.get(src.key))!=expected_source)throw std::system_error(ESTALE,std::generic_category(),"source identity changed");
        if(src.key==dst.key)return;
        if(view.contains(dst.key)) {
            if(!expected_target || decode_id(view.get(dst.key))!=*expected_target)throw std::system_error(ESTALE,std::generic_category(),"target identity changed");
            auto source=inode(view,expected_source),target=inode(view,*expected_target);
            if(source.directory!=target.directory)throw std::system_error(target.directory?EISDIR:ENOTDIR,std::generic_category(),"rename types differ");
            if(target.directory && !view.page(directory_prefix(target.id),"",1).empty())throw std::system_error(ENOTEMPTY,std::generic_category(),"target directory not empty");
        }else if(expected_target)throw std::system_error(ESTALE,std::generic_category(),"target disappeared");
        if(inode(view,expected_source).directory && source_parent!=destination_parent && directory_contains(view,expected_source,destination_parent))
            throw std::system_error(EINVAL,std::generic_category(),"cannot move directory into itself");
        rename_entry(view,changes,source_parent,source_name,destination_parent,destination_name);
    });
}
void Namespace::initialize() {
    need(view_=="main" && !revision_,"fs-init initializes main only");
    update([](const NamespaceView& j,Changes& changes) {
        need(!j.contains(root_key),"filesystem already initialized");
        need(j.keys(prefix).empty(),"orphan filesystem records prevent initialization");
        auto id=new_id(j);InodeInfo n{id,true,0755,2,0,now(),now()};n.indexed_directory=true;
        changes[root_key]=id_bytes(id);changes[inode_key(id)]=encode(n);changes[allocated_key(id)]=Bytes{};
        changes[parent_index_key]=Bytes{'F','F','P','I','D','X','0','1'};changes[parent_key(id)]=parent_record(id,"");
    });
}
void Namespace::mkdir(const std::string& path) {
    update([&](const NamespaceView& j,Changes& c) {
        auto p=parent(j,path);need(!j.contains(p.key),"path already exists");
        auto id=new_id(j);InodeInfo n{id,true,0755,2,0,now(),now()},pn=inode(j,p.id);n.indexed_directory=indexed_parents(j);
        need(pn.links<UINT64_MAX,"directory link count exhausted");++pn.links;touch(pn);
        c[p.key]=id_bytes(id);c[inode_key(id)]=encode(n);c[inode_key(p.id)]=encode(pn);c[allocated_key(id)]=Bytes{};
        if(indexed_parents(j))c[parent_key(id)]=parent_record(p.id,p.name);
    });
}
void Namespace::write(const std::string& path,const Bytes& value) {
    need(value.size()<=256*1024,"prototype files are limited to 256 KiB");
    update([&](const NamespaceView& j,Changes& c) {
        auto p=parent(j,path);bool exists=j.contains(p.key);auto id=exists?decode_id(j.get(p.key)):new_id(j);
        if(exists && inode(j,id).symlink) {
            Parent destination;id=resolve(j,path,true,&destination);
            if(id.empty()){p=std::move(destination);exists=false;id=new_id(j);}
        }
        auto n=exists?inode(j,id):InodeInfo{id,false,0644,1,0,now(),now()};
        need(!n.directory && !n.symlink,"cannot write non-regular file");n.size=value.size();touch(n);
        c[inode_key(id)]=encode(n);c[content_key(id)]=value;
        if(!exists){auto pn=inode(j,p.id);touch(pn);c[inode_key(p.id)]=encode(pn);c[p.key]=id_bytes(id);c[allocated_key(id)]=Bytes{};}
    });
}
void Namespace::replace_file(const std::string& path,const Bytes& value) {
    need(value.size()<=256*1024,"prototype files are limited to 256 KiB");
    update([&](const NamespaceView& view,Changes& changes) {
        auto p=parent(view,path);uint32_t mode=0644;
        if(view.contains(p.key)) {
            auto old=inode(view,decode_id(view.get(p.key)));need(!old.directory,"cannot replace directory with a file");
            if(!old.symlink)mode=old.mode;
            if(old.links==1)retire(view,changes,old);
            else {--old.links;old.changed_ns=now();changes[inode_key(old.id)]=encode(old);}
        }
        auto id=new_id(view);InodeInfo n{id,false,mode,1,value.size(),now(),now()};auto pn=inode(view,p.id);touch(pn);
        changes[inode_key(id)]=encode(n);changes[content_key(id)]=value;changes[allocated_key(id)]=Bytes{};
        changes[p.key]=id_bytes(id);changes[inode_key(p.id)]=encode(pn);
    });
}
Bytes Namespace::read(const std::string& path) const {
    std::lock_guard lock(container_.mutex_);const auto& journal=*container_.journal_;NamespaceView j(journal,view_,revision_);auto id=resolve(j,path);auto n=inode(j,id);
    need(!n.directory,"cannot read a directory");auto value=j.get(content_key(id));need(value.size()==n.size,"inode size mismatch");return value;
}
void Namespace::edit_content(const std::string& path,const std::function<bool(Bytes&)>& edit) {
    update([&](const NamespaceView& j,Changes& c) {
        auto id=resolve(j,path);auto n=inode(j,id);need(!n.directory,"cannot modify directory content");
        auto value=j.get(content_key(id));need(value.size()==n.size,"inode size mismatch");
        if(!edit(value))return;
        need(value.size()<=256*1024,"prototype files are limited to 256 KiB");
        n.size=value.size();touch(n);c[inode_key(id)]=encode(n);c[content_key(id)]=std::move(value);
    });
}
void Namespace::write_at(const std::string& path,uint64_t offset,const Bytes& value) {
    need(offset<=INT64_MAX,"file offset exceeds signed 64-bit range");
    need(value.empty() || (offset<=256*1024 && value.size()<=256*1024-offset),"prototype files are limited to 256 KiB");
    edit_content(path,[&](Bytes& data) {
        if(value.empty())return false;
        auto end=static_cast<size_t>(offset)+value.size();if(end>data.size())data.resize(end,0);
        std::copy(value.begin(),value.end(),data.begin()+static_cast<size_t>(offset));return true;
    });
}
uint64_t Namespace::append(const std::string& path,const Bytes& value) {
    need(value.size()<=256*1024,"prototype files are limited to 256 KiB");uint64_t offset=0;
    edit_content(path,[&](Bytes& data) {
        offset=data.size();need(data.size()<=256*1024 && value.size()<=256*1024-data.size(),"prototype files are limited to 256 KiB");
        if(value.empty())return false;data.insert(data.end(),value.begin(),value.end());return true;
    });return offset;
}
void Namespace::truncate(const std::string& path,uint64_t size) {
    need(size<=256*1024,"prototype files are limited to 256 KiB");
    edit_content(path,[&](Bytes& data) {if(data.size()==size)return false;data.resize(static_cast<size_t>(size),0);return true;});
}
Bytes Namespace::read_at(const std::string& path,uint64_t offset,uint64_t count) const {
    need(offset<=INT64_MAX,"file offset exceeds signed 64-bit range");auto data=read(path);
    if(offset>=data.size() || count==0)return {};
    auto start=static_cast<size_t>(offset),length=static_cast<size_t>(std::min<uint64_t>(count,data.size()-start));
    return Bytes(data.begin()+start,data.begin()+start+length);
}
std::vector<std::string> Namespace::list(const std::string& path) const {
    std::lock_guard lock(container_.mutex_);const auto& journal=*container_.journal_;NamespaceView j(journal,view_,revision_);auto id=resolve(j,path);
    need(inode(j,id).directory,"path is not a directory");auto pre=directory_prefix(id);std::vector<std::string> result;
    for(const auto& key:j.keys(pre))result.push_back(key.substr(pre.size()));return result;
}
Namespace::DirectoryCursor Namespace::open_directory(const std::string& path) const {
    std::lock_guard lock(container_.mutex_);const auto& j=*container_.journal_;need(j.is_leveldb(),"directory cursors require LevelDB");
    NamespaceView view(j,view_,revision_);auto id=resolve(view,path);need(inode(view,id).directory,"path is not a directory");
    return directory_cursor(view,id);
}
Namespace::DirectoryCursor Namespace::open_directory_inode(const std::string& id) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);
    if(!frontend_inode(view,id).directory)throw std::system_error(ENOTDIR,std::generic_category(),"inode is not a directory");
    if(!container_.journal_->is_leveldb())throw std::system_error(ENOTSUP,std::generic_category(),"directory cursors require LevelDB");
    return directory_cursor(view,id);
}
Namespace::DirectoryCursor Namespace::directory_cursor(const NamespaceView& view,const std::string& id) const {
    const auto& j=*container_.journal_;
    DirectoryCursor cursor;cursor.lifetime_=container_.lifetime_;cursor.view_=view_;cursor.revision_=revision_;cursor.prefix_=directory_prefix(id);
    if(!revision_)cursor.generation_=world_generation(j,view_);
    if(view.legacy()){need(view.keys(cursor.prefix_).empty(),"flat namespace directory is not empty");cursor.done_=true;}
    else cursor.manifest_=revision_?revision_manifest(j,view_):j.root_object(history+"world/"+view_);
    return cursor;
}
Namespace::DirectoryPage Namespace::read_directory(DirectoryCursor& cursor,size_t limit,bool attributes) const {
    need(limit>=1 && limit<=1024,"directory page limit must be 1..1024");
    std::lock_guard lock(container_.mutex_);need(cursor.lifetime_.lock()==container_.lifetime_ && cursor.view_==view_ && cursor.revision_==revision_,"directory cursor belongs to another repository/view");
    if(!revision_)need(cursor.generation_==world_generation(*container_.journal_,view_),"directory cursor revoked by restore");
    if(cursor.done_)return {{},true,{}};NamespaceView snapshot(*container_.journal_,cursor.manifest_);
    auto keys=snapshot.page(cursor.prefix_,cursor.after_,limit+1);bool eof=keys.size()<=limit;if(!eof)keys.resize(limit);
    std::vector<std::string> names;std::vector<DirectoryEntry> entries;
    for(const auto& key:keys) {
        auto name=key.substr(cursor.prefix_.size());names.push_back(name);
        if(attributes)entries.push_back({name,inode(snapshot,decode_id(snapshot.get(key)))});
    }
    if(!keys.empty())cursor.after_=keys.back();cursor.done_=eof;return {std::move(names),eof,std::move(entries)};
}
void Namespace::rewind_directory(DirectoryCursor& cursor) const {
    std::lock_guard lock(container_.mutex_);need(cursor.lifetime_.lock()==container_.lifetime_ && cursor.view_==view_ && cursor.revision_==revision_,"directory cursor belongs to another repository/view");
    if(!revision_)need(cursor.generation_==world_generation(*container_.journal_,view_),"directory cursor revoked by restore");
    cursor.after_.clear();cursor.done_=cursor.manifest_==ObjectId{};
}
InodeInfo Namespace::stat(const std::string& path) const {
    std::lock_guard lock(container_.mutex_);const auto& journal=*container_.journal_;NamespaceView j(journal,view_,revision_);return inode(j,resolve(j,path));
}
namespace {
void valid_attributes(const Namespace::Attributes& a) {
    need(!a.mode || *a.mode<=0777,"unsupported mode bits");
    need(!a.modified_ns || *a.modified_ns<=INT64_MAX,"mtime exceeds supported nanosecond range");
    need(!a.accessed_ns || *a.accessed_ns<=INT64_MAX,"atime exceeds supported nanosecond range");
    need(!a.created_ns || *a.created_ns<=INT64_MAX,"birthtime exceeds supported nanosecond range");
}
void apply_attributes(InodeInfo& n,const Namespace::Attributes& a) {
    if(a.mode)n.mode=*a.mode;if(a.modified_ns)n.modified_ns=*a.modified_ns;n.changed_ns=now();
    if(a.uid)n.uid=*a.uid;if(a.gid)n.gid=*a.gid;
    if(a.accessed_ns)n.accessed_ns=*a.accessed_ns;if(a.created_ns)n.created_ns=*a.created_ns;
    if(a.uid || a.gid || a.accessed_ns || a.created_ns)n.extended_attributes=true;
}
}
void Namespace::attribute_changes(const NamespaceView& view,Changes& changes,InodeInfo n,const Attributes& a,
                                 const std::string& inode_name,const std::string& content_name,bool linked) {
    bool resized=false;
    if(a.size) {
        if(n.directory || n.symlink)throw std::system_error(n.directory?EISDIR:EINVAL,std::generic_category(),"size requires a regular file");
        if(*a.size>256*1024)throw std::system_error(EFBIG,std::generic_category(),"prototype files are limited to 256 KiB");
        if(*a.size!=n.size) {
            auto data=linked?view.get(content_name):container_.journal_->get(content_name);
            need(data.size()==n.size,"inode size mismatch");data.resize(static_cast<size_t>(*a.size),0);
            n.size=*a.size;n.modified_ns=now();changes[content_name]=std::move(data);resized=true;
        }
    }
    if(!resized && !a.mode && !a.modified_ns && !a.uid && !a.gid && !a.accessed_ns && !a.created_ns)return;
    apply_attributes(n,a);changes[inode_name]=encode(n);
}
void Namespace::set_attributes(const std::string& path,const Attributes& a) {
    valid_attributes(a);
    update([&](const NamespaceView& view,Changes& changes) {
        auto id=resolve(view,path);attribute_changes(view,changes,inode(view,id),a,inode_key(id),content_key(id),true);
    });
}
void Namespace::set_inode_attributes(const std::string& id,const Attributes& a) {
    if(revision_)throw std::system_error(EROFS,std::generic_category(),"revision view is read-only");
    if(!valid_id(id))throw std::system_error(EINVAL,std::generic_category(),"invalid inode identity");
    try{valid_attributes(a);}catch(const std::runtime_error&){throw std::system_error(EINVAL,std::generic_category(),"unsupported inode attributes");}
    update([&](const NamespaceView& view,Changes& changes) {
        if(!view.contains(inode_key(id)))throw std::system_error(ENOENT,std::generic_category(),"inode missing");
        attribute_changes(view,changes,inode(view,id),a,inode_key(id),content_key(id),true);
    });
}
void Namespace::handle_set_attributes(const std::string& token,const Attributes& a) {
    valid_attributes(a);need(!revision_,"revision view is read-only");
    std::lock_guard lock(container_.mutex_);const auto& info=find_handle(token);
    if(a.size && !info.writable)throw std::system_error(EBADF,std::generic_category(),"size requires a writable handle");
    NamespaceView view(*container_.journal_,view_,revision_);bool linked=view.contains(inode_key(info.inode));
    auto key=linked?inode_key(info.inode):orphan_prefix(view_,info.inode)+"inode";
    auto n=linked?inode(view,info.inode):decode_inode(container_.journal_->get(key),info.inode,true);
    Changes changes;auto content=linked?content_key(info.inode):orphan_prefix(view_,info.inode)+"content";
    attribute_changes(view,changes,n,a,key,content,linked);commit_changes(view,changes);
}
namespace {
#ifdef ENOATTR
constexpr int missing_xattr=ENOATTR;
#else
constexpr int missing_xattr=ENODATA;
#endif
void valid_xattr_name(const std::string& name) {
    if(name.empty() || name.size()>255 || !std::all_of(name.begin(),name.end(),[](unsigned char c){return c>=33 && c<=126;}))
        throw std::system_error(EINVAL,std::generic_category(),"invalid xattr name");
}
std::vector<std::string> xattr_names(const InodeInfo& n){std::vector<std::string> result;if(n.xattrs)for(const auto& [name,value]:*n.xattrs)result.push_back(name);return result;}
Bytes xattr_value(const InodeInfo& n,const std::string& name) {
    valid_xattr_name(name);if(n.xattrs){auto found=n.xattrs->find(name);if(found!=n.xattrs->end())return found->second;}
    throw std::system_error(missing_xattr,std::generic_category(),"xattr missing");
}
}
std::pair<InodeInfo,std::string> Namespace::xattr_inode(const NamespaceView& view,const std::string& address,bool handle) const {
    if(!handle)return {frontend_inode(view,address),inode_key(address)};
    const auto& info=find_handle(address);if(view.contains(inode_key(info.inode)))return {inode(view,info.inode),inode_key(info.inode)};
    auto key=orphan_prefix(view_,info.inode)+"inode";return {decode_inode(container_.journal_->get(key),info.inode,true),key};
}
void Namespace::edit_xattrs(const std::string& address,bool handle,const std::string& name,const std::optional<Bytes>& value,XattrMode mode) {
    if(revision_)throw std::system_error(EROFS,std::generic_category(),"revision view is read-only");valid_xattr_name(name);
    if(mode!=XattrMode::Upsert && mode!=XattrMode::Create && mode!=XattrMode::Replace)throw std::system_error(EINVAL,std::generic_category(),"invalid xattr policy");
    if(value && value->size()>64*1024)throw std::system_error(E2BIG,std::generic_category(),"xattr exceeds 64 KiB");
    auto edit=[&](const NamespaceView& view,Changes& changes) {
        auto [n,key]=xattr_inode(view,address,handle);auto values=n.xattrs?*n.xattrs:InodeInfo::Xattrs{};bool exists=values.contains(name);
        if(value && mode==XattrMode::Create && exists)throw std::system_error(EEXIST,std::generic_category(),"xattr already exists");
        if((!value || mode==XattrMode::Replace) && !exists)throw std::system_error(missing_xattr,std::generic_category(),"xattr missing");
        if(value)values[name]=*value;else values.erase(name);n.xattrs=std::make_shared<const InodeInfo::Xattrs>(std::move(values));
        n.extended_attributes=true;n.changed_ns=now();changes[key]=encode(n);
    };
    if(!handle){update(edit);return;}
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);Changes changes;edit(view,changes);commit_changes(view,changes);
}
std::vector<std::string> Namespace::list_inode_xattrs(const std::string& id) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);return xattr_names(xattr_inode(view,id,false).first);
}
Bytes Namespace::get_inode_xattr(const std::string& id,const std::string& name) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);return xattr_value(xattr_inode(view,id,false).first,name);
}
std::vector<std::string> Namespace::list_handle_xattrs(const std::string& token) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);return xattr_names(xattr_inode(view,token,true).first);
}
Bytes Namespace::get_handle_xattr(const std::string& token,const std::string& name) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);return xattr_value(xattr_inode(view,token,true).first,name);
}
void Namespace::set_inode_xattr(const std::string& id,const std::string& name,const Bytes& value,XattrMode mode){edit_xattrs(id,false,name,value,mode);}
void Namespace::remove_inode_xattr(const std::string& id,const std::string& name){edit_xattrs(id,false,name,std::nullopt,XattrMode::Replace);}
void Namespace::set_handle_xattr(const std::string& token,const std::string& name,const Bytes& value,XattrMode mode){edit_xattrs(token,true,name,value,mode);}
void Namespace::remove_handle_xattr(const std::string& token,const std::string& name){edit_xattrs(token,true,name,std::nullopt,XattrMode::Replace);}
InodeInfo Namespace::lstat(const std::string& path) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);return inode(view,resolve(view,path,false));
}
InodeInfo Namespace::symlink_entry(const NamespaceView& view,Changes& c,const std::string& parent_id,
                                 const std::string& name,const std::string& target) {
    if(target.size()>4096)throw std::system_error(ENAMETOOLONG,std::generic_category(),"symlink target too long");
    try{target_components(target);}catch(const std::runtime_error&){throw std::system_error(EINVAL,std::generic_category(),"invalid symlink target");}
    auto p=inode_parent(view,parent_id,name);
    if(view.contains(p.key))throw std::system_error(EEXIST,std::generic_category(),"entry already exists");
    auto id=new_id(view);InodeInfo n{id,false,0777,1,target.size(),now(),now(),true};auto pn=inode(view,p.id);touch(pn);
    c[inode_key(id)]=encode(n);c[content_key(id)]=Bytes(target.begin(),target.end());c[allocated_key(id)]=Bytes{};
    c[p.key]=id_bytes(id);c[inode_key(p.id)]=encode(pn);return n;
}
InodeInfo Namespace::symlink_child(const std::string& parent_id,const std::string& name,const std::string& target) {
    return symlink_child(parent_id,name,target,Attributes{});
}
InodeInfo Namespace::symlink_child(const std::string& parent_id,const std::string& name,const std::string& target,const Attributes& a) {
    if(revision_)throw std::system_error(EROFS,std::generic_category(),"revision view is read-only");
    try{valid_attributes(a);}catch(const std::runtime_error&){throw std::system_error(EINVAL,std::generic_category(),"invalid creation attributes");}
    if(a.size)throw std::system_error(EINVAL,std::generic_category(),"symlink size comes from target");
    InodeInfo result;update([&](const NamespaceView& view,Changes& c){
        result=symlink_entry(view,c,parent_id,name,target);apply_attributes(result,a);c[inode_key(result.id)]=encode(result);
    });return result;
}
void Namespace::symlink(const std::string& target,const std::string& path) {
    target_components(target);
    update([&](const NamespaceView& view,Changes& c) {
        auto p=parent(view,path);symlink_entry(view,c,p.id,p.name,target);
    });
}
std::string Namespace::readlink(const std::string& path) const {
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);
    auto id=resolve(view,path,false);auto n=inode(view,id);need(n.symlink,"path is not a symbolic link");auto b=view.get(content_key(id));
    need(b.size()==n.size,"symlink size mismatch");std::string result(b.begin(),b.end());target_components(result);return result;
}
void Namespace::link(const std::string& source,const std::string& target) {
    update([&](const NamespaceView& j,Changes& c) {
        auto id=resolve(j,source,false);auto p=parent(j,target);link_entry(j,c,id,p.id,p.name);
    });
}
InodeInfo Namespace::link_entry(const NamespaceView& view,Changes& c,const std::string& id,
                              const std::string& parent_id,const std::string& name) {
    if(!valid_id(id))throw std::system_error(EINVAL,std::generic_category(),"invalid source inode");
    if(!view.contains(inode_key(id)))throw std::system_error(ENOENT,std::generic_category(),"source inode missing");
    auto n=inode(view,id);if(n.directory)throw std::system_error(EPERM,std::generic_category(),"directory hard links are forbidden");
    auto p=inode_parent(view,parent_id,name);
    if(view.contains(p.key))throw std::system_error(EEXIST,std::generic_category(),"entry already exists");
    if(n.links==UINT64_MAX)throw std::system_error(EMLINK,std::generic_category(),"link count exhausted");
    ++n.links;n.changed_ns=now();auto pn=inode(view,p.id);touch(pn);
    c[p.key]=id_bytes(id);c[inode_key(id)]=encode(n);c[inode_key(p.id)]=encode(pn);c[allocated_key(id)]=Bytes{};return n;
}
InodeInfo Namespace::link_child(const std::string& source,const std::string& parent_id,const std::string& name) {
    if(revision_)throw std::system_error(EROFS,std::generic_category(),"revision view is read-only");
    InodeInfo result;update([&](const NamespaceView& view,Changes& c){result=link_entry(view,c,source,parent_id,name);});return result;
}
void Namespace::retire(const NamespaceView& j,Changes& c,const InodeInfo& n) {
    c[inode_key(n.id)]=std::nullopt;if(n.directory){if(indexed_parents(j))c[parent_key(n.id)]=std::nullopt;return;}
    c[content_key(n.id)]=std::nullopt;
    auto count=container_.open_counts_.find({view_,n.id});
    if(count!=container_.open_counts_.end() && count->second) {
        auto orphan=n;orphan.links=0;orphan.changed_ns=now();auto pre=orphan_prefix(view_,n.id);
        c[pre+"inode"]=encode(orphan);c[pre+"content"]=j.get(content_key(n.id));
    }
}
void Namespace::remove(const std::string& path,bool directory) {
    update([&](const NamespaceView& j,Changes& c) {
        auto p=parent(j,path);remove_entry(j,c,p.id,p.name,directory);
    });
}
void Namespace::remove_entry(const NamespaceView& j,Changes& c,const std::string& parent_id,const std::string& name,bool directory) {
        Parent p{parent_id,name,entry_key(parent_id,name)};need(j.contains(p.key),"path not found");auto id=decode_id(j.get(p.key));auto n=inode(j,id),pn=inode(j,p.id);
        need(n.directory==directory,"wrong removal operation for inode type");
        if(directory){need(j.keys(directory_prefix(id)).empty(),"directory is not empty");--pn.links;}
        c[p.key]=std::nullopt;
        if(directory || n.links==1)retire(j,c,n);
        else {--n.links;n.changed_ns=now();c[inode_key(id)]=encode(n);}
        touch(pn);c[inode_key(p.id)]=encode(pn);

}

void Namespace::rename(const std::string& source,const std::string& target) {
    update([&](const NamespaceView& j,Changes& c) {
        auto src=parent(j,source);need(j.contains(src.key),"source not found");std::vector<std::string> ancestry;auto dst=parent(j,target,&ancestry);
        rename_entry(j,c,src.id,src.name,dst.id,dst.name,&ancestry);
    });
}
void Namespace::rename_entry(const NamespaceView& j,Changes& c,const std::string& source_parent,const std::string& source_name,
    const std::string& destination_parent,const std::string& destination_name,const std::vector<std::string>* ancestry) {
        Parent src{source_parent,source_name,entry_key(source_parent,source_name)};need(j.contains(src.key),"source not found");auto id=decode_id(j.get(src.key));auto n=inode(j,id);
        Parent dst{destination_parent,destination_name,entry_key(destination_parent,destination_name)};
        if(src.key==dst.key)return;
        if(n.directory && ancestry)need(std::find(ancestry->begin(),ancestry->end(),id)==ancestry->end(),"cannot move directory into itself");
        std::map<std::string,InodeInfo> parents;parents.emplace(src.id,inode(j,src.id));parents.emplace(dst.id,inode(j,dst.id));
        if(j.contains(dst.key)) {
            auto replaced=decode_id(j.get(dst.key));if(replaced==id)return;
            auto other=inode(j,replaced);need(other.directory==n.directory,"rename inode types differ");
            if(other.directory){need(j.keys(directory_prefix(replaced)).empty(),"target directory is not empty");--parents.at(dst.id).links;}
            if(other.directory || other.links==1)retire(j,c,other);
            else {--other.links;other.changed_ns=now();c[inode_key(replaced)]=encode(other);}
        }
        if(n.directory && src.id!=dst.id){--parents.at(src.id).links;need(parents.at(dst.id).links<UINT64_MAX,"directory link count exhausted");++parents.at(dst.id).links;}
        c[src.key]=std::nullopt;c[dst.key]=id_bytes(id);n.changed_ns=now();c[inode_key(id)]=encode(n);
        if(n.directory && indexed_parents(j))c[parent_key(id)]=parent_record(dst.id,dst.name);
        for(auto& [parent_id,pn]:parents){touch(pn);c[inode_key(parent_id)]=encode(pn);}

}

namespace {
void verify_namespace_view(const NamespaceView& j){
    (void)j.references();
    bool indexed=indexed_parents(j);auto root_id=root(j);need(inode(j,root_id).directory,"root inode is not a directory");std::set<std::string> expected;std::map<std::string,uint64_t> references;
    std::set<std::string> directories;expected.insert(root_key);
    std::vector<std::string> pending{root_id};
    while(!pending.empty()){auto id=std::move(pending.back());pending.pop_back();
        auto n=inode(j,id);expected.insert(inode_key(id));need(n.indexed_directory==(indexed && n.directory),"directory inode/index version mismatch");
        need(j.contains(allocated_key(id)) && j.get(allocated_key(id)).empty(),"inode missing allocation record");
        if(!n.directory){auto data=j.get(content_key(id));need(data.size()==n.size,"inode content size mismatch");if(n.symlink)target_components(std::string(data.begin(),data.end()));expected.insert(content_key(id));continue;}
        need(directories.insert(id).second,"directory cycle or hardlink");uint64_t subdirs=0;
        auto pre=directory_prefix(id);
        for(const auto& key:j.keys(pre)) {
            auto name=key.substr(pre.size());auto parsed=components("/"+name);need(parsed.size()==1 && parsed.front()==name,"invalid directory name");
            expected.insert(key);auto child=decode_id(j.get(key));++references[child];
            if(inode(j,child).directory)++subdirs;pending.push_back(child);
        }
        need(n.links==2+subdirs,"directory link count mismatch");
    }
    for(const auto& [id,count]:references){auto n=inode(j,id);if(!n.directory)need(n.links==count,"file link count mismatch");}
    for(const auto& key:j.keys(prefix+"allocated/")) {
        auto id=key.substr((prefix+"allocated/").size());
        need(valid_id(id) && j.get(key).empty(),"invalid allocation record");expected.insert(key);
    }
    if(indexed_parents(j)) {
        expected.insert(parent_index_key);auto records=directory_parent_records(j);
        for(const auto& [id,value]:records){auto key=parent_key(id);need(j.contains(key) && j.get(key)==value,"directory parent index mismatch");expected.insert(key);}
    }
    auto actual=j.keys(prefix);need(actual.size()==expected.size(),"orphan namespace records");
    for(const auto& key:actual)need(expected.contains(key),"unexpected namespace record");
}
}
void Namespace::verify() const {
    std::lock_guard lock(container_.mutex_);const auto& journal=*container_.journal_;NamespaceView j(journal,view_,revision_);
    verify_namespace_view(j);
    if(!revision_) {
        auto pre=history+"orphan/"+view_+"/";auto orphan_keys=journal.keys(pre);std::set<std::string> checked;
        for(const auto& key:orphan_keys) {
            auto relative=key.substr(pre.size());need(relative.size()>=33 && valid_id(relative.substr(0,32)) && relative[32]=='/',"invalid orphan root");
            auto id=relative.substr(0,32);need(relative.substr(33)=="inode" || relative.substr(33)=="content","invalid orphan field");
            if(!checked.insert(id).second)continue;
            auto count=container_.open_counts_.find({view_,id});need(count!=container_.open_counts_.end() && count->second>0,"orphan lacks an open handle");
            need(!j.contains(inode_key(id)),"orphan also appears in World tree");
            auto n=decode_inode(journal.get(orphan_prefix(view_,id)+"inode"),id,true);
            need(journal.get(orphan_prefix(view_,id)+"content").size()==n.size,"orphan content size mismatch");
        }
    }
}
void Namespace::verify_recovery(const std::string& repository){
    // No Container: opening a live World would reclaim persisted orphan roots
    // and advance the exact checkpoint we are about to publish.
    Journal journal(repository,true);journal.verify();const auto sequence=journal.sequence();
    std::set<ObjectId> manifests;std::set<std::string> worlds;
    auto manifest=[&](const ObjectId& id){if(manifests.insert(id).second){NamespaceView view(journal,id);verify_namespace_view(view);}};
    if(!journal.keys(prefix).empty() && !journal.contains(history+"world/main")){
        NamespaceView view(journal,"main");verify_namespace_view(view);worlds.insert("main");
    }
    for(const auto& key:journal.keys(history+"world/")){
        auto name=key.substr((history+"world/").size());valid_view_name(name);worlds.insert(name);
        try{manifest(journal.root_object(key));}catch(const std::exception& e){throw std::runtime_error("World "+name+": "+e.what());}
    }
    auto reader=[&](const ObjectId& id){return journal.get_object(id);};
    RevisionDag dag([&](const ObjectId& id){return RevisionRecord::read(reader,id);},manifest);
    auto revision=[&](const ObjectId& id){dag.validate(id);};
    for(const auto& key:journal.keys(history+"revision/")){
        auto name=key.substr((history+"revision/").size());valid_view_name(name);
        try{revision(journal.root_object(key));}catch(const std::exception& e){throw std::runtime_error("revision "+name+": "+e.what());}
    }
    for(const auto& key:journal.keys(history+"base/")){
        auto name=key.substr((history+"base/").size());need(worlds.contains(name),"World base has no World");
        auto bytes=journal.get(key);need(bytes.size()==32,"invalid World base");ObjectId id{};std::copy(bytes.begin(),bytes.end(),id.begin());need(id!=ObjectId{},"zero World base");revision(id);
    }
    for(const auto& key:journal.keys(history+"generation/")){
        auto name=key.substr((history+"generation/").size());need(worlds.contains(name),"generation has no World");(void)world_generation(journal,name);
    }
    std::set<std::pair<std::string,std::string>> orphans;
    for(const auto& key:journal.keys(history+"orphan/")){
        auto relative=key.substr((history+"orphan/").size());auto slash=relative.find('/');need(slash!=std::string::npos,"invalid orphan World");
        auto world=relative.substr(0,slash);need(worlds.contains(world),"orphan has no World");relative=relative.substr(slash+1);
        need(relative.size()>=33 && valid_id(relative.substr(0,32)) && relative[32]=='/',"invalid orphan identity");
        auto field=relative.substr(33);need(field=="inode" || field=="content","invalid orphan field");
        auto id=relative.substr(0,32);if(!orphans.emplace(world,id).second)continue;
        NamespaceView view(journal,world);need(!view.contains(inode_key(id)) && !view.contains(content_key(id)),"orphan also appears in World tree");
        need(view.contains(allocated_key(id)) && view.get(allocated_key(id)).empty(),"orphan missing allocation record");
        auto n=decode_inode(journal.get(orphan_prefix(world,id)+"inode"),id,true);
        need(journal.get(orphan_prefix(world,id)+"content").size()==n.size,"orphan content size mismatch");
    }
    need(journal.sequence()==sequence,"recovery validation changed checkpoint");
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
            id=new_id(view);InodeInfo n{id,false,options.mode,1,0,now(),now()};auto pn=inode(view,p.id);touch(pn);
            changes[inode_key(id)]=encode(n);changes[content_key(id)]=Bytes{};changes[allocated_key(id)]=Bytes{};
            changes[p.key]=id_bytes(id);changes[inode_key(p.id)]=encode(pn);
        }
    } else {id=resolve(view,path,!options.nofollow);need(!options.nofollow || !inode(view,id).symlink,"final symbolic link is forbidden");}
    if(changes.empty()) {
        auto n=inode(view,id);need(!n.directory && !n.symlink,"only regular file handles are implemented");
        if(options.truncate && n.size!=0){n.size=0;touch(n);changes[inode_key(id)]=encode(n);changes[content_key(id)]=Bytes{};}
    }
    return publish_open(view,id,access,options,changes);
}
std::string Namespace::open_inode(const std::string& id,Access access) {
    return open_inode(id,access,OpenOptions{});
}
std::string Namespace::open_inode(const std::string& id,Access access,const OpenOptions& options) {
    if(!valid_id(id) || options.create || options.exclusive || options.mode>0777 ||
       (access!=Access::Read && access!=Access::Write && access!=Access::ReadWrite) ||
       ((options.truncate || options.append) && access==Access::Read))
        throw std::system_error(EINVAL,std::generic_category(),"invalid inode open options");
    if(revision_ && access!=Access::Read)
        throw std::system_error(EROFS,std::generic_category(),"revision view is read-only");
    std::lock_guard lock(container_.mutex_);auto& journal=*container_.journal_;
    need(journal.is_leveldb(),"file handles require the LevelDB backend");
    need(container_.handles_.size()<4096,"file handle limit reached");
    NamespaceView view(journal,view_,revision_);
    // Unlinked inodes can only be accessed through an already pinned handle.
    if(!view.contains(inode_key(id)))throw std::system_error(ENOENT,std::generic_category(),"inode is not linked in this view");
    auto n=inode(view,id);
    if(n.directory || n.symlink)throw std::system_error(n.directory?EISDIR:ELOOP,std::generic_category(),"inode is not a regular file");
    Changes changes;
    if(options.truncate && n.size!=0){n.size=0;touch(n);changes[inode_key(id)]=encode(n);changes[content_key(id)]=Bytes{};}
    return publish_open(view,id,access,options,changes);
}
std::string Namespace::publish_open(const NamespaceView& view,const std::string& id,
                                    Access access,const OpenOptions& options,Changes& changes) {
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
    container_.compaction_cv_.notify_all();return token;
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
    need(value.size()<=256*1024,"prototype files are limited to 256 KiB");uint64_t next=0;std::shared_ptr<Container::Cursor> cursor;
    edit_handle(token,[&](Bytes& data,bool append) {
        const auto& info=find_handle(token);cursor=info.cursor;next=cursor->position;
        if(value.empty())return false;auto actual=append?uint64_t(data.size()):next;
        need(actual<=256*1024 && value.size()<=256*1024-actual,"prototype files are limited to 256 KiB");
        next=actual+value.size();if(next>data.size())data.resize(next,0);
        std::copy(value.begin(),value.end(),data.begin()+actual);return true;
    },[&]{cursor->position=next;});return value.size();
}
void Namespace::close(const std::string& token) {
    std::lock_guard lock(container_.mutex_);auto info=find_handle(token);
    if(!info.revision) {
        auto count=container_.open_counts_.find({info.view,info.inode});need(count!=container_.open_counts_.end() && count->second>0,"invalid open count");
        if(count->second==1) {
            auto& j=*container_.journal_;auto pre=orphan_prefix(info.view,info.inode);
            if(j.contains(pre+"inode"))j.transact(j.sequence(),{{pre+"inode",std::nullopt},{pre+"content",std::nullopt}});
            container_.open_counts_.erase(count);
        } else --count->second;
    }
    container_.handles_.erase(token);
}
InodeInfo Namespace::handle_stat(const std::string& token) const {
    std::lock_guard lock(container_.mutex_);const auto& info=find_handle(token);const auto& j=*container_.journal_;
    NamespaceView view(j,view_,revision_);auto key=inode_key(info.inode);
    if(view.contains(key))return inode(view,info.inode);
    need(!revision_,"revision inode missing");return decode_inode(j.get(orphan_prefix(view_,info.inode)+"inode"),info.inode,true);
}
Bytes Namespace::handle_read(const std::string& token,uint64_t offset,uint64_t count) const {
    need(offset<=INT64_MAX,"file offset exceeds signed 64-bit range");
    std::lock_guard lock(container_.mutex_);const auto& info=find_handle(token);need(info.readable,"handle is not readable");
    return read_handle_locked(info,offset,count);
}
Bytes Namespace::read_handle_locked(const Container::OpenHandle& info,uint64_t offset,uint64_t count) const {
    const auto& j=*container_.journal_;NamespaceView view(j,view_,revision_);Bytes data;InodeInfo n;
    if(view.contains(inode_key(info.inode))){n=inode(view,info.inode);data=view.get(content_key(info.inode));}
    else {need(!revision_,"revision inode missing");auto pre=orphan_prefix(view_,info.inode);
        n=decode_inode(j.get(pre+"inode"),info.inode,true);data=j.get(pre+"content");}
    need(data.size()==n.size,"inode size mismatch");if(offset>=data.size() || count==0)return {};
    auto start=static_cast<size_t>(offset),length=static_cast<size_t>(std::min<uint64_t>(count,data.size()-start));
    return Bytes(data.begin()+start,data.begin()+start+length);
}
void Namespace::edit_handle(const std::string& token,const std::function<bool(Bytes&,bool)>& edit,const std::function<void()>& committed) {
    need(!revision_,"revision view is read-only");
    std::lock_guard lock(container_.mutex_);NamespaceView view(*container_.journal_,view_,revision_);Changes changes;
    auto operation=[&] {
        const auto& info=find_handle(token);need(info.writable,"handle is not writable");
        bool linked=view.contains(inode_key(info.inode));auto pre=orphan_prefix(view_,info.inode);
        auto& j=*container_.journal_;auto n=linked?inode(view,info.inode):decode_inode(j.get(pre+"inode"),info.inode,true);
        auto data=linked?view.get(content_key(info.inode)):j.get(pre+"content");need(data.size()==n.size,"inode size mismatch");
        if(!edit(data,info.append))return;need(data.size()<=256*1024,"prototype files are limited to 256 KiB");n.size=data.size();touch(n);
        changes[linked?inode_key(info.inode):pre+"inode"]=encode(n);
        changes[linked?content_key(info.inode):pre+"content"]=std::move(data);
    };
    operation();commit_changes(view,changes);if(committed)committed();
}
void Namespace::handle_write(const std::string& token,uint64_t offset,const Bytes& value) {
    need(offset<=INT64_MAX,"file offset exceeds signed 64-bit range");
    need(value.size()<=256*1024,"prototype files are limited to 256 KiB");
    edit_handle(token,[&](Bytes& data,bool append) {
        if(value.empty())return false;auto actual=append?uint64_t(data.size()):offset;
        need(actual<=256*1024 && value.size()<=256*1024-actual,"prototype files are limited to 256 KiB");
        auto end=static_cast<size_t>(actual)+value.size();if(end>data.size())data.resize(end,0);
        std::copy(value.begin(),value.end(),data.begin()+static_cast<size_t>(actual));return true;
    });
}
uint64_t Namespace::handle_append(const std::string& token,const Bytes& value) {
    need(value.size()<=256*1024,"prototype files are limited to 256 KiB");uint64_t offset=0;
    edit_handle(token,[&](Bytes& data,bool) {
        offset=data.size();need(data.size()<=256*1024 && value.size()<=256*1024-data.size(),"prototype files are limited to 256 KiB");
        if(value.empty())return false;data.insert(data.end(),value.begin(),value.end());return true;
    });return offset;
}
void Namespace::handle_truncate(const std::string& token,uint64_t size) {
    need(size<=256*1024,"prototype files are limited to 256 KiB");
    edit_handle(token,[&](Bytes& data,bool){if(data.size()==size)return false;data.resize(size,0);return true;});
}
void Namespace::recover_orphans(Container& container) {
    // Previous-process handles have expired. Release retention roots, not objects.
    auto& j=*container.journal_;auto pre=history+"orphan/";auto keys=j.keys(pre);
    std::vector<Mutation> batch;
    for(const auto& key:keys) {
        auto relative=key.substr(pre.size());auto slash=relative.find('/');need(slash!=std::string::npos,"invalid orphan root");
        auto world=relative.substr(0,slash);valid_view_name(world);relative=relative.substr(slash+1);
        need(relative.size()>=33 && valid_id(relative.substr(0,32)) && relative[32]=='/' &&
            (relative.substr(33)=="inode" || relative.substr(33)=="content"),"invalid orphan root");
        batch.push_back({key,std::nullopt});
        if(batch.size()==4096){j.transact(j.sequence(),batch);batch.clear();}
    }
    if(!batch.empty())j.transact(j.sequence(),batch);
}

}
