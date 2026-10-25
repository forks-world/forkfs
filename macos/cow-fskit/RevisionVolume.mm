#import "RevisionVolume.h"
#include "namespace.h"
#import "VolumeView.h"
#include "SessionIdentity.h"
#include "file_content.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <unistd.h>
@interface ForkRevisionItem : FSItem
@property(nonatomic,copy) NSString *inode;
@property(nonatomic) FSItemID number;
@property(nonatomic) FSItemType type; // Immutable revision type; avoid extra metadata reads.
@property(nonatomic,copy) NSString *parentInode;
@property(nonatomic,copy) NSString *handle; // World regular-file retention until reclaim.
@end
@implementation ForkRevisionItem
@end
static NSError *err(int code){return fs_errorForPOSIXError(code);}
static FSItemType kind(const forkfs::InodeInfo& n){return n.directory?FSItemTypeDirectory:n.symlink?FSItemTypeSymlink:FSItemTypeFile;}
static std::pair<forkfs::Namespace::Attributes,FSItemAttribute> requestAttributes(FSItemSetAttributesRequest *req,bool creation,FSItemType type){
    if(!req)throw std::system_error(EINVAL,std::generic_category(),"missing attributes");
    const auto supported=(creation?FSItemAttributeType:0)|FSItemAttributeMode|FSItemAttributeSize|FSItemAttributeUID|FSItemAttributeGID|FSItemAttributeAccessTime|FSItemAttributeModifyTime|FSItemAttributeBirthTime;
    FSItemAttribute consumed=(FSItemAttribute)0;
    for(unsigned bit=0;bit<18;++bit){auto flag=(FSItemAttribute)(1UL<<bit);if([req isValid:flag]){
        if(!(flag&supported))throw std::system_error(ENOTSUP,std::generic_category(),"unsupported attribute");consumed|=flag;
    }}
    if((consumed&FSItemAttributeType) && req.type!=type)throw std::system_error(EINVAL,std::generic_category(),"creation type mismatch");
    forkfs::Namespace::Attributes a;
    if(consumed&FSItemAttributeMode){if(req.mode>0777)throw std::system_error(EINVAL,std::generic_category(),"invalid mode");a.mode=req.mode;}
    if(consumed&FSItemAttributeSize)a.size=req.size;
    if(consumed&FSItemAttributeUID)a.uid=req.uid;
    if(consumed&FSItemAttributeGID)a.gid=req.gid;
    auto ns=[](struct timespec t)->uint64_t{
        if(t.tv_sec<0 || t.tv_nsec<0 || t.tv_nsec>=1000000000 ||
           (uint64_t)t.tv_sec>((uint64_t)INT64_MAX-(uint64_t)t.tv_nsec)/1000000000)
        throw std::system_error(EINVAL,std::generic_category(),"invalid timestamp");
        return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
    };
    if(consumed&FSItemAttributeAccessTime)a.accessed_ns=ns(req.accessTime);
    if(consumed&FSItemAttributeModifyTime)a.modified_ns=ns(req.modifyTime);
    if(consumed&FSItemAttributeBirthTime)a.created_ns=ns(req.birthTime);
    return {a,consumed};
}
static std::string fileName(FSFileName *name){
    auto data=name.data;if(!data || !data.length)throw std::system_error(EINVAL,std::generic_category(),"empty name");
    auto value=std::string(static_cast<const char *>(data.bytes),data.length);
    if(value.find('\0')!=std::string::npos)throw std::system_error(EINVAL,std::generic_category(),"NUL in filename/target");return value;
}
struct WorldEnumeration {
    forkfs::VolumeView::DirectoryCursor start;
    FSDirectoryVerifier verifier;
    FSItemID parent;
    uint64_t issued=0;
    std::optional<forkfs::VolumeView::DirectoryCursor> checkpoint={};
    uint64_t checkpointIndex=0;
};
@implementation ForkRevisionVolume {
    std::shared_ptr<forkfs::Container> _store;
    std::unique_ptr<forkfs::VolumeView> _view;
    NSMutableDictionary<NSString *,ForkRevisionItem *> *_items;
    uint64_t _next;
    bool _revoked;
    bool _readOnly;
    uint64_t _nextVerifier;
    std::map<std::pair<std::string,bool>,WorldEnumeration> _enumerations;
    NSMutableDictionary<NSString *,NSNumber *> *_numbers;
}
- (instancetype)initWithStore:(std::shared_ptr<forkfs::Container>)store revision:(NSString *)revision error:(NSError **)error {
    return [self initWithStore:std::move(store) view:revision readOnly:YES error:error];
}
- (instancetype)initWithStore:(std::shared_ptr<forkfs::Container>)store world:(NSString *)world error:(NSError **)error {
    return [self initWithStore:std::move(store) view:world readOnly:NO error:error];
}
- (instancetype)initWithStore:(std::shared_ptr<forkfs::Container>)store view:(NSString *)name readOnly:(BOOL)readOnly error:(NSError **)error {
    if(!store || !name){if(error)*error=err(EINVAL);return nil;}
    if(!readOnly && (store->open_mode()!=forkfs::Container::OpenMode::ReadWrite || store->storage_engine()!="leveldb")){
        if(error)*error=err(store->open_mode()!=forkfs::Container::OpenMode::ReadWrite?EROFS:ENOTSUP);return nil;
    }
    try {
        auto view=std::make_unique<forkfs::VolumeView>(*store,name.UTF8String,readOnly);
        auto root=view->root_inode();
        self=[super initWithVolumeID:[[FSVolumeIdentifier alloc] initWithUUID:[NSUUID UUID]] volumeName:[FSFileName nameWithString:name]];
        if(self){_store=std::move(store);_view=std::move(view);_items=[NSMutableDictionary new];_numbers=[NSMutableDictionary new];_readOnly=readOnly;_next=3;_nextVerifier=3;[self item:root root:YES];}
        return self;
    }catch(const std::exception&){if(error)*error=err(EIO);return nil;}
}
- (instancetype)initWithMountCredentials:(const forkfs::MountCredentials&)credentials error:(NSError **)error {
    try {
        auto transport=std::make_shared<forkfs::MountTransport>(credentials);
        auto view=std::make_unique<forkfs::VolumeView>(transport);auto root=view->root_inode();
        NSString *name=[NSString stringWithUTF8String:credentials.name.c_str()];
        auto identity=forkfs::session_volume_uuid(credentials.generation,credentials.session);
        NSUUID *uuid=[[NSUUID alloc] initWithUUIDBytes:identity.data()];
        self=[super initWithVolumeID:[[FSVolumeIdentifier alloc] initWithUUID:uuid] volumeName:[FSFileName nameWithString:name]];
        if(self){_view=std::move(view);_items=[NSMutableDictionary new];_numbers=[NSMutableDictionary new];_readOnly=credentials.read_only;_next=3;_nextVerifier=3;[self item:root root:YES];}
        return self;
    }catch(const std::system_error& e){if(error)*error=err(e.code().value());return nil;}
    catch(const std::exception&){if(error)*error=err(EIO);return nil;}
}
- (FSItemID)numberForInode:(const forkfs::InodeInfo&)info root:(BOOL)root {
    NSString *key=[NSString stringWithUTF8String:info.id.c_str()];
    if(!_numbers[key]){
        if(!root && _next==std::numeric_limits<uint64_t>::max())
            throw std::system_error(EOVERFLOW,std::generic_category(),"item identity space exhausted");
        _numbers[key]=@(root?FSItemIDRootDirectory:(FSItemID)_next++);
    }
    return (FSItemID)_numbers[key].unsignedLongLongValue;
}
- (ForkRevisionItem *)item:(const forkfs::InodeInfo&)info root:(BOOL)root {
    return [self item:info root:root handle:nil];
}
- (ForkRevisionItem *)item:(const forkfs::InodeInfo&)info root:(BOOL)root handle:(NSString *)handle {
    NSString *key=[NSString stringWithUTF8String:info.id.c_str()];auto existing=_items[key];if(existing)return existing;
    auto item=[ForkRevisionItem new];item.inode=key;item.type=kind(info);item.number=[self numberForInode:info root:root];item.handle=handle;
    if(!_readOnly && item.type==FSItemTypeFile && !item.handle)item.handle=[NSString stringWithUTF8String:_view->open_inode(info.id,forkfs::Namespace::Access::ReadWrite).c_str()];
    if(root)item.parentInode=key;_items[key]=item;return item;
}
- (ForkRevisionItem *)checked:(FSItem *)item {
    if(_revoked || !_view || !_view->healthy())throw std::system_error(ENOTCONN,std::generic_category(),"revision volume revoked");
    if(![item isKindOfClass:[ForkRevisionItem class]])throw std::system_error(ESTALE,std::generic_category(),"foreign item");
    auto selected=(ForkRevisionItem *)item;if(_items[selected.inode]!=selected)throw std::system_error(ESTALE,std::generic_category(),"stale item");return selected;
}
- (FSItemAttributes *)snapshotAttributes:(const forkfs::InodeInfo&)info number:(FSItemID)number parent:(FSItemID)parent {
    auto a=[FSItemAttributes new];a.type=kind(info);a.fileID=number;a.mode=info.mode;a.linkCount=info.links;
    if(info.directory)a.parentID=parent;
    a.uid=info.uid;a.gid=info.gid;a.size=info.size;a.allocSize=info.size;
    a.modifyTime=(struct timespec){(time_t)(info.modified_ns/1000000000),(long)(info.modified_ns%1000000000)};
    a.changeTime=(struct timespec){(time_t)(info.changed_ns/1000000000),(long)(info.changed_ns%1000000000)};
    a.accessTime=(struct timespec){(time_t)(info.accessed_ns/1000000000),(long)(info.accessed_ns%1000000000)};
    a.birthTime=(struct timespec){(time_t)(info.created_ns/1000000000),(long)(info.created_ns%1000000000)};
    a.inhibitKernelOffloadedIO=YES;return a;
}
- (FSItemAttributes *)attributes:(const forkfs::InodeInfo&)info item:(ForkRevisionItem *)item {
    FSItemID parent=FSItemIDRootDirectory;
    if(info.directory && !_readOnly){
        auto pn=_view->parent_inode(info.id);parent=[self numberForInode:pn root:pn.id==info.id];
        item.parentInode=[NSString stringWithUTF8String:pn.id.c_str()];
    }else if(info.directory && item.parentInode && _numbers[item.parentInode])parent=(FSItemID)_numbers[item.parentInode].unsignedLongLongValue;
    return [self snapshotAttributes:info number:item.number parent:parent];
}
- (NSInteger)maximumLinkCount {return NSIntegerMax;}
- (NSInteger)maximumNameLength {return 255;}
- (NSInteger)maximumXattrSize {return 64*1024;}
- (BOOL)restrictsOwnershipChanges {return YES;}
- (BOOL)truncatesLongNames {return NO;}
- (void)enumerateDirectory:(FSItem *)directory startingAtCookie:(FSDirectoryCookie)cookie verifier:(FSDirectoryVerifier)verifier
    providingAttributes:(FSItemGetAttributesRequest *)attributes usingPacker:(FSDirectoryEntryPacker *)packer
    replyHandler:(void (^)(FSDirectoryVerifier,NSError *))reply {
    const FSDirectoryVerifier current=attributes?1:2;
    @synchronized(self){try{
        auto dir=[self checked:directory];
        if(dir.type!=FSItemTypeDirectory){reply(current,err(ENOTDIR));return;}
        if(!_readOnly || _view->remote()){[self enumerateWorld:dir cookie:cookie verifier:verifier attributes:attributes packer:packer reply:reply];return;}
        if((cookie==FSDirectoryCookieInitial && verifier!=FSDirectoryVerifierInitial && verifier!=current) ||
           (cookie!=FSDirectoryCookieInitial && verifier!=current)){reply(current,err(FSErrorInvalidDirectoryCookie));return;}
        uint64_t index=0;std::string after;bool done=false;
        if(!attributes){
            auto parent=_items[dir.parentInode];if(!parent)throw std::runtime_error("directory parent missing");
            for(unsigned i=0;i<2;++i){++index;if(index<=cookie)continue;
                if(![packer packEntryWithName:[FSFileName nameWithString:i==0?@".":@".."]
                    itemType:FSItemTypeDirectory itemID:i==0?dir.number:parent.number nextCookie:index attributes:nil]){reply(current,nil);return;}
            }
        }
        while(!done){auto page=_view->list_inode(dir.inode.UTF8String,after,128);done=page.eof;
            for(const auto& entry:page.entries){after=entry.name;++index;if(index<=cookie)continue;
                auto it=[self item:entry.inode root:NO];if(entry.inode.directory)it.parentInode=dir.inode;
                auto attr=attributes?[self attributes:entry.inode item:it]:nil;
                if(![packer packEntryWithName:[FSFileName nameWithBytes:entry.name.data() length:entry.name.size()]
                    itemType:kind(entry.inode) itemID:it.number nextCookie:index attributes:attr]){reply(current,nil);return;}
            }
        }
        reply(current,index<cookie?err(FSErrorInvalidDirectoryCookie):nil);
    }catch(const std::system_error& e){reply(current,err(e.code().value()));}catch(const std::exception&){reply(current,err(EIO));}}
}

