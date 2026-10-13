#import "RevisionVolume.h"
#include "namespace.h"
#include <cstring>
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
@implementation ForkRevisionVolume {
    std::shared_ptr<forkfs::Container> _store;
    std::unique_ptr<forkfs::Namespace> _view;
    NSMutableDictionary<NSString *,ForkRevisionItem *> *_items;
    uint64_t _next;
    bool _revoked;
    bool _readOnly;
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
        auto view=std::make_unique<forkfs::Namespace>(*store,name.UTF8String,readOnly);
        auto root=view->root_inode();
        self=[super initWithVolumeID:[[FSVolumeIdentifier alloc] initWithUUID:[NSUUID UUID]] volumeName:[FSFileName nameWithString:name]];
        if(self){_store=std::move(store);_view=std::move(view);_items=[NSMutableDictionary new];_numbers=[NSMutableDictionary new];_readOnly=readOnly;_next=3;[self item:root root:YES];}
        return self;
    }catch(const std::exception&){if(error)*error=err(EIO);return nil;}
}
- (ForkRevisionItem *)item:(const forkfs::InodeInfo&)info root:(BOOL)root {
    NSString *key=[NSString stringWithUTF8String:info.id.c_str()];auto existing=_items[key];if(existing)return existing;
    if(!_numbers[key] && _numbers.count>=4096)throw std::system_error(ENFILE,std::generic_category(),"revision item quota exceeded");
    auto item=[ForkRevisionItem new];item.inode=key;item.type=kind(info);item.number=_numbers[key]?(FSItemID)_numbers[key].unsignedLongLongValue:root?FSItemIDRootDirectory:(FSItemID)_next++;
    if(!_readOnly && item.type==FSItemTypeFile)item.handle=[NSString stringWithUTF8String:_view->open_inode(info.id,forkfs::Namespace::Access::ReadWrite).c_str()];
    if(root)item.parentInode=key;_numbers[key]=@(item.number);_items[key]=item;return item;
}
- (ForkRevisionItem *)checked:(FSItem *)item {
    if(_revoked)throw std::system_error(ENOTCONN,std::generic_category(),"revision volume revoked");
    if(![item isKindOfClass:[ForkRevisionItem class]])throw std::system_error(ESTALE,std::generic_category(),"foreign item");
    auto selected=(ForkRevisionItem *)item;if(_items[selected.inode]!=selected)throw std::system_error(ESTALE,std::generic_category(),"stale item");return selected;
}
- (FSItemAttributes *)attributes:(const forkfs::InodeInfo&)info item:(ForkRevisionItem *)item {
    auto a=[FSItemAttributes new];a.type=kind(info);a.fileID=item.number;a.mode=info.mode;a.linkCount=info.links;
    if(info.directory && !_readOnly){
        auto parent=_view->parent_inode(info.id);auto parentItem=[self item:parent root:parent.id==info.id];
        item.parentInode=parentItem.inode;a.parentID=parentItem.number;
    }else if(info.directory && item.parentInode && _numbers[item.parentInode])a.parentID=(FSItemID)_numbers[item.parentInode].unsignedLongLongValue;
    a.uid=info.uid;a.gid=info.gid;a.size=info.size;a.allocSize=info.size;
    a.modifyTime=(struct timespec){(time_t)(info.modified_ns/1000000000),(long)(info.modified_ns%1000000000)};
    a.changeTime=(struct timespec){(time_t)(info.changed_ns/1000000000),(long)(info.changed_ns%1000000000)};
    a.accessTime=(struct timespec){(time_t)(info.accessed_ns/1000000000),(long)(info.accessed_ns%1000000000)};
    a.birthTime=(struct timespec){(time_t)(info.created_ns/1000000000),(long)(info.created_ns%1000000000)};
    a.inhibitKernelOffloadedIO=YES;return a;
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
        // Revision ordinal cookies are unsafe on a mutable directory. World
        // enumeration will use the backend's pinned cursor in the next stage.
        if(!_readOnly){reply(current,err(ENOTSUP));return;}
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

- (FSVolumeSupportedCapabilities *)supportedVolumeCapabilities {
    auto c=[FSVolumeSupportedCapabilities new];c.supportsHardLinks=YES;c.supportsSymbolicLinks=YES;c.supports64BitObjectIDs=YES;
    c.supportsPersistentObjectIDs=NO;c.caseFormat=FSVolumeCaseFormatSensitive;return c;
}
- (FSStatFSResult *)volumeStatistics {return [[FSStatFSResult alloc] initWithFileSystemTypeName:@"forkrevision"];}
- (NSError *)revoke {@synchronized(self){
    _revoked=true;
    try{for(ForkRevisionItem *item in _items.allValues)if(item.handle){_view->close(item.handle.UTF8String);item.handle=nil;}}
    catch(const std::system_error& e){return err(e.code().value());}catch(const std::exception&){return err(EIO);}
    [_items removeAllObjects];[_numbers removeAllObjects];_view.reset();_store.reset();return nil;
}}
- (void)dealloc {[self revoke];}
- (FSItem *)rootItem {@synchronized(self){if(_revoked)return nil;return [self item:_view->root_inode() root:YES];}}
- (void)activateWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(FSItem *,NSError *))reply {
    @synchronized(self){try{if(_revoked){reply(nil,err(ENOTCONN));return;}reply([self item:_view->root_inode() root:YES],nil);}catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)deactivateWithOptions:(FSDeactivateOptions)options replyHandler:(void (^)(NSError *))reply {reply([self revoke]);}
- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(NSError *))reply {@synchronized(self){reply(_revoked?err(ENOTCONN):nil);}}
- (void)unmountWithReplyHandler:(void (^)(void))reply {[self revoke];reply();}
- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError *))reply {@synchronized(self){reply(_revoked?err(ENOTCONN):nil);}}
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
    }reply(nil);}catch(const std::system_error& e){reply(err(e.code().value()));}catch(const std::exception&){reply(err(EIO));}}
} // Revisions retain FSItems; Worlds retain only numeric identity after reclaim.
- (void)readSymbolicLink:(FSItem *)item replyHandler:(void (^)(FSFileName *,NSError *))reply {
    @synchronized(self){try{auto it=[self checked:item];if(it.type!=FSItemTypeSymlink){reply(nil,err(EINVAL));return;}auto data=_view->readlink_inode(it.inode.UTF8String);reply([FSFileName nameWithBytes:data.data() length:data.size()],nil);}catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)readFromFile:(FSItem *)item offset:(off_t)offset length:(size_t)length intoBuffer:(FSMutableFileDataBuffer *)buffer replyHandler:(void (^)(size_t,NSError *))reply {
    if(offset<0 || !buffer){reply(0,err(EINVAL));return;}
    @synchronized(self){try{auto it=[self checked:item];if(it.type!=FSItemTypeFile){reply(0,err(it.type==FSItemTypeDirectory?EISDIR:EINVAL));return;}auto data=it.handle?_view->handle_read(it.handle.UTF8String,offset,MIN(length,buffer.length)):_view->read_inode(it.inode.UTF8String,offset,MIN(length,buffer.length));if(!data.empty()){if(!buffer.mutableBytes){reply(0,err(EFAULT));return;}memcpy(buffer.mutableBytes,data.data(),data.size());}reply(data.size(),nil);}catch(const std::system_error& e){reply(0,err(e.code().value()));}catch(const std::exception&){reply(0,err(EIO));}}
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
        if((uint64_t)offset>256*1024 || contents.length>256*1024-(uint64_t)offset){reply(0,err(EFBIG));return;}
        std::vector<unsigned char> bytes(contents.length);if(contents.length)memcpy(bytes.data(),contents.bytes,contents.length);
        _view->handle_write(it.handle.UTF8String,offset,bytes);reply(bytes.size(),nil);
    }catch(const std::system_error& e){reply(0,err(e.code().value()));}catch(const std::exception&){reply(0,err(EIO));}}
}
- (void)setAttributes:(FSItemSetAttributesRequest *)req onItem:(FSItem *)item
         replyHandler:(void (^)(FSItemAttributes *_Nullable, NSError *_Nullable))reply {
    @synchronized(self){try{
        auto it=[self checked:item];if(_readOnly){reply(nil,err(EROFS));return;}
        if(!req){reply(nil,err(EINVAL));return;}
        const auto supported=FSItemAttributeMode|FSItemAttributeSize|FSItemAttributeUID|FSItemAttributeGID|FSItemAttributeAccessTime|FSItemAttributeModifyTime|FSItemAttributeBirthTime;
        FSItemAttribute consumed=(FSItemAttribute)0;
        for(unsigned bit=0;bit<18;++bit){auto flag=(FSItemAttribute)(1UL<<bit);if([req isValid:flag]){
            if(!(flag&supported)){reply(nil,err(ENOTSUP));return;}consumed|=flag;
        }}
        forkfs::Namespace::Attributes a;
        if(consumed&FSItemAttributeMode){if(req.mode>0777){reply(nil,err(EINVAL));return;}a.mode=req.mode;}
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
        if(it.handle)_view->handle_set_attributes(it.handle.UTF8String,a);else _view->set_inode_attributes(it.inode.UTF8String,a);
        req.consumedAttributes=consumed;
        reply([self attributes:(it.handle?_view->handle_stat(it.handle.UTF8String):_view->stat_inode(it.inode.UTF8String)) item:it],nil);
    }catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)createItemNamed:(FSFileName *)name type:(FSItemType)type inDirectory:(FSItem *)directory
             attributes:(FSItemSetAttributesRequest *)req
           replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {reply(nil,nil,[self readOnlyErrorForItems:@[directory]]);}
- (void)createSymbolicLinkNamed:(FSFileName *)name inDirectory:(FSItem *)directory
                     attributes:(FSItemSetAttributesRequest *)req linkContents:(FSFileName *)contents
                   replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {reply(nil,nil,[self readOnlyErrorForItems:@[directory]]);}
- (void)createLinkToItem:(FSItem *)item named:(FSFileName *)name inDirectory:(FSItem *)directory
            replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply {reply(nil,[self readOnlyErrorForItems:@[item,directory]]);}
- (void)removeItem:(FSItem *)item named:(FSFileName *)name fromDirectory:(FSItem *)directory
      replyHandler:(void (^)(NSError *_Nullable))reply {reply([self readOnlyErrorForItems:@[item,directory]]);}
- (void)renameItem:(FSItem *)item inDirectory:(FSItem *)sourceDirectory named:(FSFileName *)sourceName
         toNewName:(FSFileName *)destinationName inDirectory:(FSItem *)destinationDirectory
          overItem:(FSItem *_Nullable)overItem replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply {reply(nil,[self readOnlyErrorForItems:overItem?@[item,sourceDirectory,destinationDirectory,overItem]:@[item,sourceDirectory,destinationDirectory]]);}
@end
