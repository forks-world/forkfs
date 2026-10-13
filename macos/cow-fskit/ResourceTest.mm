#import "RevisionFileSystem.h"
#import "RevisionVolume.h"
#include "namespace.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <sys/stat.h>
#define CHECK(x) do{if(!(x))throw std::runtime_error("line "+std::to_string(__LINE__)+": " #x);}while(0)
// Only the sandbox grant is simulated; descriptors, Container, and volumes are real.
@interface ScopedTestURL : NSURL
@property(nonatomic,strong) NSURL *backing;
@property(nonatomic) NSUInteger starts;
@property(nonatomic) NSUInteger stops;
@property(nonatomic) BOOL denied;
@end
@implementation ScopedTestURL
- (NSString *)absoluteString {return self.backing.absoluteString;}
- (NSString *)relativeString {return self.backing.relativeString;}
- (NSURL *)baseURL {return nil;}
- (NSURL *)absoluteURL {return self;}
- (BOOL)isFileURL {return YES;}
- (const char *)fileSystemRepresentation {return self.backing.fileSystemRepresentation;}
- (id)copyWithZone:(NSZone *)zone {return self;}
- (BOOL)startAccessingSecurityScopedResource {++self.starts;return !self.denied;}
- (void)stopAccessingSecurityScopedResource {++self.stops;}
@end
// Cleanup failure is deterministic and does not need a production fault hook.
@interface FailingRevoke : NSObject
@property(nonatomic) NSUInteger calls;
- (NSError *)revoke;
@end
@implementation FailingRevoke
- (NSError *)revoke {return ++self.calls==1?fs_errorForPOSIXError(EIO):nil;}
@end
static void put(const std::string& path,const std::string& value){std::ofstream out(path,std::ios::binary);out<<value;CHECK(out.good());}
static ForkRevisionVolume *load(ForkRevisionFileSystem *fs,FSResource *resource,int code=0){
    // FSTaskOptions is system-created; the loader does not inspect options.
    __block ForkRevisionVolume *result=nil;__block int calls=0;
    [fs loadResource:resource options:(FSTaskOptions *)[NSObject new] replyHandler:^(FSVolume *v,NSError *e){++calls;if(e.code!=code)throw std::runtime_error("load errno "+std::to_string(e.code)+", expected "+std::to_string(code));CHECK(code?!v:v!=nil);result=(ForkRevisionVolume *)v;}];CHECK(calls==1);return result;
}
static void unload(ForkRevisionFileSystem *fs,FSResource *resource,int code=0){
    __block int calls=0;[fs unloadResource:resource options:(FSTaskOptions *)[NSObject new] replyHandler:^(NSError *e){++calls;CHECK(e.code==code);}];CHECK(calls==1);
}
static FSItem *lookup(ForkRevisionVolume *v,NSString *name){
    __block FSItem *item=nil;__block int calls=0;
    [v lookupItemNamed:[FSFileName nameWithString:name] inDirectory:v.rootItem replyHandler:^(FSItem *i,FSFileName *n,NSError *e){++calls;CHECK(!e && i);item=i;}];CHECK(calls==1);return item;
}
int main(){@autoreleasepool {
    auto pattern=(std::filesystem::temp_directory_path()/"ff-resource-XXXXXX").string();CHECK(mkdtemp(pattern.data()));
    try{
        auto path=pattern+"/store";forkfs::Container::create(path);
        {forkfs::Container store(path);forkfs::Namespace fs(store);fs.initialize();fs.write("/a",{1});fs.snapshot("base");fs.fork("base","work");}
        auto url=[ScopedTestURL new];url.backing=[NSURL fileURLWithPath:@(path.c_str()) isDirectory:YES];
        auto resource=[[FSPathURLResource alloc] initWithURL:url writable:YES];
        auto roResource=[[FSPathURLResource alloc] initWithURL:url writable:NO];
        auto fs=[ForkRevisionFileSystem new];
        auto revision=path+"/.forkfs-fskit-revision",world=path+"/.forkfs-fskit-world";
        CHECK(![ForkFSResourceSelection selectionAtURL:url]);
        load(fs,resource,EINVAL);CHECK(url.starts==1 && url.stops==1);
        put(revision,"base\n");auto r=[ForkFSResourceSelection selectionAtURL:url];CHECK(r.readOnly && [r.name isEqualToString:@"base"]);
        CHECK([[r probeIdentityAtURL:url] isEqual:[r probeIdentityAtURL:url]]);
        put(world,"base");CHECK(![ForkFSResourceSelection selectionAtURL:url]);load(fs,resource,EINVAL);
        std::filesystem::remove(revision);auto w=[ForkFSResourceSelection selectionAtURL:url];CHECK(w && !w.readOnly);
        CHECK(![[r probeIdentityAtURL:url] isEqual:[w probeIdentityAtURL:url]]);
        put(world,"other");CHECK(![[w probeIdentityAtURL:url] isEqual:[[ForkFSResourceSelection selectionAtURL:url] probeIdentityAtURL:url]]);
        auto otherURL=[NSURL fileURLWithPath:@"/different/path"];
        CHECK(![[w probeIdentityAtURL:url] isEqual:[w probeIdentityAtURL:otherURL]]);
        for(const auto& invalid:std::vector<std::string>{"","../work","work\n\n",std::string("work\0x",6),std::string(65,'a'),std::string(1,char(255))}){
            put(world,invalid);CHECK(![ForkFSResourceSelection selectionAtURL:url]);
        }
        put(world,std::string(64,'a')+"\n");CHECK([ForkFSResourceSelection selectionAtURL:url].name.length==64);
        std::filesystem::remove(world);std::filesystem::create_symlink("absent",world);CHECK(![ForkFSResourceSelection selectionAtURL:url]);
        put(revision,"base");CHECK(![ForkFSResourceSelection selectionAtURL:url]);std::filesystem::remove(revision);
        std::filesystem::remove(world);std::filesystem::create_directory(world);CHECK(![ForkFSResourceSelection selectionAtURL:url]);std::filesystem::remove(world);
        CHECK(mkfifo(world.c_str(),0600)==0);CHECK(![ForkFSResourceSelection selectionAtURL:url]);std::filesystem::remove(world);
        put(world,"work");
        auto alias=pattern+"/alias";std::filesystem::create_directory_symlink(path,alias);
        CHECK(![ForkFSResourceSelection selectionAtURL:[NSURL fileURLWithPath:@(alias.c_str())]]);
        auto starts=url.starts;load(fs,roResource,EROFS);CHECK(url.starts==starts);
        url.denied=YES;auto stops=url.stops;load(fs,resource,EPERM);CHECK(url.stops==stops);url.denied=NO;
        // Probe must not contend with an existing database owner.
        {forkfs::Container owner(path);__block int calls=0;
            [fs probeResource:resource replyHandler:^(FSProbeResult *result,NSError *error){++calls;CHECK(result.result==FSMatchResultUsable && [result.name isEqualToString:@"work"] && result.containerID && !error);}];CHECK(calls==1);
        }
        CHECK(url.starts==url.stops+1); // One denied grant, never balanced by stop.
        auto v=load(fs,resource);CHECK(v.rootItem);starts=url.starts;load(fs,resource,EBUSY);CHECK(url.starts==starts);
        auto a=lookup(v,@"a");__block int calls=0;unsigned char byte=7;
        [v writeContents:[NSData dataWithBytes:&byte length:1] toFile:a atOffset:0 replyHandler:^(size_t count,NSError *error){++calls;CHECK(!error && count==1);}];CHECK(calls==1);
        // Selection changes cannot retarget an already loaded volume.
        put(world,"main");unload(fs,resource);CHECK(!v.rootItem);CHECK(url.starts==url.stops+1);
        {forkfs::Container store(path);forkfs::Namespace main(store),work(store,"work");CHECK(main.read("/a")==std::vector<unsigned char>{1});CHECK(work.read("/a")==std::vector<unsigned char>{7});}
        // Missing World/revision releases both database ownership and grant.
        put(world,"missing");load(fs,resource,EIO);CHECK(url.starts==url.stops+1);
        put(world,"work");v=load(fs,resource);unload(fs,resource);
        std::filesystem::remove(world);put(revision,"base");v=load(fs,resource);a=lookup(v,@"a");calls=0;
        [v writeContents:[NSData data] toFile:a atOffset:0 replyHandler:^(size_t count,NSError *error){++calls;CHECK(!count && error.code==EROFS);}];CHECK(calls==1);unload(fs,resource);
        put(revision,"missing");load(fs,resource,EIO);CHECK(url.starts==url.stops+1);
        // Exercise retryable loader teardown with a failing volume double.
        auto failure=[FailingRevoke new];[fs setValue:failure forKey:@"volume"];
        CHECK([url startAccessingSecurityScopedResource]);[fs setValue:url forKey:@"grantedURL"];
        stops=url.stops;unload(fs,resource,EIO);CHECK(url.stops==stops && failure.calls==1);
        load(fs,resource,EBUSY);unload(fs,resource);CHECK(url.stops==stops+1 && failure.calls==2);
        unload(fs,resource);CHECK(url.stops==stops+1);
        // Final object destruction balances a grant even on cleanup failure.
        @autoreleasepool{auto dying=[ForkRevisionFileSystem new];[dying setValue:[FailingRevoke new] forKey:@"volume"];
            CHECK([url startAccessingSecurityScopedResource]);[dying setValue:url forKey:@"grantedURL"];dying=nil;}
        CHECK(url.starts==url.stops+1);
        std::filesystem::remove_all(pattern);std::cout<<"resource selection and lifecycle passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";std::filesystem::remove_all(pattern);return 1;}
}}
