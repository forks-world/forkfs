#import "RevisionVolume.h"
#include "namespace.h"
#include <cstring>
#include <unistd.h>
@interface ForkRevisionItem : FSItem
@property(nonatomic,copy) NSString *inode;
@property(nonatomic) FSItemID number;
@property(nonatomic,copy) NSString *parentInode;
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
}
- (instancetype)initWithStore:(std::shared_ptr<forkfs::Container>)store revision:(NSString *)revision error:(NSError **)error {
    if(!store || !revision){if(error)*error=err(EINVAL);return nil;}
    try {
        auto view=std::make_unique<forkfs::Namespace>(*store,revision.UTF8String,true);
        auto root=view->root_inode();
        self=[super initWithVolumeID:[[FSVolumeIdentifier alloc] initWithUUID:[NSUUID UUID]] volumeName:[FSFileName nameWithString:revision]];
        if(self){_store=std::move(store);_view=std::move(view);_items=[NSMutableDictionary new];_next=3;[self item:root root:YES];}
        return self;
    }catch(const std::exception&){if(error)*error=err(EIO);return nil;}
}
- (ForkRevisionItem *)item:(const forkfs::InodeInfo&)info root:(BOOL)root {
    NSString *key=[NSString stringWithUTF8String:info.id.c_str()];auto existing=_items[key];if(existing)return existing;
    if(_items.count>=4096)throw std::runtime_error("revision item quota exceeded");
    auto item=[ForkRevisionItem new];item.inode=key;item.number=root?FSItemIDRootDirectory:(FSItemID)_next++;if(root)item.parentInode=key;_items[key]=item;return item;
}
- (ForkRevisionItem *)checked:(FSItem *)item {
    if(_revoked)throw std::system_error(ENOTCONN,std::generic_category(),"revision volume revoked");
    if(![item isKindOfClass:[ForkRevisionItem class]])throw std::runtime_error("foreign item");
    auto selected=(ForkRevisionItem *)item;if(_items[selected.inode]!=selected)throw std::runtime_error("stale item");return selected;
}
- (FSItemAttributes *)attributes:(const forkfs::InodeInfo&)info item:(ForkRevisionItem *)item {
    auto a=[FSItemAttributes new];a.type=kind(info);a.fileID=item.number;a.mode=info.mode;a.linkCount=info.links;
    a.uid=getuid();a.gid=getgid();a.size=info.size;a.allocSize=info.size;
    a.modifyTime=(struct timespec){(time_t)(info.modified_ns/1000000000),(long)(info.modified_ns%1000000000)};
    a.changeTime=(struct timespec){(time_t)(info.changed_ns/1000000000),(long)(info.changed_ns%1000000000)};
    a.inhibitKernelOffloadedIO=YES;return a;
}
- (NSInteger)maximumLinkCount {return NSIntegerMax;}
- (NSInteger)maximumNameLength {return 255;}
- (BOOL)restrictsOwnershipChanges {return YES;}
- (BOOL)truncatesLongNames {return NO;}
- (void)enumerateDirectory:(FSItem *)directory startingAtCookie:(FSDirectoryCookie)cookie verifier:(FSDirectoryVerifier)verifier
    providingAttributes:(FSItemGetAttributesRequest *)attributes usingPacker:(FSDirectoryEntryPacker *)packer
    replyHandler:(void (^)(FSDirectoryVerifier,NSError *))reply {
    const FSDirectoryVerifier current=attributes?1:2;
    if((cookie==FSDirectoryCookieInitial && verifier!=FSDirectoryVerifierInitial && verifier!=current) ||
       (cookie!=FSDirectoryCookieInitial && verifier!=current)){reply(current,err(FSErrorInvalidDirectoryCookie));return;}
    @synchronized(self){try{
        auto dir=[self checked:directory];uint64_t index=0;std::string after;bool done=false;
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
- (void)revoke {@synchronized(self){_revoked=true;[_items removeAllObjects];_view.reset();_store.reset();}}
- (FSItem *)rootItem {@synchronized(self){if(_revoked)return nil;return [self item:_view->root_inode() root:YES];}}
- (void)activateWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(FSItem *,NSError *))reply {
    @synchronized(self){try{if(_revoked){reply(nil,err(ENOTCONN));return;}reply([self item:_view->root_inode() root:YES],nil);}catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)deactivateWithOptions:(FSDeactivateOptions)options replyHandler:(void (^)(NSError *))reply {[self revoke];reply(nil);}
- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(NSError *))reply {@synchronized(self){reply(_revoked?err(ENOTCONN):nil);}}
- (void)unmountWithReplyHandler:(void (^)(void))reply {[self revoke];reply();}
- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError *))reply {@synchronized(self){reply(_revoked?err(ENOTCONN):nil);}}
- (void)getAttributes:(FSItemGetAttributesRequest *)desired ofItem:(FSItem *)item replyHandler:(void (^)(FSItemAttributes *,NSError *))reply {
    @synchronized(self){try{auto it=[self checked:item];reply([self attributes:_view->stat_inode(it.inode.UTF8String) item:it],nil);}catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)lookupItemNamed:(FSFileName *)name inDirectory:(FSItem *)directory replyHandler:(void (^)(FSItem *,FSFileName *,NSError *))reply {
    @synchronized(self){try{auto dir=[self checked:directory];auto n=_view->lookup_child(dir.inode.UTF8String,std::string((const char *)name.data.bytes,name.data.length));auto child=[self item:n root:NO];if(n.directory)child.parentInode=dir.inode;reply(child,name,nil);}catch(const std::system_error& e){reply(nil,nil,err(e.code().value()));}catch(const std::exception&){reply(nil,nil,err(EIO));}}
}
- (void)reclaimItem:(FSItem *)item replyHandler:(void (^)(NSError *))reply {reply(nil);} // Retain identity for the mount lifetime.
- (void)readSymbolicLink:(FSItem *)item replyHandler:(void (^)(FSFileName *,NSError *))reply {
    @synchronized(self){try{auto it=[self checked:item];auto data=_view->readlink_inode(it.inode.UTF8String);reply([FSFileName nameWithBytes:data.data() length:data.size()],nil);}catch(const std::system_error& e){reply(nil,err(e.code().value()));}catch(const std::exception&){reply(nil,err(EIO));}}
}
- (void)readFromFile:(FSItem *)item offset:(off_t)offset length:(size_t)length intoBuffer:(FSMutableFileDataBuffer *)buffer replyHandler:(void (^)(size_t,NSError *))reply {
    if(offset<0){reply(0,err(EINVAL));return;}
    @synchronized(self){try{auto it=[self checked:item];auto data=_view->read_inode(it.inode.UTF8String,offset,MIN(length,buffer.length));if(!data.empty())memcpy(buffer.mutableBytes,data.data(),data.size());reply(data.size(),nil);}catch(const std::system_error& e){reply(0,err(e.code().value()));}catch(const std::exception&){reply(0,err(EIO));}}
}
- (void)writeContents:(NSData *)contents toFile:(FSItem *)item atOffset:(off_t)offset replyHandler:(void (^)(size_t,NSError *))reply {reply(0,err(EROFS));}
- (void)setAttributes:(FSItemSetAttributesRequest *)req onItem:(FSItem *)item
         replyHandler:(void (^)(FSItemAttributes *_Nullable, NSError *_Nullable))reply {reply(nil,err(EROFS));}
- (void)createItemNamed:(FSFileName *)name type:(FSItemType)type inDirectory:(FSItem *)directory
             attributes:(FSItemSetAttributesRequest *)req
           replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {reply(nil,nil,err(EROFS));}
- (void)createSymbolicLinkNamed:(FSFileName *)name inDirectory:(FSItem *)directory
                     attributes:(FSItemSetAttributesRequest *)req linkContents:(FSFileName *)contents
                   replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {reply(nil,nil,err(EROFS));}
- (void)createLinkToItem:(FSItem *)item named:(FSFileName *)name inDirectory:(FSItem *)directory
            replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply {reply(nil,err(EROFS));}
- (void)removeItem:(FSItem *)item named:(FSFileName *)name fromDirectory:(FSItem *)directory
      replyHandler:(void (^)(NSError *_Nullable))reply {reply(err(EROFS));}
- (void)renameItem:(FSItem *)item inDirectory:(FSItem *)sourceDirectory named:(FSFileName *)sourceName
         toNewName:(FSFileName *)destinationName inDirectory:(FSItem *)destinationDirectory
          overItem:(FSItem *_Nullable)overItem replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply {reply(nil,err(EROFS));}
@end
