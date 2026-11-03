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
        [volume openItem:file withModes:FSVolumeOpenModesRead|FSVolumeOpenModesWrite replyHandler:^(NSError* e){result=e;}];check(!result,"open callback failed");
        const char payload[]={'a',0,'b','c'};NSData* contents=[NSData dataWithBytes:payload length:4];__block size_t count=0;
        [volume writeContents:contents toFile:file atOffset:0 replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(!result && count==4,"binary write failed");
        TestBuffer* buffer=[TestBuffer new];buffer.storage=[NSMutableData dataWithLength:32];
        [volume readFromFile:file offset:0 length:32 intoBuffer:(FSMutableFileDataBuffer*)buffer replyHandler:^(size_t n,NSError* e){count=n;result=e;}];check(!result && count==4 && !memcmp(buffer.storage.bytes,payload,4),"binary read failed");
        [volume renameItem:file inDirectory:root named:[FSFileName nameWithString:@"file"] toNewName:[FSFileName nameWithString:@"renamed"] inDirectory:root overItem:nil replyHandler:^(FSFileName* n,NSError* e){result=e;}];check(!result,"rename callback failed");
        __block FSItem* again=nil;
        [volume lookupItemNamed:[FSFileName nameWithString:@"renamed"] inDirectory:root replyHandler:^(FSItem* it,FSFileName* n,NSError* e){again=it;result=e;}];check(!result && again==file,"rename lost item identity");
        [volume removeItem:file named:[FSFileName nameWithString:@"renamed"] fromDirectory:root replyHandler:^(NSError* e){result=e;}];check(!result,"unlink failed");
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
