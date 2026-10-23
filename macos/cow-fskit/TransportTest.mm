#import "RevisionVolume.h"
#import "RevisionFileSystem.h"
#import "VolumeView.h"
#include "rpc.h"
#include <fstream>
#include <iostream>
#include <unistd.h>
#define CHECK(x) do{if(!(x))throw std::runtime_error("transport test line "+std::to_string(__LINE__)+": " #x);}while(0)
// Only the OS-supplied security-scope grant is simulated. Session resource,
// ownership/ACL admission, peer credentials, socket and callbacks remain real.
@interface TransportURL : NSURL
@property(nonatomic,strong) NSURL *backing;
@end
@implementation TransportURL
- (NSString *)absoluteString{return self.backing.absoluteString;}
- (NSString *)relativeString{return self.backing.relativeString;}
- (NSURL *)baseURL{return nil;}
- (NSURL *)absoluteURL{return self;}
- (NSString *)path{return self.backing.path;}
- (BOOL)isFileURL{return YES;}
- (const char *)fileSystemRepresentation{return self.backing.fileSystemRepresentation;}
- (id)copyWithZone:(NSZone *)zone{return self;}
- (NSURL *)URLByAppendingPathComponent:(NSString *)component{return [self.backing URLByAppendingPathComponent:component];}
- (NSURL *)URLByResolvingSymlinksInPath{return self.backing.URLByResolvingSymlinksInPath;}
- (BOOL)startAccessingSecurityScopedResource{return YES;}
- (void)stopAccessingSecurityScopedResource{}
@end
int main(int argc,char **argv){@autoreleasepool{try{
    CHECK(argc==3 || argc==4);
    ForkRevisionVolume *volume=nil;ForkRevisionFileSystem *loader=nil;FSPathURLResource *resource=nil;
    std::optional<forkfs::MountCredentials> c;
    if(std::string(argv[1])=="--resource"){
        auto url=[TransportURL new];url.backing=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]];
        c=forkfs::mount_credentials(url);CHECK(c);
        loader=[ForkRevisionFileSystem new];resource=[[FSPathURLResource alloc] initWithURL:url writable:NO];
        __block ForkRevisionVolume *loaded=nil;__block unsigned calls=0;
        [loader loadResource:resource options:(FSTaskOptions *)[NSObject new] replyHandler:^(FSVolume *v,NSError *e){++calls;CHECK(!e && v);loaded=(ForkRevisionVolume *)v;}];
        CHECK(calls==1);volume=loaded;
        // A frontend credential must not give management authority.
        bool denied=false;try{forkfs::request_rpc(c->socket,{"status"},c->service_uid);}catch(...){denied=true;}CHECK(denied);
    }else{
        CHECK(std::string(argv[1])=="--test-config");
        std::ifstream input(argv[2]);forkfs::MountCredentials config{};unsigned uid;int ro;
        input>>config.socket>>uid>>config.generation>>config.session>>config.name>>ro;CHECK(input.good());
        config.service_uid=uid;config.read_only=ro;config.lease_ms=3000;c=config;
        NSError *error=nil;volume=[[ForkRevisionVolume alloc] initWithMountCredentials:*c error:&error];CHECK(volume && !error);
    }
    auto root=volume.rootItem;CHECK(root);
    if(c->read_only){
        __block unsigned replies=0;
        [volume createItemNamed:[FSFileName nameWithString:@"denied"] type:FSItemTypeFile inDirectory:root attributes:[FSItemSetAttributesRequest new]
            replyHandler:^(FSItem *created,FSFileName *,NSError *e){++replies;CHECK(!created && e.code==EROFS);}];CHECK(replies==1);
        auto transport=std::make_shared<forkfs::MountTransport>(*c);forkfs::VolumeView view(transport);
        auto cursor=view.open_directory_inode(view.root_inode().id);auto page=view.read_directory(cursor,128,true);CHECK(!page.entries.empty());
        CHECK(!view.read_inode(page.entries.front().inode.id,0,16).empty());
        if(loader)[loader unloadResource:resource options:(FSTaskOptions *)[NSObject new] replyHandler:^(NSError *e){CHECK(!e);}];
        else CHECK(![volume revoke]);
        std::cout<<"OK"<<std::endl;return 0;
    }
    __block unsigned calls=0;__block FSItem *item=nil;
    char invalid[]={'a','\n',char(0xff)};
    [volume createItemNamed:[FSFileName nameWithBytes:invalid length:sizeof(invalid)] type:FSItemTypeFile inDirectory:root attributes:[FSItemSetAttributesRequest new]
        replyHandler:^(FSItem *created,FSFileName *,NSError *e){CHECK(!created && e.code==EINVAL);}];
    // Preserve the metadata layer's printable-ASCII policy, while proving that
    // quote/backslash names are byte-exact through raw directory responses.
    char raw[]={'a','"','\\'};auto name=[FSFileName nameWithBytes:raw length:sizeof(raw)];
    [volume createItemNamed:name type:FSItemTypeFile inDirectory:root attributes:[FSItemSetAttributesRequest new]
        replyHandler:^(FSItem *created,FSFileName *,NSError *error){++calls;CHECK(!error && created);item=created;}];CHECK(calls==1);
    char payload[]={0,1,char(0xff),2};NSData *data=[NSData dataWithBytes:payload length:sizeof(payload)];calls=0;
    [volume writeContents:data toFile:item atOffset:0 replyHandler:^(size_t n,NSError *e){++calls;CHECK(!e && n==sizeof(payload));}];CHECK(calls==1);
    // Binary xattr values remain byte-exact; namespace xattr names are ASCII.
    auto xname=[FSFileName nameWithString:@"user.binary"];
    [volume setXattrNamed:xname toData:data onItem:item policy:FSSetXattrPolicyAlwaysSet replyHandler:^(NSError *e){CHECK(!e);}];
    [volume listXattrsOfItem:item replyHandler:^(NSArray<FSFileName *> *names,NSError *e){CHECK(!e && names.count==1 && [names.firstObject.data isEqual:xname.data]);}];
    [volume getXattrNamed:xname ofItem:item replyHandler:^(NSData *value,NSError *e){CHECK(!e && [value isEqual:data]);}];
    // Cursor copies share one pinned server snapshot and can replay old cookies.
    auto transport=std::make_shared<forkfs::MountTransport>(*c);forkfs::VolumeView view(transport);
    auto cursor=view.open_directory_inode(view.root_inode().id);auto copy=cursor;
    auto first=view.read_directory(cursor,1,true);CHECK(first.entries.size()==1 && first.entries[0].name==std::string(raw,sizeof(raw)));
    CHECK(view.read_inode(first.entries[0].inode.id,0,16)==std::vector<unsigned char>({0,1,255,2}));
    auto replay=view.read_directory(copy,1,true);CHECK(replay.entries.size()==1 && replay.entries[0].inode.id==first.entries[0].inode.id);
    view.handle_set_attributes("not-a-handle",{}); // Empty patches perform no RPC.
    [volume synchronizeWithFlags:(FSSyncFlags)0 replyHandler:^(NSError *e){CHECK(!e);}];
    // Idle longer than the 3s local lease; background renewal must retain it.
    if(argc==4){sleep(4);CHECK(view.root_inode().directory);
        std::cout<<"READY"<<std::endl;char signal;std::cin.get(signal);
        bool disconnected=false;try{view.root_inode();}catch(const std::system_error& e){disconnected=e.code().value()==ENOTCONN;}CHECK(disconnected);
        // Permanent local invalidation: never reopen against the new instance.
        CHECK(!transport->healthy());
        [volume writeContents:data toFile:item atOffset:0 replyHandler:^(size_t n,NSError *e){CHECK(n==0 && e.code==ENOTCONN);}];
    }
    if(loader){[loader unloadResource:resource options:(FSTaskOptions *)[NSObject new] replyHandler:^(NSError *e){CHECK(!e);}];}
    else CHECK(![volume revoke]);
    std::cout<<"OK"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}}