- (void)enumerateWorld:(ForkRevisionItem *)dir cookie:(FSDirectoryCookie)cookie verifier:(FSDirectoryVerifier)verifier
    attributes:(FSItemGetAttributesRequest *)attributes packer:(FSDirectoryEntryPacker *)packer reply:(void (^)(FSDirectoryVerifier,NSError *))reply {
    auto key=std::make_pair(std::string(dir.inode.UTF8String),attributes!=nil);
    if(cookie==FSDirectoryCookieInitial && verifier==FSDirectoryVerifierInitial){
        auto cursor=_view->open_directory_inode(key.first);auto parent=_view->parent_inode(key.first);
        auto parentNumber=[self numberForInode:parent root:parent.id==key.first];
        if(_nextVerifier==UINT64_MAX)throw std::system_error(EOVERFLOW,std::generic_category(),"directory verifier exhausted");
        if(!_enumerations.contains(key) && _enumerations.size()>=32){
            auto oldest=std::min_element(_enumerations.begin(),_enumerations.end(),[](const auto& a,const auto& b){return a.second.verifier<b.second.verifier;});
            _enumerations.erase(oldest);
        }
        _enumerations.insert_or_assign(key,WorldEnumeration{std::move(cursor),(FSDirectoryVerifier)_nextVerifier++,parentNumber});
    }
    auto found=_enumerations.find(key);
    if(found==_enumerations.end() || (verifier!=FSDirectoryVerifierInitial && verifier!=found->second.verifier) ||
       (cookie!=FSDirectoryCookieInitial && verifier==FSDirectoryVerifierInitial) || cookie>found->second.issued){
        reply(verifier,err(FSErrorInvalidDirectoryCookie));return;
    }
    auto& state=found->second;auto cursor=state.start;uint64_t index=0;
    if(state.checkpoint && cookie>=state.checkpointIndex){cursor=*state.checkpoint;index=state.checkpointIndex;}
    // Sequential calls replay at most one 128-entry batch; earlier cookies can
    // still replay from the pinned root without retaining all directory entries.
    if(!attributes && index==0)for(unsigned i=0;i<2;++i){++index;if(index<=cookie)continue;
        if(![packer packEntryWithName:[FSFileName nameWithString:i==0?@".":@".."] itemType:FSItemTypeDirectory
            itemID:i==0?dir.number:state.parent nextCookie:index attributes:nil]){reply(state.verifier,nil);return;}
        state.issued=std::max(state.issued,index);
    }
    bool done=false;
    while(!done){auto batchStart=cursor;auto batchIndex=index;auto page=_view->read_directory(cursor,128,true);done=page.eof;
        for(const auto& entry:page.entries){++index;if(index<=cookie)continue;
            // Enumeration must not open a live inode: it may have been deleted
            // since this snapshot. Only numeric identity and frozen attributes.
            auto number=[self numberForInode:entry.inode root:NO];
            auto a=attributes?[self snapshotAttributes:entry.inode number:number parent:dir.number]:nil;
            if(![packer packEntryWithName:[FSFileName nameWithBytes:entry.name.data() length:entry.name.size()]
                itemType:kind(entry.inode) itemID:number nextCookie:index attributes:a]){state.checkpoint=std::move(batchStart);state.checkpointIndex=batchIndex;reply(state.verifier,nil);return;}
            state.issued=std::max(state.issued,index);
        }
        state.checkpoint=cursor;state.checkpointIndex=index;
    }
    reply(state.verifier,nil);
}

