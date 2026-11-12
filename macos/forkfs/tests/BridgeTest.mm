#import "RevisionVolume.h"
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>

static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static NSData* command(NSString* binary,NSArray<NSString*>* args){
    NSTask* task=[NSTask new];task.executableURL=[NSURL fileURLWithPath:binary];task.arguments=args;
    NSPipe* out=[NSPipe pipe];NSPipe* err=[NSPipe pipe];task.standardOutput=out;task.standardError=err;
    NSError* e=nil;check([task launchAndReturnError:&e],"cannot launch daemon command");
    NSData* data=[out.fileHandleForReading readDataToEndOfFile];NSData* errors=[err.fileHandleForReading readDataToEndOfFile];[task waitUntilExit];
    if(task.terminationStatus)std::cerr<<[[[NSString alloc] initWithData:errors encoding:NSUTF8StringEncoding] UTF8String];
    check(task.terminationStatus==0,"daemon command failed");return data;
}
@interface TestBuffer:NSObject
@property NSMutableData* storage;
@end
@implementation TestBuffer
- (NSUInteger)length{return self.storage.length;}
- (void*)mutableBytes{return self.storage.mutableBytes;}
@end
@interface RecordingPacker:NSObject
@property NSUInteger limit;
@property NSUInteger packedThisCall;
@property NSMutableArray<NSDictionary*>* entries;
@end
@implementation RecordingPacker
- (instancetype)init {if((self=[super init]))_entries=[NSMutableArray new];return self;}
- (BOOL)packEntryWithName:(FSFileName*)name itemType:(FSItemType)type itemID:(FSItemID)itemID nextCookie:(FSDirectoryCookie)cookie attributes:(FSItemAttributes*)attributes {
    if(_packedThisCall>=_limit)return NO;
    ++_packedThisCall;[_entries addObject:@{@"name":name.string,@"type":@(type),@"id":@(itemID),@"cookie":@(cookie),@"hasAttributes":@(attributes!=nil),@"parentID":attributes?@(attributes.parentID):NSNull.null,@"linkCount":attributes?@(attributes.linkCount):NSNull.null}];return YES;
}
@end
int main(int argc,char** argv){@autoreleasepool {
    if(argc!=2 && argc!=4)return 2;char tmp[]="/tmp/ff-fskit-XXXXXX";if(!mkdtemp(tmp))return 2;
    NSString* runtime=@(tmp);NSString* db=[runtime stringByAppendingPathComponent:@"db"];
    NSString* binary=@(argv[1]);NSTask* server=nil;NSTask* bridge=nil;__block RevisionVolume* volume=nil;
    try {
        command(binary,@[@"init",db]);command(binary,@[@"fs-init",db]);
        NSString* xattrInput=[runtime stringByAppendingPathComponent:@"xattr-input"];[[NSData dataWithBytes:"R\0X" length:3] writeToFile:xattrInput atomically:YES];
        command(binary,@[@"fs-xattr-set",db,@"/",@"user.retained",@"create",xattrInput]);command(binary,@[@"fs-snapshot",db,@"base"]);
        NSDictionary* status=[NSJSONSerialization JSONObjectWithData:command(binary,@[@"check",db]) options:0 error:nil];
        NSDictionary* config=@{@"schema_version":@1,@"world":@"main",@"repository":status[@"repository_id"]};
        NSString* configPath=[runtime stringByAppendingPathComponent:@"forkfs.json"];
        NSData* encoded=[NSJSONSerialization dataWithJSONObject:config options:0 error:nil];
        int configFD=open(configPath.fileSystemRepresentation,O_WRONLY|O_CREAT|O_EXCL,0600);check(configFD>=0,"cannot create mount descriptor");
        check(write(configFD,encoded.bytes,encoded.length)==static_cast<ssize_t>(encoded.length),"cannot write descriptor");close(configFD);
        NSString* socket=[runtime stringByAppendingPathComponent:@"control.sock"];
        server=[NSTask new];server.executableURL=[NSURL fileURLWithPath:binary];server.arguments=@[@"serve",db,@"--socket",socket,@"--rpc"];
        NSPipe* pipe=[NSPipe pipe];server.standardOutput=pipe;server.standardError=[NSPipe pipe];__block NSError* error=nil;
        check([server launchAndReturnError:&error],"cannot launch resident daemon");
        // Read the ready status line, emitted after listen().
        check([pipe.fileHandleForReading availableData].length>0,"daemon did not become ready");
        if(argc==4){
            bridge=[NSTask new];bridge.executableURL=[NSURL fileURLWithPath:@(argv[2])];bridge.arguments=@[@(argv[3]),binary,runtime,@"--world",@"main"];
            NSPipe* ready=[NSPipe pipe];bridge.standardOutput=ready;
            check([bridge launchAndReturnError:&error],"cannot launch loopback bridge");
            check([ready.fileHandleForReading availableData].length>0,"bridge did not become ready");
            config=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:configPath] options:0 error:nil];
            check(config[@"rpc"]!=nil,"bridge descriptor missing");
            NSMutableDictionary* wrong=[config mutableCopy];NSMutableDictionary* rpc=[config[@"rpc"] mutableCopy];
            NSString* key=rpc[@"key"];rpc[@"key"]=[( [key hasPrefix:@"0"]?@"1":@"0") stringByAppendingString:[key substringFromIndex:1]];wrong[@"rpc"]=rpc;
            NSData* bad=[NSJSONSerialization dataWithJSONObject:wrong options:0 error:nil];
            check([bad writeToFile:configPath options:0 error:nil],"cannot write authentication test descriptor");
            RevisionVolume* rejected=[[RevisionVolume alloc] initWithRuntimeURL:[NSURL fileURLWithPath:runtime isDirectory:YES] error:&error];
            check(!rejected && error,"wrong server key must fail closed");error=nil;
            NSData* good=[NSJSONSerialization dataWithJSONObject:config options:0 error:nil];
            check([good writeToFile:configPath options:0 error:nil],"cannot restore authenticated descriptor");
        }
        FSUnaryFileSystem<FSUnaryFileSystemOperations>* filesystem=[NSClassFromString(@"RevisionFileSystem") new];
        FSPathURLResource* resource=[[FSPathURLResource alloc] initWithURL:[NSURL fileURLWithPath:runtime isDirectory:YES] writable:NO];
        [filesystem loadResource:resource options:nil replyHandler:^(FSVolume* loaded,NSError* failure){volume=(RevisionVolume*)loaded;error=failure;}];
        if(error)std::cerr<<error.description.UTF8String<<"\n";check(volume!=nil && !error,"volume initialization failed");
        __block FSItem* root=nil;__block NSError* result=nil;
        [volume activateWithOptions:nil replyHandler:^(FSItem* item,NSError* e){root=item;result=e;}];check(root && !result,"activation failed");
        __block FSItemAttributes* rootAttrs=nil;
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:root replyHandler:^(FSItemAttributes* a,NSError* e){rootAttrs=a;result=e;}];check(!result && rootAttrs.mode==0700,"mount root must be private");
        FSItemSetAttributesRequest* expose=[FSItemSetAttributesRequest new];expose.mode=0755;
        [volume setAttributes:expose onItem:root replyHandler:^(FSItemAttributes* a,NSError* e){result=e;}];check(result.code==EPERM,"must not expose private mount root");
        __block FSItem* file=nil;FSItemSetAttributesRequest* create=[FSItemSetAttributesRequest new];create.mode=0600;
        [volume createItemNamed:[FSFileName nameWithString:@"文件📄"] type:FSItemTypeFile inDirectory:root attributes:create replyHandler:^(FSItem* item,FSFileName* n,NSError* e){file=item;result=e;}];check(file && !result,"create callback failed");
        __block FSItemAttributes* fileAttrs=nil;
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:file replyHandler:^(FSItemAttributes* a,NSError* e){fileAttrs=a;result=e;}];check(fileAttrs && !result,"file identity lookup failed");
        FSItemSetAttributesRequest* times=[FSItemSetAttributesRequest new];times.accessTime=(timespec){123,456};times.modifyTime=(timespec){234,567};times.birthTime=(timespec){100,789};
        [volume setAttributes:times onItem:file replyHandler:^(FSItemAttributes* a,NSError* e){fileAttrs=a;result=e;}];
        check(!result && fileAttrs.accessTime.tv_sec==123 && fileAttrs.accessTime.tv_nsec==456 && fileAttrs.modifyTime.tv_sec==234 && fileAttrs.modifyTime.tv_nsec==567 && fileAttrs.birthTime.tv_sec==100 && fileAttrs.birthTime.tv_nsec==789,"timestamp callback roundtrip failed");
        check((times.consumedAttributes&(FSItemAttributeAccessTime|FSItemAttributeModifyTime|FSItemAttributeBirthTime))==(FSItemAttributeAccessTime|FSItemAttributeModifyTime|FSItemAttributeBirthTime),"timestamp attributes were not consumed");
        FSItemSetAttributesRequest* invalidTimes=[FSItemSetAttributesRequest new];invalidTimes.mode=0777;invalidTimes.accessTime=(timespec){9223372036,854775808};
        [volume setAttributes:invalidTimes onItem:file replyHandler:^(FSItemAttributes* a,NSError* e){result=e;}];check(result.code==EINVAL,"overflowing time must fail before mutation");
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:file replyHandler:^(FSItemAttributes* a,NSError* e){fileAttrs=a;result=e;}];check(!result && fileAttrs.mode==0600 && fileAttrs.accessTime.tv_sec==123,"invalid timestamp partially changed attributes");
        BOOL expectDots=[NSProcessInfo processInfo].operatingSystemVersion.majorVersion>=27;
        NSArray<NSString*>* expectedNames=expectDots?@[@".",@"..",@"文件📄"]:@[@"文件📄"];
        RecordingPacker* directoryPacker=[RecordingPacker new];directoryPacker.limit=1;
        __block FSDirectoryVerifier directoryVerifier=FSDirectoryVerifierInitial;
        FSDirectoryCookie next=FSDirectoryCookieInitial;
        for(NSUInteger page=0;page<expectedNames.count;++page) {
            __block NSError* enumerationError=nil;
            directoryPacker.packedThisCall=0;
            [volume enumerateDirectory:root startingAtCookie:next verifier:directoryVerifier providingAttributes:nil usingPacker:(FSDirectoryEntryPacker*)directoryPacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){directoryVerifier=verifier;enumerationError=e;}];
            check(!enumerationError,"attribute-less directory enumeration failed");
            check(directoryPacker.packedThisCall==1,"directory packer made no progress");
            next=[directoryPacker.entries.lastObject[@"cookie"] unsignedLongLongValue];
        }
        check(directoryPacker.entries.count==expectedNames.count,"directory enumeration returned an unexpected entry count");
        for(NSUInteger i=0;i<expectedNames.count;++i){NSDictionary* entry=directoryPacker.entries[i];check([entry[@"name"] isEqual:expectedNames[i]],"directory entries or resume cookies are out of order");check(![entry[@"hasAttributes"] boolValue],"attribute-less enumeration unexpectedly supplied attributes");
            if(expectDots && i==0)check([entry[@"id"] unsignedLongLongValue]==rootAttrs.fileID,"dot entry did not use the directory item ID");
            if(expectDots && i==1)check([entry[@"id"] unsignedLongLongValue]==FSItemIDParentOfRoot,"root parent dot entry did not use parent-of-root ID");
            if([entry[@"name"] isEqual:@"文件📄"])check([entry[@"id"] unsignedLongLongValue]==fileAttrs.fileID,"normal directory item ID changed");
        }
        directoryPacker.packedThisCall=0;__block NSError* eofError=nil;
        [volume enumerateDirectory:root startingAtCookie:next verifier:directoryVerifier providingAttributes:nil usingPacker:(FSDirectoryEntryPacker*)directoryPacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){eofError=e;}];
        check(!eofError && directoryPacker.packedThisCall==0 && directoryPacker.entries.count==expectedNames.count,"end-of-directory resume duplicated entries");
        RecordingPacker* attributePacker=[RecordingPacker new];attributePacker.limit=8;
        __block FSDirectoryVerifier attributeVerifier=FSDirectoryVerifierInitial;__block NSError* enumerationError=nil;
        [volume enumerateDirectory:root startingAtCookie:FSDirectoryCookieInitial verifier:attributeVerifier providingAttributes:[FSItemGetAttributesRequest new] usingPacker:(FSDirectoryEntryPacker*)attributePacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){attributeVerifier=verifier;enumerationError=e;}];
        check(!enumerationError && attributePacker.entries.count==1 && [attributePacker.entries[0][@"name"] isEqual:@"文件📄"],"attribute directory enumeration must omit dot entries");
        check([attributePacker.entries[0][@"hasAttributes"] boolValue],"requested directory attributes were not supplied");
        __block FSItem* childDirectory=nil;
        [volume createItemNamed:[FSFileName nameWithString:@"child"] type:FSItemTypeDirectory inDirectory:root attributes:[FSItemSetAttributesRequest new] replyHandler:^(FSItem* item,FSFileName* n,NSError* e){childDirectory=item;result=e;}];check(childDirectory && !result,"test directory creation failed");
        RecordingPacker* stalePacker=[RecordingPacker new];stalePacker.limit=1;stalePacker.packedThisCall=0;
        __block FSDirectoryVerifier staleVerifier=FSDirectoryVerifierInitial;__block NSError* staleError=nil;
        [volume enumerateDirectory:root startingAtCookie:FSDirectoryCookieInitial verifier:staleVerifier providingAttributes:nil usingPacker:(FSDirectoryEntryPacker*)stalePacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){staleVerifier=verifier;staleError=e;}];check(!staleError,"initial resumable enumeration failed");
        FSDirectoryCookie staleCookie=[stalePacker.entries.lastObject[@"cookie"] unsignedLongLongValue];
        [volume enumerateDirectory:childDirectory startingAtCookie:staleCookie verifier:staleVerifier providingAttributes:nil usingPacker:(FSDirectoryEntryPacker*)[RecordingPacker new] replyHandler:^(FSDirectoryVerifier verifier,NSError* e){staleError=e;}];check(staleError.code==ESTALE,"directory cookie was accepted for another directory");
        staleError=nil;
        [volume enumerateDirectory:root startingAtCookie:staleCookie verifier:staleVerifier providingAttributes:[FSItemGetAttributesRequest new] usingPacker:(FSDirectoryEntryPacker*)[RecordingPacker new] replyHandler:^(FSDirectoryVerifier verifier,NSError* e){staleError=e;}];check(staleError.code==ESTALE,"directory cookie was accepted in another attribute mode");
        staleError=nil;
        [volume enumerateDirectory:root startingAtCookie:UINT64_MAX verifier:staleVerifier providingAttributes:nil usingPacker:(FSDirectoryEntryPacker*)[RecordingPacker new] replyHandler:^(FSDirectoryVerifier verifier,NSError* e){staleError=e;}];check(staleError.code==ESTALE,"invalid directory cookie was accepted");
        if(expectDots){
        __block FSItem* parentA=nil;__block FSItem* parentB=nil;__block FSItem* movingDirectory=nil;
        [volume createItemNamed:[FSFileName nameWithString:@"parent-a"] type:FSItemTypeDirectory inDirectory:root attributes:[FSItemSetAttributesRequest new] replyHandler:^(FSItem* item,FSFileName* n,NSError* e){parentA=item;result=e;}];check(parentA && !result,"first parent directory creation failed");
        [volume createItemNamed:[FSFileName nameWithString:@"parent-b"] type:FSItemTypeDirectory inDirectory:root attributes:[FSItemSetAttributesRequest new] replyHandler:^(FSItem* item,FSFileName* n,NSError* e){parentB=item;result=e;}];check(parentB && !result,"second parent directory creation failed");
        [volume createItemNamed:[FSFileName nameWithString:@"a-first"] type:FSItemTypeFile inDirectory:parentA attributes:[FSItemSetAttributesRequest new] replyHandler:^(FSItem* item,FSFileName* n,NSError* e){result=e;}];check(!result,"snapshot prefix entry creation failed");
        [volume createItemNamed:[FSFileName nameWithString:@"z-moving"] type:FSItemTypeDirectory inDirectory:parentA attributes:[FSItemSetAttributesRequest new] replyHandler:^(FSItem* item,FSFileName* n,NSError* e){movingDirectory=item;result=e;}];check(movingDirectory && !result,"nested directory creation failed");
        __block FSItemAttributes* parentAAttrs=nil;__block FSItemAttributes* parentBAttrs=nil;
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:parentA replyHandler:^(FSItemAttributes* a,NSError* e){parentAAttrs=a;result=e;}];check(parentAAttrs && !result,"parent identity lookup failed");
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:parentB replyHandler:^(FSItemAttributes* a,NSError* e){parentBAttrs=a;result=e;}];check(parentBAttrs && !result,"new parent identity lookup failed");
        RecordingPacker* parentSnapshotPacker=[RecordingPacker new];parentSnapshotPacker.limit=1;
        __block FSDirectoryVerifier parentSnapshotVerifier=FSDirectoryVerifierInitial;__block NSError* parentSnapshotError=nil;
        [volume enumerateDirectory:parentA startingAtCookie:FSDirectoryCookieInitial verifier:parentSnapshotVerifier providingAttributes:[FSItemGetAttributesRequest new] usingPacker:(FSDirectoryEntryPacker*)parentSnapshotPacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){parentSnapshotVerifier=verifier;parentSnapshotError=e;}];
        check(!parentSnapshotError && parentSnapshotPacker.entries.count==1 && [parentSnapshotPacker.entries[0][@"name"] isEqual:@"a-first"],"parent snapshot did not stop before moving child");
        FSDirectoryCookie parentSnapshotCookie=[parentSnapshotPacker.entries.lastObject[@"cookie"] unsignedLongLongValue];
        RecordingPacker* movingPacker=[RecordingPacker new];movingPacker.limit=1;
        __block FSDirectoryVerifier movingVerifier=FSDirectoryVerifierInitial;__block NSError* movingError=nil;
        [volume enumerateDirectory:movingDirectory startingAtCookie:FSDirectoryCookieInitial verifier:movingVerifier providingAttributes:nil usingPacker:(FSDirectoryEntryPacker*)movingPacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){movingVerifier=verifier;movingError=e;}];check(!movingError && movingPacker.entries.count==1 && [movingPacker.entries[0][@"name"] isEqual:@"."],"nested directory first page failed");
        FSDirectoryCookie movingCookie=[movingPacker.entries.lastObject[@"cookie"] unsignedLongLongValue];
        [volume renameItem:movingDirectory inDirectory:parentA named:[FSFileName nameWithString:@"z-moving"] toNewName:[FSFileName nameWithString:@"moved"] inDirectory:parentB overItem:nil replyHandler:^(FSFileName* n,NSError* e){movingError=e;}];check(!movingError,"directory move between pages failed");
        movingPacker.packedThisCall=0;
        [volume enumerateDirectory:movingDirectory startingAtCookie:movingCookie verifier:movingVerifier providingAttributes:nil usingPacker:(FSDirectoryEntryPacker*)movingPacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){movingError=e;}];
        check(!movingError && movingPacker.entries.count==2 && [movingPacker.entries[1][@"name"] isEqual:@".."] && [movingPacker.entries[1][@"id"] unsignedLongLongValue]==parentAAttrs.fileID,"resumed parent dot ID changed during rename");
        RecordingPacker* freshMovingPacker=[RecordingPacker new];freshMovingPacker.limit=1;
        __block FSDirectoryVerifier freshMovingVerifier=FSDirectoryVerifierInitial;__block NSError* freshMovingError=nil;
        [volume enumerateDirectory:movingDirectory startingAtCookie:FSDirectoryCookieInitial verifier:freshMovingVerifier providingAttributes:nil usingPacker:(FSDirectoryEntryPacker*)freshMovingPacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){freshMovingVerifier=verifier;freshMovingError=e;}];
        FSDirectoryCookie freshMovingCookie=[freshMovingPacker.entries.lastObject[@"cookie"] unsignedLongLongValue];freshMovingPacker.packedThisCall=0;
        [volume enumerateDirectory:movingDirectory startingAtCookie:freshMovingCookie verifier:freshMovingVerifier providingAttributes:nil usingPacker:(FSDirectoryEntryPacker*)freshMovingPacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){freshMovingError=e;}];
        check(!freshMovingError && freshMovingPacker.entries.count==2 && [freshMovingPacker.entries[1][@"id"] unsignedLongLongValue]==parentBAttrs.fileID,"new parent dot ID did not follow directory rename");
        parentSnapshotPacker.packedThisCall=0;
        [volume enumerateDirectory:parentA startingAtCookie:parentSnapshotCookie verifier:parentSnapshotVerifier providingAttributes:[FSItemGetAttributesRequest new] usingPacker:(FSDirectoryEntryPacker*)parentSnapshotPacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){parentSnapshotError=e;}];
        check(!parentSnapshotError && parentSnapshotPacker.entries.count==2 && [parentSnapshotPacker.entries[1][@"name"] isEqual:@"z-moving"] && [parentSnapshotPacker.entries[1][@"parentID"] unsignedLongLongValue]==parentAAttrs.fileID,"old snapshot entry changed after directory rename");
        __block FSItemAttributes* movingAttrs=nil;
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:movingDirectory replyHandler:^(FSItemAttributes* a,NSError* e){movingAttrs=a;parentSnapshotError=e;}];
        check(!parentSnapshotError && movingAttrs.parentID==parentBAttrs.fileID,"replayed snapshot overwrote the live directory parent");
        }
        [volume openItem:file withModes:FSVolumeOpenModesRead|FSVolumeOpenModesWrite replyHandler:^(NSError* e){result=e;}];check(!result,"open callback failed");
        const char payload[]={'a',0,'b','c'};NSData* contents=[NSData dataWithBytes:payload length:4];__block size_t count=0;
        [volume writeContents:contents toFile:file atOffset:0 replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(!result && count==4,"binary write failed");
        TestBuffer* buffer=[TestBuffer new];buffer.storage=[NSMutableData dataWithLength:32];
        [volume readFromFile:file offset:0 length:32 intoBuffer:(FSMutableFileDataBuffer*)buffer replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(!result && count==4 && !memcmp(buffer.storage.bytes,payload,4),"binary read failed");
        FSItemSetAttributesRequest* flagRequest=[FSItemSetAttributesRequest new];flagRequest.flags=UF_HIDDEN|UF_NODUMP;
        __block FSItemAttributes* flagAttrs=nil;
        [volume setAttributes:flagRequest onItem:file replyHandler:^(FSItemAttributes* a,NSError* e){flagAttrs=a;result=e;}];check(!result && flagAttrs.flags==(UF_HIDDEN|UF_NODUMP) && (flagRequest.consumedAttributes&FSItemAttributeFlags),"flag callback failed");
        FSItemSetAttributesRequest* badFlags=[FSItemSetAttributesRequest new];badFlags.flags=UF_IMMUTABLE;badFlags.size=0;
        [volume setAttributes:badFlags onItem:file replyHandler:^(FSItemAttributes* a,NSError* e){result=e;}];check(result.code==ENOTSUP,"protection flags must be rejected");
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:file replyHandler:^(FSItemAttributes* a,NSError* e){flagAttrs=a;result=e;}];check(!result && flagAttrs.flags==(UF_HIDDEN|UF_NODUMP) && flagAttrs.size==4,"failed flags changed attributes");
        flagRequest=[FSItemSetAttributesRequest new];flagRequest.flags=0;
        [volume setAttributes:flagRequest onItem:file replyHandler:^(FSItemAttributes* a,NSError* e){result=e;}];check(!result,"clear flags callback failed");
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
        if(@available(macOS 27.0,*)){
            __block FSSeekRegionResult* seekResult=nil;
            [volume seekWithinItem:file fromOffset:0 region:FSSeekRegionData context:nil replyHandler:^(FSSeekRegionResult* r,NSError* e){seekResult=r;result=e;}];check(seekResult && !result,"seek data callback failed");
            [volume seekWithinItem:file fromOffset:4 region:FSSeekRegionData context:nil replyHandler:^(FSSeekRegionResult* r,NSError* e){result=e;}];check(result.code==ENXIO,"seek EOF must report ENXIO");
            [volume seekWithinItem:file fromOffset:-1 region:FSSeekRegionHole context:nil replyHandler:^(FSSeekRegionResult* r,NSError* e){result=e;}];check(result.code==EINVAL,"negative seek must fail");
        }
#endif
        check(volume.maximumXattrSizeInBits==18,"wrong xattr size capability");
        FSFileName* attrName=[FSFileName nameWithString:@"user.测试📄"];__block NSData* xattrValue=nil;__block NSArray<FSFileName*>* attrNames=nil;
        const char badName[]={char(0xff)};FSFileName* invalidAttr=[FSFileName nameWithBytes:badName length:1];
        [volume getXattrNamed:invalidAttr ofItem:file replyHandler:^(NSData* v,NSError* e){result=e;}];check(result.code==EINVAL,"invalid UTF-8 xattr get must fail safely");
        [volume setXattrNamed:invalidAttr toData:nil onItem:file policy:FSSetXattrPolicyDelete replyHandler:^(NSError* e){result=e;}];check(result.code==EINVAL,"invalid UTF-8 xattr delete must fail safely");
        [volume getXattrNamed:attrName ofItem:file replyHandler:^(NSData* v,NSError* e){xattrValue=v;result=e;}];check(result.code==ENOATTR,"missing xattr must report ENOATTR");
        [volume setXattrNamed:attrName toData:contents onItem:file policy:FSSetXattrPolicyMustCreate replyHandler:^(NSError* e){result=e;}];check(!result,"xattr create failed");
        [volume getXattrNamed:attrName ofItem:file replyHandler:^(NSData* v,NSError* e){xattrValue=v;result=e;}];check(!result && [xattrValue isEqualToData:contents],"binary xattr read failed");
        [volume listXattrsOfItem:file replyHandler:^(NSArray<FSFileName*>* v,NSError* e){attrNames=v;result=e;}];check(!result && attrNames.count==1 && [attrNames[0].string isEqual:attrName.string],"xattr list failed");
        [volume setXattrNamed:attrName toData:contents onItem:file policy:FSSetXattrPolicyMustCreate replyHandler:^(NSError* e){result=e;}];check(result.code==EEXIST,"xattr create must reject existing name");
        [volume setXattrNamed:attrName toData:[NSData data] onItem:file policy:FSSetXattrPolicyMustReplace replyHandler:^(NSError* e){result=e;}];check(!result,"empty xattr replace failed");
        [volume getXattrNamed:attrName ofItem:file replyHandler:^(NSData* v,NSError* e){xattrValue=v;result=e;}];check(!result && xattrValue.length==0,"empty xattr must remain present");
        [volume setXattrNamed:attrName toData:nil onItem:file policy:FSSetXattrPolicyDelete replyHandler:^(NSError* e){result=e;}];check(!result,"xattr delete failed");
        [volume setXattrNamed:attrName toData:contents onItem:file policy:FSSetXattrPolicyMustReplace replyHandler:^(NSError* e){result=e;}];check(result.code==ENOATTR,"xattr replace must reject missing name");
        [volume setXattrNamed:attrName toData:contents onItem:file policy:FSSetXattrPolicyAlwaysSet replyHandler:^(NSError* e){result=e;}];check(!result,"xattr upsert failed");
        NSMutableData* largeAttr=[NSMutableData dataWithLength:262143];memset(largeAttr.mutableBytes,0xa5,largeAttr.length);
        [volume setXattrNamed:attrName toData:largeAttr onItem:file policy:FSSetXattrPolicyMustReplace replyHandler:^(NSError* e){result=e;}];check(!result,"large xattr set failed");
        [volume getXattrNamed:attrName ofItem:file replyHandler:^(NSData* v,NSError* e){xattrValue=v;result=e;}];check(!result && [xattrValue isEqualToData:largeAttr],"large xattr read failed");
        [volume setXattrNamed:attrName toData:[NSMutableData dataWithLength:262144] onItem:file policy:FSSetXattrPolicyMustReplace replyHandler:^(NSError* e){result=e;}];check(result.code==E2BIG,"oversize xattr must fail before mutation");
        [volume getXattrNamed:attrName ofItem:file replyHandler:^(NSData* v,NSError* e){xattrValue=v;result=e;}];check(!result && [xattrValue isEqualToData:largeAttr],"failed large xattr set changed data");
        [volume setXattrNamed:attrName toData:contents onItem:file policy:FSSetXattrPolicyMustReplace replyHandler:^(NSError* e){result=e;}];check(!result,"restore small xattr failed");
        [volume setXattrNamed:[FSFileName nameWithString:@"user.root"] toData:contents onItem:root policy:FSSetXattrPolicyAlwaysSet replyHandler:^(NSError* e){result=e;}];check(!result,"inode root xattr set failed");
        [volume getXattrNamed:[FSFileName nameWithString:@"user.root"] ofItem:root replyHandler:^(NSData* v,NSError* e){xattrValue=v;result=e;}];check(!result && [xattrValue isEqualToData:contents],"inode root xattr get failed");
        [volume setXattrNamed:[FSFileName nameWithString:@"user.root"] toData:nil onItem:root policy:FSSetXattrPolicyDelete replyHandler:^(NSError* e){result=e;}];check(!result,"inode root xattr remove failed");
        check(volume.maximumFileSize==(uint64_t{1}<<40),"wrong file size capability");
        [volume writeContents:contents toFile:file atOffset:300001 replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(!result && count==4,"large sparse write failed");
        buffer.storage=[NSMutableData dataWithLength:300010];
        [volume readFromFile:file offset:0 length:300010 intoBuffer:(FSMutableFileDataBuffer*)buffer replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(!result && count==300005,"large callback read must complete via bounded requests");
        [volume readFromFile:file offset:299999 length:16 intoBuffer:(FSMutableFileDataBuffer*)buffer replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(!result && count==6 && !memcmp((const char*)buffer.storage.bytes+2,payload,4),"large sparse read failed");
        NSMutableData* oversized=[NSMutableData dataWithLength:300000];
        [volume writeContents:oversized toFile:file atOffset:500000 replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(!result && count==300000,"large callback write must complete via bounded requests");
        [volume writeContents:contents toFile:file atOffset:(uint64_t{1}<<40) replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(result.code==EFBIG && count==0,"oversized file write must fail before mutation");
        [volume writeContents:contents toFile:file atOffset:(uint64_t{5}<<30)+123 replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(!result && count==4,"large-offset write failed");
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:file replyHandler:^(FSItemAttributes* a,NSError* e){result=e;check(a.size>(uint64_t{5}<<30) && a.allocSize>0 && a.allocSize<1024*1024,"sparse allocation must not equal logical size");}];check(!result,"sparse allocation attributes failed");
        [volume readFromFile:file offset:(uint64_t{5}<<30)+121 length:6 intoBuffer:(FSMutableFileDataBuffer*)buffer replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(!result && count==6 && !memcmp((const char*)buffer.storage.bytes+2,payload,4),"large-offset read failed");
        FSItemSetAttributesRequest* shrinkLarge=[FSItemSetAttributesRequest new];shrinkLarge.size=800000;
        [volume setAttributes:shrinkLarge onItem:file replyHandler:^(FSItemAttributes* a,NSError* e){result=e;}];check(!result,"large-offset shrink failed");
        [volume renameItem:file inDirectory:root named:[FSFileName nameWithString:@"文件📄"] toNewName:[FSFileName nameWithString:@"重命名🚀"] inDirectory:root overItem:nil replyHandler:^(FSFileName* n,NSError* e){result=e;}];check(!result,"rename callback failed");
        __block FSItem* again=nil;
        [volume lookupItemNamed:[FSFileName nameWithString:@"重命名🚀"] inDirectory:root replyHandler:^(FSItem* it,FSFileName* n,NSError* e){again=it;result=e;}];check(!result && again==file,"rename lost item identity");
        [volume removeItem:file named:[FSFileName nameWithString:@"重命名🚀"] fromDirectory:root replyHandler:^(NSError* e){result=e;}];check(!result,"unlink failed");
        __block FSItem* snapshotDirectory=nil;__block FSItem* snapshotVictim=nil;
        [volume createItemNamed:[FSFileName nameWithString:@"snapshot-test"] type:FSItemTypeDirectory inDirectory:root attributes:[FSItemSetAttributesRequest new] replyHandler:^(FSItem* item,FSFileName* n,NSError* e){snapshotDirectory=item;result=e;}];check(snapshotDirectory && !result,"snapshot test directory creation failed");
        [volume createItemNamed:[FSFileName nameWithString:@"a-first"] type:FSItemTypeFile inDirectory:snapshotDirectory attributes:[FSItemSetAttributesRequest new] replyHandler:^(FSItem* item,FSFileName* n,NSError* e){result=e;}];check(!result,"snapshot first entry creation failed");
        [volume createItemNamed:[FSFileName nameWithString:@"z-victim"] type:FSItemTypeFile inDirectory:snapshotDirectory attributes:[FSItemSetAttributesRequest new] replyHandler:^(FSItem* item,FSFileName* n,NSError* e){snapshotVictim=item;result=e;}];check(snapshotVictim && !result,"snapshot victim creation failed");
        RecordingPacker* unlinkPacker=[RecordingPacker new];unlinkPacker.limit=1;__block FSDirectoryVerifier unlinkVerifier=FSDirectoryVerifierInitial;__block NSError* unlinkError=nil;
        [volume enumerateDirectory:snapshotDirectory startingAtCookie:FSDirectoryCookieInitial verifier:unlinkVerifier providingAttributes:[FSItemGetAttributesRequest new] usingPacker:(FSDirectoryEntryPacker*)unlinkPacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){unlinkVerifier=verifier;unlinkError=e;}];
        check(!unlinkError && unlinkPacker.entries.count==1 && [unlinkPacker.entries[0][@"name"] isEqual:@"a-first"],"snapshot first page did not stop before victim");
        FSDirectoryCookie unlinkCookie=[unlinkPacker.entries.lastObject[@"cookie"] unsignedLongLongValue];
        [volume removeItem:snapshotVictim named:[FSFileName nameWithString:@"z-victim"] fromDirectory:snapshotDirectory replyHandler:^(NSError* e){unlinkError=e;}];check(!unlinkError,"snapshot victim unlink failed");
        unlinkPacker.packedThisCall=0;
        [volume enumerateDirectory:snapshotDirectory startingAtCookie:unlinkCookie verifier:unlinkVerifier providingAttributes:[FSItemGetAttributesRequest new] usingPacker:(FSDirectoryEntryPacker*)unlinkPacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){unlinkError=e;}];
        check(!unlinkError && unlinkPacker.entries.count==2 && [unlinkPacker.entries[1][@"name"] isEqual:@"z-victim"] && [unlinkPacker.entries[1][@"linkCount"] unsignedLongLongValue]==1,"resumed stream did not retain the captured victim attributes");
        __block FSItemAttributes* victimAttrs=nil;
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:snapshotVictim replyHandler:^(FSItemAttributes* a,NSError* e){victimAttrs=a;unlinkError=e;}];check(!unlinkError && victimAttrs.linkCount==0,"resumed snapshot resurrected an unlinked inode");
        [volume writeContents:[NSData dataWithBytes:"Z" length:1] toFile:file atOffset:0 replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(!result && count==1,"orphan write failed");
        __block FSItemAttributes* attrs=nil;
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:file replyHandler:^(FSItemAttributes* a,NSError* e){attrs=a;result=e;}];check(!result && attrs.linkCount==0,"orphan attributes failed");
        [volume getXattrNamed:attrName ofItem:file replyHandler:^(NSData* v,NSError* e){xattrValue=v;result=e;}];check(!result && [xattrValue isEqualToData:contents],"orphan xattr lost after unlink");
        FSItemSetAttributesRequest* truncate=[FSItemSetAttributesRequest new];truncate.size=2;truncate.mode=0700;
        [volume setAttributes:truncate onItem:file replyHandler:^(FSItemAttributes* a,NSError* e){attrs=a;result=e;}];check(!result && attrs.size==2 && attrs.mode==0700,"orphan setattr failed");
        [volume closeItem:file keepingModes:0 replyHandler:^(NSError* e){result=e;}];check(!result,"close callback failed");
        [volume reclaimItem:file replyHandler:^(NSError* e){result=e;}];check(!result,"reclaim failed");
        [volume synchronizeWithFlags:(FSSyncFlags)0 replyHandler:^(NSError* e){result=e;}];check(!result,"sync failed");
        check(volume.volumeStatistics.blockSize>0,"space callback failed");
        __block FSItem* danglingLink=nil;
        [volume createSymbolicLinkNamed:[FSFileName nameWithString:@"dangling-attributes"] inDirectory:root attributes:[FSItemSetAttributesRequest new] linkContents:[FSFileName nameWithString:@"missing-target"] replyHandler:^(FSItem* it,FSFileName* n,NSError* e){danglingLink=it;result=e;}];check(danglingLink && !result,"dangling symlink creation failed");
        FSItemSetAttributesRequest* linkAttrs=[FSItemSetAttributesRequest new];linkAttrs.mode=0701;linkAttrs.accessTime=(timespec){111,222};linkAttrs.modifyTime=(timespec){333,444};linkAttrs.birthTime=(timespec){55,66};
        [volume setAttributes:linkAttrs onItem:danglingLink replyHandler:^(FSItemAttributes* a,NSError* e){attrs=a;result=e;}];
        check(!result && attrs.type==FSItemTypeSymlink && attrs.mode==0701 && attrs.modifyTime.tv_sec==333 && attrs.accessTime.tv_nsec==222 && attrs.birthTime.tv_nsec==66,"symlink attributes must not resolve the missing target");
        FSItemSetAttributesRequest* badLinkSize=[FSItemSetAttributesRequest new];badLinkSize.mode=0777;badLinkSize.size=attrs.size;
        [volume setAttributes:badLinkSize onItem:danglingLink replyHandler:^(FSItemAttributes* a,NSError* e){result=e;}];check(result.code==EINVAL,"symlink size change must fail even for the current length");
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:danglingLink replyHandler:^(FSItemAttributes* a,NSError* e){attrs=a;result=e;}];check(!result && attrs.mode==0701 && attrs.modifyTime.tv_sec==333,"invalid symlink size partially changed metadata");
        [volume openItem:danglingLink withModes:FSVolumeOpenModesRead|FSVolumeOpenModesWrite replyHandler:^(NSError* e){result=e;}];check(!result,"symlink metadata open failed");
        [volume setXattrNamed:attrName toData:contents onItem:danglingLink policy:FSSetXattrPolicyAlwaysSet replyHandler:^(NSError* e){result=e;}];check(!result,"symlink fd xattr set failed");
        [volume readFromFile:danglingLink offset:0 length:0 intoBuffer:(FSMutableFileDataBuffer*)buffer replyHandler:^(size_t n,NSError* e){result=e;count=n;}];check(result.code==EPERM && count==0,"symlink data read must fail even when empty");
        [volume writeContents:[NSData data] toFile:danglingLink atOffset:0 replyHandler:^(size_t n,NSError* e){result=e;count=n;}];check(result.code==EPERM && count==0,"symlink data write must fail even when empty");
        [volume removeItem:danglingLink named:[FSFileName nameWithString:@"dangling-attributes"] fromDirectory:root replyHandler:^(NSError* e){result=e;}];check(!result,"open symlink unlink failed");
        __block FSItem* reusedLink=nil;
        [volume createSymbolicLinkNamed:[FSFileName nameWithString:@"dangling-attributes"] inDirectory:root attributes:[FSItemSetAttributesRequest new] linkContents:[FSFileName nameWithString:@"replacement"] replyHandler:^(FSItem* it,FSFileName* n,NSError* e){reusedLink=it;result=e;}];check(reusedLink && reusedLink!=danglingLink && !result,"symlink path reuse lost identity");
        [volume getXattrNamed:attrName ofItem:danglingLink replyHandler:^(NSData* v,NSError* e){xattrValue=v;result=e;}];check(!result && [xattrValue isEqualToData:contents],"orphan symlink fd lost xattr");
        [volume closeItem:danglingLink keepingModes:FSVolumeOpenModesRead replyHandler:^(NSError* e){result=e;}];check(!result,"symlink writer downgrade failed");
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:danglingLink replyHandler:^(FSItemAttributes* a,NSError* e){attrs=a;result=e;}];check(!result && attrs.linkCount==0 && attrs.type==FSItemTypeSymlink,"symlink reader must retain orphan after downgrade");
        [volume closeItem:danglingLink keepingModes:0 replyHandler:^(NSError* e){result=e;}];check(!result,"symlink metadata close failed");
        [volume reclaimItem:danglingLink replyHandler:^(NSError* e){result=e;}];check(!result,"symlink metadata reclaim failed");
        [volume deactivateWithOptions:0 replyHandler:^(NSError* e){result=e;}];check(!result,"deactivation failed");volume=nil;
        NSMutableDictionary* roConfig=[@{@"schema_version":@1,@"revision":@"base",@"repository":status[@"repository_id"]} mutableCopy];
        if(config[@"rpc"])roConfig[@"rpc"]=config[@"rpc"];
        NSData* roData=[NSJSONSerialization dataWithJSONObject:roConfig options:0 error:nil];
        int roFD=open(configPath.fileSystemRepresentation,O_WRONLY|O_TRUNC);check(roFD>=0,"cannot replace descriptor");
        check(write(roFD,roData.bytes,roData.length)==static_cast<ssize_t>(roData.length),"cannot write revision descriptor");close(roFD);
        volume=[[RevisionVolume alloc] initWithRuntimeURL:[NSURL fileURLWithPath:runtime isDirectory:YES] error:&error];check(volume!=nil,"revision volume initialization failed");
        [volume activateWithOptions:nil replyHandler:^(FSItem* it,NSError* e){root=it;result=e;}];check(root && !result,"revision activation failed");
        [volume createItemNamed:[FSFileName nameWithString:@"forbidden"] type:FSItemTypeFile inDirectory:root attributes:[FSItemSetAttributesRequest new] replyHandler:^(FSItem* it,FSFileName* n,NSError* e){result=e;}];check(result.code==EROFS,"revision creation must fail read-only");
        [volume getXattrNamed:[FSFileName nameWithString:@"user.retained"] ofItem:root replyHandler:^(NSData* v,NSError* e){xattrValue=v;result=e;}];check(!result && xattrValue.length==3 && !memcmp(xattrValue.bytes,"R\0X",3),"revision xattr read failed");
        [volume setXattrNamed:attrName toData:contents onItem:root policy:FSSetXattrPolicyAlwaysSet replyHandler:^(NSError* e){result=e;}];check(result.code==EROFS,"revision xattr write must fail read-only");
        [server terminate];[server waitUntilExit];server=nil;
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:root replyHandler:^(FSItemAttributes* a,NSError* e){result=e;}];check(result!=nil,"daemon loss must not use cached fallback");
        volume=nil;
        if(bridge.running){[bridge terminate];[bridge waitUntilExit];bridge=nil;}
        command(binary,@[@"fs-check",db]);command(binary,@[@"gc",db]);
        [[NSFileManager defaultManager] removeItemAtPath:runtime error:nil];
        std::cout<<"FSKit callbacks -> resident daemon: binary I/O, identity, open-unlink, atomic attributes and sync passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";volume=nil;if(bridge.running){[bridge terminate];[bridge waitUntilExit];}if(server.running){[server terminate];[server waitUntilExit];}[[NSFileManager defaultManager] removeItemAtPath:runtime error:nil];return 1;}
}}
