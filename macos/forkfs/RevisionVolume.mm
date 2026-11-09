#import "RevisionVolume.h"
#import "RevisionConfig.h"
#import <CommonCrypto/CommonDigest.h>
#include "rpc.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <stdlib.h>

namespace {
void require(bool ok,int code,const char* message){if(!ok)throw forkfs::RpcError(code,message);}
std::string bytes(NSString* value){NSData* data=[value dataUsingEncoding:NSUTF8StringEncoding];require(data!=nil,EINVAL,"invalid UTF-8 argument");return std::string(data.length?static_cast<const char*>(data.bytes):"",data.length);}
NSDictionary* object(const std::string& value){
    NSData* data=[NSData dataWithBytes:value.data() length:value.size()];
    id result=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    require([result isKindOfClass:NSDictionary.class],EPROTO,"invalid daemon JSON");return result;
}
NSError* posix(int code){return fs_errorForPOSIXError(code);}
FSItemType itemType(NSDictionary* a){return [a[@"directory"] boolValue]?FSItemTypeDirectory:[a[@"symlink"] boolValue]?FSItemTypeSymlink:FSItemTypeFile;}
BOOL emitsDotEntriesWithoutAttributes(void) {
    char release[64]={0};size_t length=sizeof release-1;long major=27;
    if(sysctlbyname("kern.osrelease",release,&length,nullptr,0)==0)major=strtol(release,nullptr,10);
    return major>=27;
}
}
@interface RevisionItem : FSItem
@property NSString* inode;
@property uint64_t number;
@property uint64_t parent;
@property NSDictionary* attrs;
@property NSString* reader;
@property NSString* writer;
@end
@implementation RevisionItem
@end

