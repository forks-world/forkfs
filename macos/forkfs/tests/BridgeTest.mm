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
    if(argc!=2)return 2;char tmp[]="/tmp/ff-fskit-XXXXXX";if(!mkdtemp(tmp))return 2;
    NSString* runtime=@(tmp);NSString* db=[runtime stringByAppendingPathComponent:@"db"];
    NSString* binary=@(argv[1]);NSTask* server=nil;RevisionVolume* volume=nil;
    try {
        command(binary,@[@"init",db]);command(binary,@[@"fs-init",db]);command(binary,@[@"fs-snapshot",db,@"base"]);
        NSDictionary* status=[NSJSONSerialization JSONObjectWithData:command(binary,@[@"check",db]) options:0 error:nil];
        NSDictionary* config=@{@"schema_version":@1,@"world":@"main",@"repository":status[@"repository_id"]};
        NSString* configPath=[runtime stringByAppendingPathComponent:@"forkfs.json"];
        NSData* encoded=[NSJSONSerialization dataWithJSONObject:config options:0 error:nil];
        int configFD=open(configPath.fileSystemRepresentation,O_WRONLY|O_CREAT|O_EXCL,0600);check(configFD>=0,"cannot create mount descriptor");
        check(write(configFD,encoded.bytes,encoded.length)==static_cast<ssize_t>(encoded.length),"cannot write descriptor");close(configFD);
        NSString* socket=[runtime stringByAppendingPathComponent:@"control.sock"];
        server=[NSTask new];server.executableURL=[NSURL fileURLWithPath:binary];server.arguments=@[@"serve",db,@"--socket",socket,@"--rpc"];
        NSPipe* pipe=[NSPipe pipe];server.standardOutput=pipe;server.standardError=[NSPipe pipe];NSError* error=nil;
        check([server launchAndReturnError:&error],"cannot launch resident daemon");
        // Read the ready status line, emitted after listen().
        check([pipe.fileHandleForReading availableData].length>0,"daemon did not become ready");
        volume=[[RevisionVolume alloc] initWithRuntimeURL:[NSURL fileURLWithPath:runtime isDirectory:YES] error:&error];
        if(error)std::cerr<<error.description.UTF8String<<"\n";check(volume!=nil && !error,"volume initialization failed");
        __block FSItem* root=nil;__block NSError* result=nil;
        [volume activateWithOptions:nil replyHandler:^(FSItem* item,NSError* e){root=item;result=e;}];check(root && !result,"activation failed");
        __block FSItemAttributes* rootAttrs=nil;
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:root replyHandler:^(FSItemAttributes* a,NSError* e){rootAttrs=a;result=e;}];check(!result && rootAttrs.mode==0700,"mount root must be private");
        FSItemSetAttributesRequest* expose=[FSItemSetAttributesRequest new];expose.mode=0755;
        [volume setAttributes:expose onItem:root replyHandler:^(FSItemAttributes* a,NSError* e){result=e;}];check(result.code==EPERM,"must not expose private mount root");
        __block FSItem* file=nil;FSItemSetAttributesRequest* create=[FSItemSetAttributesRequest new];create.mode=0600;
        [volume createItemNamed:[FSFileName nameWithString:@"file"] type:FSItemTypeFile inDirectory:root attributes:create replyHandler:^(FSItem* item,FSFileName* n,NSError* e){file=item;result=e;}];check(file && !result,"create callback failed");
        __block FSItemAttributes* fileAttrs=nil;
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:file replyHandler:^(FSItemAttributes* a,NSError* e){fileAttrs=a;result=e;}];check(fileAttrs && !result,"file identity lookup failed");
        BOOL expectDots=[NSProcessInfo processInfo].operatingSystemVersion.majorVersion>=27;
        NSArray<NSString*>* expectedNames=expectDots?@[@".",@"..",@"file"]:@[@"file"];
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
            if([entry[@"name"] isEqual:@"file"])check([entry[@"id"] unsignedLongLongValue]==fileAttrs.fileID,"normal directory item ID changed");
        }
        directoryPacker.packedThisCall=0;__block NSError* eofError=nil;
        [volume enumerateDirectory:root startingAtCookie:next verifier:directoryVerifier providingAttributes:nil usingPacker:(FSDirectoryEntryPacker*)directoryPacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){eofError=e;}];
        check(!eofError && directoryPacker.packedThisCall==0 && directoryPacker.entries.count==expectedNames.count,"end-of-directory resume duplicated entries");
        RecordingPacker* attributePacker=[RecordingPacker new];attributePacker.limit=8;
        __block FSDirectoryVerifier attributeVerifier=FSDirectoryVerifierInitial;__block NSError* enumerationError=nil;
        [volume enumerateDirectory:root startingAtCookie:FSDirectoryCookieInitial verifier:attributeVerifier providingAttributes:[FSItemGetAttributesRequest new] usingPacker:(FSDirectoryEntryPacker*)attributePacker replyHandler:^(FSDirectoryVerifier verifier,NSError* e){attributeVerifier=verifier;enumerationError=e;}];
        check(!enumerationError && attributePacker.entries.count==1 && [attributePacker.entries[0][@"name"] isEqual:@"file"],"attribute directory enumeration must omit dot entries");
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
        [volume renameItem:file inDirectory:root named:[FSFileName nameWithString:@"file"] toNewName:[FSFileName nameWithString:@"renamed"] inDirectory:root overItem:nil replyHandler:^(FSFileName* n,NSError* e){result=e;}];check(!result,"rename callback failed");
        __block FSItem* again=nil;
        [volume lookupItemNamed:[FSFileName nameWithString:@"renamed"] inDirectory:root replyHandler:^(FSItem* it,FSFileName* n,NSError* e){again=it;result=e;}];check(!result && again==file,"rename lost item identity");
        [volume removeItem:file named:[FSFileName nameWithString:@"renamed"] fromDirectory:root replyHandler:^(NSError* e){result=e;}];check(!result,"unlink failed");
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
        FSItemSetAttributesRequest* truncate=[FSItemSetAttributesRequest new];truncate.size=2;truncate.mode=0700;
        [volume setAttributes:truncate onItem:file replyHandler:^(FSItemAttributes* a,NSError* e){attrs=a;result=e;}];check(!result && attrs.size==2 && attrs.mode==0700,"orphan setattr failed");
        [volume closeItem:file keepingModes:0 replyHandler:^(NSError* e){result=e;}];check(!result,"close callback failed");
        [volume reclaimItem:file replyHandler:^(NSError* e){result=e;}];check(!result,"reclaim failed");
        [volume synchronizeWithFlags:(FSSyncFlags)0 replyHandler:^(NSError* e){result=e;}];check(!result,"sync failed");
        check(volume.volumeStatistics.blockSize>0,"space callback failed");
        [volume deactivateWithOptions:0 replyHandler:^(NSError* e){result=e;}];check(!result,"deactivation failed");volume=nil;
        NSDictionary* roConfig=@{@"schema_version":@1,@"revision":@"base",@"repository":status[@"repository_id"]};
        NSData* roData=[NSJSONSerialization dataWithJSONObject:roConfig options:0 error:nil];
        int roFD=open(configPath.fileSystemRepresentation,O_WRONLY|O_TRUNC);check(roFD>=0,"cannot replace descriptor");
        check(write(roFD,roData.bytes,roData.length)==static_cast<ssize_t>(roData.length),"cannot write revision descriptor");close(roFD);
        volume=[[RevisionVolume alloc] initWithRuntimeURL:[NSURL fileURLWithPath:runtime isDirectory:YES] error:&error];check(volume!=nil,"revision volume initialization failed");
        [volume activateWithOptions:nil replyHandler:^(FSItem* it,NSError* e){root=it;result=e;}];check(root && !result,"revision activation failed");
        [volume createItemNamed:[FSFileName nameWithString:@"forbidden"] type:FSItemTypeFile inDirectory:root attributes:[FSItemSetAttributesRequest new] replyHandler:^(FSItem* it,FSFileName* n,NSError* e){result=e;}];check(result.code==EROFS,"revision creation must fail read-only");
        [server terminate];[server waitUntilExit];server=nil;
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:root replyHandler:^(FSItemAttributes* a,NSError* e){result=e;}];check(result!=nil,"daemon loss must not use cached fallback");
        volume=nil;
        command(binary,@[@"fs-check",db]);command(binary,@[@"gc",db]);
        [[NSFileManager defaultManager] removeItemAtPath:runtime error:nil];
        std::cout<<"FSKit callbacks -> resident daemon: binary I/O, identity, open-unlink, atomic attributes and sync passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";volume=nil;if(server.running){[server terminate];[server waitUntilExit];}[[NSFileManager defaultManager] removeItemAtPath:runtime error:nil];return 1;}
}}