- (FSVolumeSupportedCapabilities *)supportedVolumeCapabilities {
    auto c=[FSVolumeSupportedCapabilities new];c.supportsHardLinks=YES;c.supportsSymbolicLinks=YES;c.supports64BitObjectIDs=YES;
    c.supportsPersistentObjectIDs=NO;c.caseFormat=FSVolumeCaseFormatSensitive;return c;
}
- (FSStatFSResult *)volumeStatistics {
    @synchronized(self){auto result=[[FSStatFSResult alloc] initWithFileSystemTypeName:@"forkrevision"];result.blockSize=4096;result.ioSize=65536;
        if(!_revoked && _view && _view->healthy())try{auto space=_store?_store->storage_space():_view->space();result.totalBlocks=space.first/4096;result.availableBlocks=space.second/4096;result.freeBlocks=result.availableBlocks;result.usedBlocks=result.totalBlocks-result.freeBlocks;}catch(const std::exception&){/* Report no available space on a backing error. */}
        return result;}
}
- (NSError *)revoke {@synchronized(self){
    _revoked=true;
    if(_view && _view->remote()){
        NSError *failure=nil;try{_view->revoke();}catch(const std::system_error& e){failure=err(e.code().value());}catch(...){failure=err(EIO);}
        [_items removeAllObjects];[_numbers removeAllObjects];_enumerations.clear();_view.reset();return failure;
    }
    try{for(ForkRevisionItem *item in _items.allValues)if(item.handle){_view->close(item.handle.UTF8String);item.handle=nil;}}
    catch(const std::system_error& e){return err(e.code().value());}catch(const std::exception&){return err(EIO);}
    [_items removeAllObjects];[_numbers removeAllObjects];_enumerations.clear();_view.reset();_store.reset();return nil;
}}
- (void)dealloc {[self revoke];}
- (FSItem *)rootItem {@synchronized(self){if(_revoked || !_view || !_view->healthy())return nil;try{return [self item:_view->root_inode() root:YES];}catch(...){return nil;}}}
- (void)activateWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(FSItem *,NSError *))reply {
    @synchronized(self){try{if(_revoked || !_view || !_view->healthy()){reply(nil,err(ENOTCONN));return;}reply([self item:_view->root_inode() root:YES],nil);}catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)deactivateWithOptions:(FSDeactivateOptions)options replyHandler:(void (^)(NSError *))reply {reply([self revoke]);}
- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(NSError *))reply {@synchronized(self){reply((_revoked || !_view || !_view->healthy())?err(ENOTCONN):nil);}}
- (void)unmountWithReplyHandler:(void (^)(void))reply {[self revoke];reply();}
- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError *))reply {@synchronized(self){try{if(_revoked || !_view || !_view->healthy()){reply(err(ENOTCONN));return;}_view->sync();reply(nil);}catch(const std::system_error& e){reply(err(e.code().value()));}catch(...){reply(err(EIO));}}}
- (void)getAttributes:(FSItemGetAttributesRequest *)desired ofItem:(FSItem *)item replyHandler:(void (^)(FSItemAttributes *,NSError *))reply {
    @synchronized(self){try{auto it=[self checked:item];reply([self attributes:(it.handle?_view->handle_stat(it.handle.UTF8String):_view->stat_inode(it.inode.UTF8String)) item:it],nil);}catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)lookupItemNamed:(FSFileName *)name inDirectory:(FSItem *)directory replyHandler:(void (^)(FSItem *,FSFileName *,NSError *))reply {
    @synchronized(self){try{
        auto dir=[self checked:directory];
        if(dir.type!=FSItemTypeDirectory){reply(nil,nil,err(ENOTDIR));return;}
        auto data=name.data;
        if(!data || data.length==0){reply(nil,nil,err(EINVAL));return;}
        if(data.length>255){reply(nil,nil,err(ENAMETOOLONG));return;}
        std::string component((const char *)data.bytes,data.length);
        if(component.find('/')!=std::string::npos || component.find('\0')!=std::string::npos){reply(nil,nil,err(EINVAL));return;}
        if(component=="." || component==".."){
            ForkRevisionItem *selected=dir;
            if(component==".."){
                if(_readOnly)selected=_items[dir.parentInode];
                else {auto parent=_view->parent_inode(dir.inode.UTF8String);selected=[self item:parent root:parent.id==std::string(dir.inode.UTF8String)];dir.parentInode=selected.inode;}
            }
            if(!selected){reply(nil,nil,err(ESTALE));return;}
            reply(selected,name,nil);return;
        }
        auto n=_view->lookup_child(dir.inode.UTF8String,component);auto child=[self item:n root:NO];
        if(n.directory)child.parentInode=dir.inode;reply(child,name,nil);
    }catch(const std::system_error& e){reply(nil,nil,err(e.code().value()));}catch(const std::exception&){reply(nil,nil,err(EIO));}}
}
- (void)reclaimItem:(FSItem *)item replyHandler:(void (^)(NSError *))reply {
    @synchronized(self){try{auto it=[self checked:item];if(!_readOnly){
        if(it.handle)_view->close(it.handle.UTF8String);it.handle=nil;[_items removeObjectForKey:it.inode];
        _enumerations.erase({std::string(it.inode.UTF8String),false});_enumerations.erase({std::string(it.inode.UTF8String),true});
    }reply(nil);}catch(const std::system_error& e){reply(err(e.code().value()));}catch(const std::exception&){reply(err(EIO));}}
} // Revisions retain FSItems; Worlds retain only numeric identity after reclaim.
- (void)readSymbolicLink:(FSItem *)item replyHandler:(void (^)(FSFileName *,NSError *))reply {
    @synchronized(self){try{auto it=[self checked:item];if(it.type!=FSItemTypeSymlink){reply(nil,err(EINVAL));return;}auto data=_view->readlink_inode(it.inode.UTF8String);reply([FSFileName nameWithBytes:data.data() length:data.size()],nil);}catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)readFromFile:(FSItem *)item offset:(off_t)offset length:(size_t)length intoBuffer:(FSMutableFileDataBuffer *)buffer replyHandler:(void (^)(size_t,NSError *))reply {
    if(offset<0 || !buffer){reply(0,err(EINVAL));return;}
    @synchronized(self){try{auto it=[self checked:item];if(it.type!=FSItemTypeFile){reply(0,err(it.type==FSItemTypeDirectory?EISDIR:EINVAL));return;}auto data=it.handle?_view->handle_read(it.handle.UTF8String,offset,std::min<uint64_t>(MIN(length,buffer.length),_view->max_io())):_view->read_inode(it.inode.UTF8String,offset,std::min<uint64_t>(MIN(length,buffer.length),_view->max_io()));if(!data.empty()){if(!buffer.mutableBytes){reply(0,err(EFAULT));return;}memcpy(buffer.mutableBytes,data.data(),data.size());}reply(data.size(),nil);}catch(const std::system_error& e){reply(0,err(e.code().value()));}catch(const std::exception&){reply(0,err(EIO));}}
}
// Route all names through the versioned inode; do not use AppleDouble sidecars.
- (void)getXattrNamed:(FSFileName *)name ofItem:(FSItem *)item replyHandler:(void (^)(NSData *,NSError *))reply {
    @synchronized(self){try{auto it=[self checked:item];auto data=name.data;if(!data || !data.length){reply(nil,err(EINVAL));return;}
        auto key=std::string(static_cast<const char *>(data.bytes),data.length);
        auto value=it.handle?_view->get_handle_xattr(it.handle.UTF8String,key):_view->get_inode_xattr(it.inode.UTF8String,key);
        reply([NSData dataWithBytes:value.data() length:value.size()],nil);
    }catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)listXattrsOfItem:(FSItem *)item replyHandler:(void (^)(NSArray<FSFileName *> *,NSError *))reply {
    @synchronized(self){try{auto it=[self checked:item];auto names=[NSMutableArray<FSFileName *> new];
        for(const auto& name:(it.handle?_view->list_handle_xattrs(it.handle.UTF8String):_view->list_inode_xattrs(it.inode.UTF8String)))[names addObject:[FSFileName nameWithBytes:name.data() length:name.size()]];reply(names,nil);
    }catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (NSError *)readOnlyErrorForItems:(NSArray<FSItem *> *)items {
    @synchronized(self){try{for(FSItem *item in items)[self checked:item];return err(_readOnly?EROFS:ENOTSUP);}catch(const std::system_error& e){return err(e.code().value());}catch(const std::exception&){return err(EIO);}}
}
- (void)setXattrNamed:(FSFileName *)name toData:(NSData *)value onItem:(FSItem *)item policy:(FSSetXattrPolicy)policy replyHandler:(void (^)(NSError *))reply {
    @synchronized(self){try{
        auto it=[self checked:item];if(_readOnly){reply(err(EROFS));return;}
        auto data=name.data;if(!data || !data.length || (!value && policy!=FSSetXattrPolicyDelete)){reply(err(EINVAL));return;}
        auto key=std::string(static_cast<const char *>(data.bytes),data.length);
        if(policy==FSSetXattrPolicyDelete){
            if(it.handle)_view->remove_handle_xattr(it.handle.UTF8String,key);else _view->remove_inode_xattr(it.inode.UTF8String,key);
        }else{
            forkfs::Namespace::XattrMode mode;
            switch(policy){
                case FSSetXattrPolicyAlwaysSet:mode=forkfs::Namespace::XattrMode::Upsert;break;
                case FSSetXattrPolicyMustCreate:mode=forkfs::Namespace::XattrMode::Create;break;
                case FSSetXattrPolicyMustReplace:mode=forkfs::Namespace::XattrMode::Replace;break;
                default:reply(err(EINVAL));return;
            }
            if(value.length>64*1024){reply(err(E2BIG));return;}
            std::vector<unsigned char> bytes(value.length);if(value.length)memcpy(bytes.data(),value.bytes,value.length);
            if(it.handle)_view->set_handle_xattr(it.handle.UTF8String,key,bytes,mode);else _view->set_inode_xattr(it.inode.UTF8String,key,bytes,mode);
        }
        reply(nil);
    }catch(const std::system_error& e){reply(err(e.code().value()));}catch(const std::exception&){reply(err(EIO));}}
}
- (void)writeContents:(NSData *)contents toFile:(FSItem *)item atOffset:(off_t)offset replyHandler:(void (^)(size_t,NSError *))reply {
    @synchronized(self){try{
        auto it=[self checked:item];if(_readOnly){reply(0,err(EROFS));return;}
        if(it.type!=FSItemTypeFile){reply(0,err(it.type==FSItemTypeDirectory?EISDIR:EINVAL));return;}
        if(!contents || offset<0){reply(0,err(EINVAL));return;}
        if(contents.length>uint64_t(INT64_MAX)-(uint64_t)offset){reply(0,err(EFBIG));return;}
        std::vector<unsigned char> bytes(std::min<uint64_t>(contents.length,_view->max_io()));if(!bytes.empty())memcpy(bytes.data(),contents.bytes,bytes.size());
        _view->handle_write(it.handle.UTF8String,offset,bytes);reply(bytes.size(),nil);
    }catch(const std::system_error& e){reply(0,err(e.code().value()));}catch(const std::exception&){reply(0,err(EIO));}}
}
- (void)setAttributes:(FSItemSetAttributesRequest *)req onItem:(FSItem *)item
         replyHandler:(void (^)(FSItemAttributes *_Nullable, NSError *_Nullable))reply {
    @synchronized(self){try{
        auto it=[self checked:item];if(_readOnly){reply(nil,err(EROFS));return;}
        auto [a,consumed]=requestAttributes(req,false,FSItemTypeFile);
        if(it.handle)_view->handle_set_attributes(it.handle.UTF8String,a);else _view->set_inode_attributes(it.inode.UTF8String,a);
        req.consumedAttributes=consumed;
        reply([self attributes:(it.handle?_view->handle_stat(it.handle.UTF8String):_view->stat_inode(it.inode.UTF8String)) item:it],nil);
    }catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)createItemNamed:(FSFileName *)name type:(FSItemType)type inDirectory:(FSItem *)directory
             attributes:(FSItemSetAttributesRequest *)req replyHandler:(void (^)(FSItem *,FSFileName *,NSError *))reply {
    @synchronized(self){try{
        auto dir=[self checked:directory];if(_readOnly){reply(nil,nil,err(EROFS));return;}
        if(type!=FSItemTypeFile && type!=FSItemTypeDirectory){reply(nil,nil,err(ENOTSUP));return;}
        auto [a,consumed]=requestAttributes(req,true,type);auto component=fileName(name);
        if(_next==std::numeric_limits<uint64_t>::max()){reply(nil,nil,err(EOVERFLOW));return;}
        std::string handle;auto n=_view->create_child(dir.inode.UTF8String,component,type==FSItemTypeDirectory,a,type==FSItemTypeFile?&handle:nullptr);
        auto created=[self item:n root:NO handle:handle.empty()?nil:[NSString stringWithUTF8String:handle.c_str()]];
        if(n.directory)created.parentInode=dir.inode;req.consumedAttributes=consumed;reply(created,name,nil);
    }catch(const std::system_error& e){reply(nil,nil,err(e.code().value()));}catch(const std::exception&){reply(nil,nil,err(EIO));}}
}
- (void)createSymbolicLinkNamed:(FSFileName *)name inDirectory:(FSItem *)directory
                     attributes:(FSItemSetAttributesRequest *)req linkContents:(FSFileName *)contents replyHandler:(void (^)(FSItem *,FSFileName *,NSError *))reply {
    @synchronized(self){try{
        auto dir=[self checked:directory];if(_readOnly){reply(nil,nil,err(EROFS));return;}
        auto [a,consumed]=requestAttributes(req,true,FSItemTypeSymlink);auto component=fileName(name),target=fileName(contents);
        if(_next==std::numeric_limits<uint64_t>::max()){reply(nil,nil,err(EOVERFLOW));return;}
        auto n=_view->symlink_child(dir.inode.UTF8String,component,target,a);auto created=[self item:n root:NO];
        req.consumedAttributes=consumed;reply(created,name,nil);
    }catch(const std::system_error& e){reply(nil,nil,err(e.code().value()));}catch(const std::exception&){reply(nil,nil,err(EIO));}}
}
- (void)createLinkToItem:(FSItem *)item named:(FSFileName *)name inDirectory:(FSItem *)directory replyHandler:(void (^)(FSFileName *,NSError *))reply {
    @synchronized(self){try{
        auto it=[self checked:item],dir=[self checked:directory];if(_readOnly){reply(nil,err(EROFS));return;}
        if(it.type==FSItemTypeDirectory){reply(nil,err(ENOTSUP));return;}
        _view->link_child(it.inode.UTF8String,dir.inode.UTF8String,fileName(name));reply(name,nil);
    }catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)removeItem:(FSItem *)item named:(FSFileName *)name fromDirectory:(FSItem *)directory replyHandler:(void (^)(NSError *))reply {
    @synchronized(self){try{
        auto it=[self checked:item],dir=[self checked:directory];if(_readOnly){reply(err(EROFS));return;}
        _view->remove_child(dir.inode.UTF8String,fileName(name),it.inode.UTF8String,it.type==FSItemTypeDirectory);reply(nil);
    }catch(const std::system_error& e){reply(err(e.code().value()));}catch(const std::exception&){reply(err(EIO));}}
}
- (void)renameItem:(FSItem *)item inDirectory:(FSItem *)sourceDirectory named:(FSFileName *)sourceName
         toNewName:(FSFileName *)destinationName inDirectory:(FSItem *)destinationDirectory overItem:(FSItem *)overItem replyHandler:(void (^)(FSFileName *,NSError *))reply {
    @synchronized(self){try{
        auto it=[self checked:item],src=[self checked:sourceDirectory],dst=[self checked:destinationDirectory];
        std::optional<std::string> target;if(overItem)target=[self checked:overItem].inode.UTF8String;
        if(_readOnly){reply(nil,err(EROFS));return;}
        _view->rename_child(src.inode.UTF8String,fileName(sourceName),dst.inode.UTF8String,fileName(destinationName),it.inode.UTF8String,target);
        if(it.type==FSItemTypeDirectory)it.parentInode=dst.inode;reply(destinationName,nil);
    }catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
@end