@implementation RevisionVolume {
    NSRecursiveLock* _lock;
    NSMutableDictionary<NSString*,RevisionItem*>* _items;
    NSMutableDictionary<NSString*,NSNumber*>* _numbers;
    NSMutableDictionary<NSNumber*,NSDictionary*>* _streams;
    uint64_t _nextItem,_nextStream;
    std::string _socket;
    forkfs::LoopbackEndpoint _loopback;
    BOOL _bridged;
    NSString* _session;
    NSString* _view;
    BOOL _revision,_readOnly,_alive,_scoped,_initialized;
    NSURL* _runtime;
    uid_t _uid;gid_t _gid;
    RevisionItem* _root;
    dispatch_source_t _heartbeat;
}
- (std::string)call:(NSArray<NSString*>*)args {
    require(_alive,ESTALE,"mount session is closed");std::vector<std::string> fields;
    for(NSString* arg in args)fields.push_back(bytes(arg));
    fields.push_back(_revision?"--revision":"--world");fields.push_back(bytes(_view));
    fields.push_back("--session");fields.push_back(bytes(_session));
    return forkfs::request_rpc(_socket,fields,_bridged?&_loopback:nullptr);
}
- (NSDictionary*)json:(NSArray<NSString*>*)args {return object([self call:args]);}
- (NSError*)perform:(void(^)(void))operation {
    [_lock lock];NSError* error=nil;
    @try {try{require(!_initialized || _alive,ESTALE,"mount session is closed");operation();}catch(const forkfs::RpcError& e){error=posix(e.code);}
          catch(const std::exception& e){error=posix(forkfs::rpc_posix_error(e.what()));}}
    @catch(NSException* e){error=posix(EIO);}
    @finally{[_lock unlock];}
    return error;
}
- (uint64_t)numberForInode:(NSString*)inode {
    require([inode isKindOfClass:NSString.class] && inode.length==32,EPROTO,"invalid inode attributes");
    NSNumber* number=_numbers[inode];if(!number){number=@(_nextItem++);_numbers[inode]=number;}return number.unsignedLongLongValue;
}
- (RevisionItem*)remember:(NSDictionary*)attrs parent:(uint64_t)parent {
    NSString* inode=attrs[@"inode_id"];require([inode isKindOfClass:NSString.class] && inode.length==32,EPROTO,"invalid inode attributes");
    RevisionItem* item=_items[inode];if(!item){item=[RevisionItem new];item.inode=inode;item.number=[self numberForInode:inode];_items[inode]=item;}
    item.attrs=attrs;item.parent=parent;return item;
}
- (FSItemAttributes*)attributes:(RevisionItem*)item {
    NSDictionary* a=item.attrs;FSItemAttributes* r=[FSItemAttributes new];r.type=itemType(a);
    r.fileID=(FSItemID)item.number;r.parentID=(FSItemID)item.parent;r.mode=item==_root?0700:[a[@"mode"] unsignedIntValue];r.linkCount=[a[@"links"] unsignedLongLongValue];
    r.uid=_uid;r.gid=_gid;r.size=[a[@"size"] unsignedLongLongValue];r.allocSize=r.size;
    uint64_t m=[a[@"modified_ns"] unsignedLongLongValue],c=[a[@"changed_ns"] unsignedLongLongValue];
    r.modifyTime=(timespec){(time_t)(m/1000000000),(long)(m%1000000000)};
    r.changeTime=(timespec){(time_t)(c/1000000000),(long)(c%1000000000)};
    r.supportsLimitedXAttrs=YES;r.inhibitKernelOffloadedIO=YES;return r;
}
- (RevisionItem*)item:(FSItem*)item {
    require([item isKindOfClass:RevisionItem.class],EINVAL,"invalid mount item");
    RevisionItem* result=(RevisionItem*)item;require(_items[result.inode]==result,ESTALE,"item belongs to another volume");return result;
}
- (NSString*)handle:(RevisionItem*)item {return item.writer?:item.reader;}
- (void)refresh:(RevisionItem*)item {
    if([item.attrs[@"links"] unsignedLongLongValue]==0 && ![self handle:item])return;
    NSString* handle=[self handle:item];item.attrs=[self json:handle?@[@"fs-hstat",handle]:@[@"inode-stat",item.inode]];
}
- (void)writable {require(!_readOnly,EROFS,"revision mount is read-only");}
- (instancetype)initWithRuntimeURL:(NSURL*)url error:(NSError**)error {return [self initWithRuntimeURL:url readOnly:NO error:error];}
- (instancetype)initWithRuntimeURL:(NSURL*)url readOnly:(BOOL)readOnly error:(NSError**)error {
    BOOL scoped=[url startAccessingSecurityScopedResource];
    NSDictionary* loaded=RevisionLoadDescriptor(url,error);if(!loaded){if(scoped)[url stopAccessingSecurityScopedResource];return nil;}
    if(!(self=[super initWithVolumeID:[[FSVolumeIdentifier alloc] initWithUUID:RevisionDescriptorUUID(loaded)] volumeName:[FSFileName nameWithString:@"ForkFS"]])){if(scoped)[url stopAccessingSecurityScopedResource];return nil;}
    _lock=[NSRecursiveLock new];_items=[NSMutableDictionary new];_numbers=[NSMutableDictionary new];_streams=[NSMutableDictionary new];_nextItem=3;_nextStream=1;
    _runtime=url;_scoped=scoped;
    NSError* failure=[self perform:^{
        NSDictionary* config=loaded;
        BOOL revision=config[@"revision"]!=nil;id view=config[revision?@"revision":@"world"];
        require([view isKindOfClass:NSString.class] && (config[@"world"]==nil || config[@"revision"]==nil),EINVAL,"select exactly one view");
        _view=view;_revision=revision;_readOnly=readOnly||revision;_socket=bytes([url.path stringByAppendingPathComponent:@"control.sock"]);
        if(config[@"rpc"]){_bridged=YES;_loopback.port=[config[@"rpc"][@"port"] unsignedShortValue];_loopback.key=bytes(config[@"rpc"][@"key"]);}
        auto session=object(forkfs::request_rpc(_socket,{"session-open","60000"},_bridged?&_loopback:nullptr));_session=session[@"session"];
        require([_session isKindOfClass:NSString.class],EPROTO,"invalid session response");_alive=YES;
        NSDictionary* root=[self json:@[@"inode-root"]];require([root[@"repository"] isEqual:config[@"repository"]],ESTALE,"repository identity changed");
        _uid=[root[@"uid"] unsignedIntValue];_gid=[root[@"gid"] unsignedIntValue];
        _root=[self remember:root[@"inode"] parent:FSItemIDParentOfRoot];_root.number=FSItemIDRootDirectory;_numbers[_root.inode]=@(FSItemIDRootDirectory);
    }];
    if(failure){[self shutdown];if(error)*error=failure;return nil;}
    _initialized=YES;
    __weak RevisionVolume* weak=self;
    _heartbeat=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_global_queue(QOS_CLASS_UTILITY,0));
    dispatch_source_set_timer(_heartbeat,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC),15*NSEC_PER_SEC,NSEC_PER_SEC);
    dispatch_source_set_event_handler(_heartbeat,^{RevisionVolume* volume=weak;if(!volume)return;
        NSError* failure=[volume perform:^{require(volume->_alive,ESTALE,"closed mount");forkfs::request_rpc(volume->_socket,{"session-keepalive",bytes(volume->_session)},volume->_bridged?&volume->_loopback:nullptr);}];
        if(failure){[volume->_lock lock];volume->_alive=NO;[volume->_lock unlock];}
    });dispatch_resume(_heartbeat);return self;
}
- (void)shutdown {
    [_lock lock];if(_heartbeat){dispatch_source_cancel(_heartbeat);_heartbeat=nil;}
    if(_session){try{forkfs::request_rpc(_socket,{"session-close",bytes(_session)},_bridged?&_loopback:nullptr);}catch(...){} _session=nil;}
    _alive=NO;[_streams removeAllObjects];[_items removeAllObjects];[_lock unlock];
}
- (void)dealloc {[self shutdown];if(_scoped)[_runtime stopAccessingSecurityScopedResource];}
- (NSInteger)maximumLinkCount{return 32767;}
- (NSInteger)maximumNameLength{return 255;}
- (BOOL)restrictsOwnershipChanges{return YES;}
- (BOOL)truncatesLongNames{return NO;}
- (NSInteger)maximumXattrSize{return 0;}
- (uint64_t)maximumFileSize{return 64*1024*1024;}
#ifdef FSKIT_API_AVAILABILITY_V2_4
- (FSMountOptions)requestedMountOptions {return _readOnly?FSMountOptionsReadOnly:0;}
#endif
- (FSVolumeSupportedCapabilities*)supportedVolumeCapabilities {
    FSVolumeSupportedCapabilities* c=[FSVolumeSupportedCapabilities new];c.supportsHardLinks=YES;c.supportsSymbolicLinks=YES;
    c.supportsPersistentObjectIDs=NO;c.supports64BitObjectIDs=YES;c.supportsSparseFiles=YES;c.supports2TBFiles=NO;
    c.caseFormat=FSVolumeCaseFormatSensitive;return c;
}
- (FSStatFSResult*)volumeStatistics {
    FSStatFSResult* r=[[FSStatFSResult alloc] initWithFileSystemTypeName:@"forkfs"];
    (void)[self perform:^{NSDictionary* space=[self json:@[@"inode-space"]];uint64_t block=[space[@"block_size"] unsignedLongLongValue];
        r.blockSize=block;r.ioSize=256*1024;r.totalBlocks=[space[@"blocks"] unsignedLongLongValue];
        r.freeBlocks=[space[@"free_blocks"] unsignedLongLongValue];r.availableBlocks=[space[@"available_blocks"] unsignedLongLongValue];
        r.usedBlocks=r.totalBlocks-r.freeBlocks;r.totalBytes=r.totalBlocks*block;r.freeBytes=r.freeBlocks*block;
        r.availableBytes=r.availableBlocks*block;r.usedBytes=r.usedBlocks*block;r.totalFiles=[space[@"files"] unsignedLongLongValue];r.freeFiles=[space[@"free_files"] unsignedLongLongValue];
    }];return r;
}
- (void)activateWithOptions:(FSTaskOptions*)options replyHandler:(void(^)(FSItem*,NSError*))reply {
    NSError* e=[self perform:^{[self refresh:_root];}];reply(e?nil:_root,e);
}
- (void)deactivateWithOptions:(FSDeactivateOptions)options replyHandler:(void(^)(NSError*))reply {[self shutdown];reply(nil);}
- (void)mountWithOptions:(FSTaskOptions*)options replyHandler:(void(^)(NSError*))reply {
    [_lock lock];if([options.taskOptions containsObject:@"ro"] || [options.taskOptions containsObject:@"rdonly"])_readOnly=YES;[_lock unlock];reply(nil);
}
- (void)unmountWithReplyHandler:(void(^)(void))reply {[self shutdown];reply();}
- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void(^)(NSError*))reply {reply([self perform:^{[self json:@[@"inode-sync"]];}]);}
- (void)getAttributes:(FSItemGetAttributesRequest*)desired ofItem:(FSItem*)item replyHandler:(void(^)(FSItemAttributes*,NSError*))reply {
    __block FSItemAttributes* a=nil;NSError* e=[self perform:^{RevisionItem* it=[self item:item];[self refresh:it];a=[self attributes:it];}];reply(a,e);
}
- (void)setAttributes:(FSItemSetAttributesRequest*)request onItem:(FSItem*)item replyHandler:(void(^)(FSItemAttributes*,NSError*))reply {
    __block FSItemAttributes* a=nil;NSError* e=[self perform:^{[self writable];RevisionItem* it=[self item:item];
        require(it!=_root || ![request isValid:FSItemAttributeMode] || request.mode==0700,EPERM,"mount root must remain private");
        NSString* mode=[request isValid:FSItemAttributeMode]?[@(request.mode) stringValue]:@"-";
        NSString* size=[request isValid:FSItemAttributeSize]?[@(request.size) stringValue]:@"-";
        NSString* mtime=@"-";if([request isValid:FSItemAttributeModifyTime]){auto t=request.modifyTime;
            require(t.tv_sec>=0 && t.tv_nsec>=0 && t.tv_nsec<1000000000 && uint64_t(t.tv_sec)<=uint64_t(INT64_MAX)/1000000000,EINVAL,"invalid modification time");
            mtime=[@(uint64_t(t.tv_sec)*1000000000+t.tv_nsec) stringValue];}
        // Only these attributes are persisted by the current inode schema.
        require((![request isValid:FSItemAttributeUID] || request.uid==_uid) && (![request isValid:FSItemAttributeGID] || request.gid==_gid),EPERM,"ownership changes are unsupported");
        require(![request isValid:FSItemAttributeType] && ![request isValid:FSItemAttributeLinkCount] && ![request isValid:FSItemAttributeAllocSize] && ![request isValid:FSItemAttributeFileID] && ![request isValid:FSItemAttributeParentID] && ![request isValid:FSItemAttributeChangeTime] && ![request isValid:FSItemAttributeBackupTime] && ![request isValid:FSItemAttributeAddedTime],ENOTSUP,"unsupported inode attributes");
        require(![request isValid:FSItemAttributeFlags] && ![request isValid:FSItemAttributeAccessTime] && ![request isValid:FSItemAttributeBirthTime],ENOTSUP,"unsupported inode attributes");
        it.attrs=[self json:@[@"inode-setattr",it.inode,mode,size,mtime,([request isValid:FSItemAttributeSize]?(it.writer?:@"-"):([self handle:it]?:@"-"))]];
        request.consumedAttributes=([request isValid:FSItemAttributeUID]?FSItemAttributeUID:0)|([request isValid:FSItemAttributeGID]?FSItemAttributeGID:0)|([request isValid:FSItemAttributeMode]?FSItemAttributeMode:0)|([request isValid:FSItemAttributeSize]?FSItemAttributeSize:0)|([request isValid:FSItemAttributeModifyTime]?FSItemAttributeModifyTime:0);
        a=[self attributes:it];}];reply(a,e);
}
- (void)lookupItemNamed:(FSFileName*)name inDirectory:(FSItem*)directory replyHandler:(void(^)(FSItem*,FSFileName*,NSError*))reply {
    __block RevisionItem* it=nil;NSError* e=[self perform:^{auto dir=[self item:directory];it=[self remember:[self json:@[@"inode-lookup",dir.inode,name.string]] parent:dir.number];}];reply(it,e?nil:name,e);
}
- (void)reclaimItem:(FSItem*)item replyHandler:(void(^)(NSError*))reply {
    NSError* e=[self perform:^{auto it=[self item:item];require(!it.reader && !it.writer,EBUSY,"reclaim of open inode");
        if(it!=_root)[_items removeObjectForKey:it.inode]; // numbers remain stable for the mount lifetime
    }];reply(e);
}
- (void)readSymbolicLink:(FSItem*)item replyHandler:(void(^)(FSFileName*,NSError*))reply {
    __block FSFileName* name=nil;NSError* e=[self perform:^{auto value=[self call:@[@"inode-readlink",[self item:item].inode]];
        name=[FSFileName nameWithBytes:value.data() length:value.size()];}];reply(name,e);
}
- (void)createItemNamed:(FSFileName*)name type:(FSItemType)type inDirectory:(FSItem*)directory attributes:(FSItemSetAttributesRequest*)request replyHandler:(void(^)(FSItem*,FSFileName*,NSError*))reply {
    __block RevisionItem* result=nil;NSError* e=[self perform:^{[self writable];auto dir=[self item:directory];
        auto mode=[request isValid:FSItemAttributeMode]?request.mode:(type==FSItemTypeDirectory?0755:0644);
        require(mode<=0777,ENOTSUP,"unsupported creation mode");NSDictionary* attrs;
        if(type==FSItemTypeDirectory)attrs=[self json:@[@"inode-mkdir",dir.inode,name.string,[@(mode) stringValue]]];
        else {require(type==FSItemTypeFile,ENOTSUP,"unsupported creation type");
            NSString* handle=[self json:@[@"inode-create",dir.inode,name.string,[@(mode) stringValue]]][@"handle"];
            @try{attrs=[self json:@[@"fs-hstat",handle]];}@finally{[self json:@[@"fs-close",handle]];}}
        result=[self remember:attrs parent:dir.number];request.consumedAttributes=[request isValid:FSItemAttributeMode]?FSItemAttributeMode:0;
    }];reply(result,e?nil:name,e);
}
- (void)createSymbolicLinkNamed:(FSFileName*)name inDirectory:(FSItem*)directory attributes:(FSItemSetAttributesRequest*)request linkContents:(FSFileName*)contents replyHandler:(void(^)(FSItem*,FSFileName*,NSError*))reply {
    __block RevisionItem* result=nil;NSError* e=[self perform:^{[self writable];auto dir=[self item:directory];
        result=[self remember:[self json:@[@"inode-symlink",dir.inode,name.string,contents.string]] parent:dir.number];request.consumedAttributes=0;}];reply(result,e?nil:name,e);
}
- (void)createLinkToItem:(FSItem*)item named:(FSFileName*)name inDirectory:(FSItem*)directory replyHandler:(void(^)(FSFileName*,NSError*))reply {
    NSError* e=[self perform:^{[self writable];auto it=[self item:item],dir=[self item:directory];it.attrs=[self json:@[@"inode-link",it.inode,dir.inode,name.string]];}];reply(e?nil:name,e);
}
- (void)removeItem:(FSItem*)item named:(FSFileName*)name fromDirectory:(FSItem*)directory replyHandler:(void(^)(NSError*))reply {
    NSError* e=[self perform:^{[self writable];auto it=[self item:item],dir=[self item:directory];
        [self json:@[@"inode-remove",dir.inode,name.string,it.inode,itemType(it.attrs)==FSItemTypeDirectory?@"directory":@"file"]];
        if([self handle:it])[self refresh:it];else {NSMutableDictionary* a=[it.attrs mutableCopy];auto links=[a[@"links"] unsignedLongLongValue];a[@"links"]=@(itemType(a)==FSItemTypeDirectory?0:links-1);it.attrs=a;}
    }];reply(e);
}
- (void)renameItem:(FSItem*)item inDirectory:(FSItem*)source named:(FSFileName*)name toNewName:(FSFileName*)newName inDirectory:(FSItem*)destination overItem:(FSItem*)over replyHandler:(void(^)(FSFileName*,NSError*))reply {
    NSError* e=[self perform:^{[self writable];auto it=[self item:item],src=[self item:source],dst=[self item:destination];
        it.attrs=[self json:@[@"inode-rename",src.inode,name.string,dst.inode,newName.string,it.inode,over?[self item:over].inode:@"-"]];it.parent=dst.number;
        if(over && over!=item){auto replaced=[self item:over];if([self handle:replaced])[self refresh:replaced];
            else{NSMutableDictionary* a=[replaced.attrs mutableCopy];auto links=[a[@"links"] unsignedLongLongValue];a[@"links"]=@(itemType(a)==FSItemTypeDirectory?0:links-1);replaced.attrs=a;}}
    }];reply(e?nil:newName,e);
}
- (void)enumerateDirectory:(FSItem*)directory startingAtCookie:(FSDirectoryCookie)cookie verifier:(FSDirectoryVerifier)verifier providingAttributes:(FSItemGetAttributesRequest*)attributes usingPacker:(FSDirectoryEntryPacker*)packer replyHandler:(void(^)(FSDirectoryVerifier,NSError*))reply {
    __block FSDirectoryVerifier out=verifier;NSError* e=[self perform:^{auto dir=[self item:directory];NSArray* entries;
        BOOL withDots=attributes==nil && emitsDotEntriesWithoutAttributes();
        if(cookie==FSDirectoryCookieInitial){require(_streams.count<64,EMFILE,"directory stream limit");
            NSString* cursor=[self json:@[@"inode-dir-open",dir.inode]][@"cursor"];NSMutableArray* all=[NSMutableArray new];
            @try {for(;;){NSDictionary* page=[self json:@[@"fs-dir-entries",cursor,@"512"]];NSArray* values=page[@"entries"];
                require([values isKindOfClass:NSArray.class] && all.count+values.count+(withDots?2:0)<=65536,EOVERFLOW,"directory enumeration limit");[all addObjectsFromArray:values];if([page[@"eof"] boolValue])break;}}
            @finally{[self json:@[@"fs-dir-close",cursor]];}
            if(withDots){[all insertObject:@{@"dotID":@(dir.parent),@"name":@".."} atIndex:0];[all insertObject:@{@"dotID":@(dir.number),@"name":@"."} atIndex:0];}
            out=_nextStream++;_streams[@(out)]=@{@"directory":dir.inode,@"entries":all,@"dots":@(withDots),@"attributes":@(attributes!=nil)};entries=all;
        } else {NSDictionary* stream=_streams[@(verifier)];require([stream[@"directory"] isEqual:dir.inode],ESTALE,"directory cookie belongs to another inode");
            require([stream[@"dots"] boolValue]==withDots && [stream[@"attributes"] boolValue]==(attributes!=nil),ESTALE,"directory cookie mode changed");entries=stream[@"entries"];}
        require(entries!=nil && cookie<=entries.count,ESTALE,"invalid directory cookie");
        for(NSUInteger index=cookie;index<entries.count;++index){NSDictionary* entry=entries[index];BOOL dot=entry[@"dotID"]!=nil;
            RevisionItem* it=nil;FSItemType type=FSItemTypeDirectory;FSItemID itemID;
            if(dot)itemID=(FSItemID)[entry[@"dotID"] unsignedLongLongValue];
            else {NSDictionary* snapshot=entry[@"inode"];type=itemType(snapshot);itemID=(FSItemID)[self numberForInode:snapshot[@"inode_id"]];
                if(attributes){it=[RevisionItem new];it.inode=snapshot[@"inode_id"];it.number=itemID;it.parent=dir.number;it.attrs=snapshot;}}
            if(![packer packEntryWithName:[FSFileName nameWithString:entry[@"name"]] itemType:type itemID:itemID nextCookie:index+1 attributes:(attributes && it)?[self attributes:it]:nil])break;
        }
        // Bounded snapshots remain available for resumed cookies. Evict only when
        // starting a later stream; old cookies then fail explicitly with ESTALE.
        if(_streams.count>=64){NSNumber* oldest=[[_streams.allKeys sortedArrayUsingSelector:@selector(compare:)] firstObject];if(oldest.unsignedLongLongValue!=out)[_streams removeObjectForKey:oldest];}
    }];reply(out,e);
}
- (BOOL)isOpenCloseInhibited{return NO;}
- (void)openItem:(FSItem*)item withModes:(FSVolumeOpenModes)modes replyHandler:(void(^)(NSError*))reply {
    NSError* e=[self perform:^{auto it=[self item:item];if(itemType(it.attrs)==FSItemTypeDirectory){require(!(modes&FSVolumeOpenModesWrite),EISDIR,"directory is not writable as a file");return;}require(itemType(it.attrs)==FSItemTypeFile,ENOTSUP,"only regular file opens are supported");
        if((modes&FSVolumeOpenModesWrite) && !it.writer){[self writable];it.writer=[self json:@[@"inode-open",it.inode,@"rw"]][@"handle"];}
        if((modes&FSVolumeOpenModesRead) && !it.reader && !it.writer)it.reader=[self json:@[@"inode-open",it.inode,@"r"]][@"handle"];
    }];reply(e);
}
- (void)closeItem:(FSItem*)item keepingModes:(FSVolumeOpenModes)modes replyHandler:(void(^)(NSError*))reply {
    NSError* e=[self perform:^{auto it=[self item:item];if([self handle:it])[self refresh:it];
        if(!(modes&FSVolumeOpenModesWrite) && it.writer){
            if((modes&FSVolumeOpenModesRead) && !it.reader) { // preserve an unlinked inode through a read-capable dup
                it.reader=[self json:@[@"fs-dup",it.writer]][@"handle"];}
            [self json:@[@"fs-close",it.writer]];it.writer=nil;
        }
        if(!(modes&FSVolumeOpenModesRead) && it.reader){[self json:@[@"fs-close",it.reader]];it.reader=nil;}
    }];reply(e);
}
- (void)readFromFile:(FSItem*)item offset:(off_t)offset length:(size_t)length intoBuffer:(FSMutableFileDataBuffer*)buffer replyHandler:(void(^)(size_t,NSError*))reply {
    __block size_t count=0;NSError* e=[self perform:^{require(offset>=0,EINVAL,"negative read offset");auto it=[self item:item];NSString* handle=[self handle:it];require(handle!=nil,EBADF,"file is not open");
        const auto wanted=MIN(length,buffer.length);
        while(count<wanted){auto data=[self call:@[@"fs-hread",handle,[@(uint64_t(offset)+count) stringValue],[@(MIN(wanted-count,size_t(256*1024))) stringValue]]];
            require(data.size()<=MIN(wanted-count,size_t(256*1024)),EPROTO,"oversized daemon read");if(data.empty())break;
            memcpy(static_cast<unsigned char*>(buffer.mutableBytes)+count,data.data(),data.size());count+=data.size();}
    }];reply(count,e);
}
- (void)writeContents:(NSData*)contents toFile:(FSItem*)item atOffset:(off_t)offset replyHandler:(void(^)(size_t,NSError*))reply {
    __block size_t count=0;NSError* e=[self perform:^{[self writable];require(offset>=0,EINVAL,"negative write offset");auto it=[self item:item];require(it.writer!=nil,EBADF,"file is not open for writing");
        // FSKit can exceed ioSize in a single callback. Complete it using
        // bounded RPCs; ioSize is a preference rather than a request limit.
        require(contents.length==0 || (uint64_t(offset)<=64*1024*1024 && contents.length<=64*1024*1024-uint64_t(offset)),EFBIG,"file exceeds 64 MiB");
        while(count<contents.length){auto length=MIN(contents.length-count,size_t(256*1024));
            std::vector<std::string> fields{"fs-hwrite",bytes(it.writer),std::to_string(uint64_t(offset)+count),std::string(static_cast<const char*>(contents.bytes)+count,length),_revision?"--revision":"--world",bytes(_view),"--session",bytes(_session)};
            forkfs::request_rpc(_socket,fields,_bridged?&_loopback:nullptr);count+=length;}
    }];reply(count,e);
}
- (void)getXattrNamed:(FSFileName*)name ofItem:(FSItem*)item replyHandler:(void(^)(NSData*,NSError*))reply {reply(nil,posix(ENOATTR));}
- (void)setXattrNamed:(FSFileName*)name toData:(NSData*)value onItem:(FSItem*)item policy:(FSSetXattrPolicy)policy replyHandler:(void(^)(NSError*))reply {reply(posix(_readOnly?EROFS:ENOTSUP));}
- (void)listXattrsOfItem:(FSItem*)item replyHandler:(void(^)(NSArray<FSFileName*>*,NSError*))reply {reply(@[],nil);}
@end
