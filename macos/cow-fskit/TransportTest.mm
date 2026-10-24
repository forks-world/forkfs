#import "RevisionVolume.h"
#import "RevisionFileSystem.h"
#import "VolumeView.h"
#include "rpc.h"
#include <cerrno>
#include <fstream>
#include <iostream>
#include <unistd.h>
#define CHECK(x) do{if(!(x))throw std::runtime_error("transport test line "+std::to_string(__LINE__)+": " #x);}while(0)
@interface TransportBuffer : NSObject
@property(nonatomic,strong) NSMutableData *data;
- (NSUInteger)length;
- (void *)mutableBytes;
@end
@implementation TransportBuffer
- (NSUInteger)length{return self.data.length;}
- (void *)mutableBytes{return self.data.mutableBytes;}
@end
@interface PaginationPacker : NSObject
@property(nonatomic) NSUInteger capacity;
@property(nonatomic,strong) NSMutableArray<NSString *> *names;
@property(nonatomic,strong) NSMutableArray<NSNumber *> *cookies;
@property(nonatomic) NSUInteger attributeCount;
@end
@implementation PaginationPacker
- (instancetype)init {if((self=[super init])){_capacity=31;_names=[NSMutableArray new];_cookies=[NSMutableArray new];}return self;}
- (BOOL)packEntryWithName:(FSFileName *)name itemType:(FSItemType)type itemID:(FSItemID)itemID nextCookie:(FSDirectoryCookie)cookie attributes:(FSItemAttributes *)attributes {
    if(self.names.count==self.capacity)return NO;
    [self.names addObject:name.string];[self.cookies addObject:@(cookie)];if(attributes)++_attributeCount;return YES;
}
@end
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
static FSItem *unicodeCreate(ForkRevisionVolume *v,FSItem *parent,NSString *name,FSItemType type){
    __block FSItem *result=nil;__block unsigned calls=0;auto requested=[FSFileName nameWithString:name];
    [v createItemNamed:requested type:type inDirectory:parent attributes:[FSItemSetAttributesRequest new]
        replyHandler:^(FSItem *item,FSFileName *actual,NSError *e){++calls;CHECK(!e && item && [actual.data isEqual:requested.data]);result=item;}];
    CHECK(calls==1 && result);return result;
}
static void unicodeCallbacks(ForkRevisionVolume *v,FSItem *root,NSData *payload){
    auto dir=unicodeCreate(v,root,@"中文目录😀",FSItemTypeDirectory);
    NSMutableArray<FSItem *> *files=[NSMutableArray new];
    for(NSString *name in @[@"源代码🧪.txt",@"café",@"cafe\u0301"]){
        auto file=unicodeCreate(v,dir,name,FSItemTypeFile);[files addObject:file];
        [v writeContents:payload toFile:file atOffset:0 replyHandler:^(size_t n,NSError *e){CHECK(!e && n==payload.length);}];
        auto buffer=[TransportBuffer new];buffer.data=[NSMutableData dataWithLength:payload.length];
        [v readFromFile:file offset:0 length:payload.length intoBuffer:(FSMutableFileDataBuffer *)buffer
            replyHandler:^(size_t n,NSError *e){CHECK(!e && n==payload.length && [buffer.data isEqual:payload]);}];
        [v lookupItemNamed:[FSFileName nameWithString:name] inDirectory:dir replyHandler:^(FSItem *found,FSFileName *,NSError *e){CHECK(!e && found==file);}];
    }
    CHECK(files[1]!=files[2]);
    auto renamed=[FSFileName nameWithString:@"重命名́.txt"];
    [v renameItem:files[0] inDirectory:dir named:[FSFileName nameWithString:@"源代码🧪.txt"] toNewName:renamed inDirectory:dir overItem:nil
        replyHandler:^(FSFileName *actual,NSError *e){CHECK(!e && [actual.data isEqual:renamed.data]);}];
    [v createLinkToItem:files[0] named:[FSFileName nameWithString:@"别名📎"] inDirectory:dir replyHandler:^(FSFileName *,NSError *e){CHECK(!e);}];
    __block FSItem *link=nil;
    [v createSymbolicLinkNamed:[FSFileName nameWithString:@"链接🔗"] inDirectory:dir attributes:[FSItemSetAttributesRequest new] linkContents:renamed
        replyHandler:^(FSItem *item,FSFileName *,NSError *e){CHECK(!e && item);link=item;}];
    [v readSymbolicLink:link replyHandler:^(FSFileName *target,NSError *e){CHECK(!e && [target.data isEqual:renamed.data]);}];
    // Reclaim retained regular handles before the independent 256-handle test.
    for(FSItem *file in files)[v reclaimItem:file replyHandler:^(NSError *e){CHECK(!e);}];
    [v reclaimItem:link replyHandler:^(NSError *e){CHECK(!e);}];
    [v reclaimItem:dir replyHandler:^(NSError *e){CHECK(!e);}];
}
int main(int argc,char **argv){@autoreleasepool{try{
    CHECK(argc==3 || argc==4);
    const bool paginationMode=std::string(argv[1])=="--pagination-config";
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
        CHECK(std::string(argv[1])=="--test-config" || paginationMode);
        std::ifstream input(argv[2]);forkfs::MountCredentials config{};unsigned uid;int ro;
        input>>config.socket>>uid>>config.generation>>config.session>>config.name>>ro;CHECK(input.good());
        config.service_uid=uid;config.read_only=ro;config.lease_ms=3000;c=config;
        NSError *error=nil;volume=[[ForkRevisionVolume alloc] initWithMountCredentials:*c error:&error];CHECK(volume && !error);
    }
    auto root=volume.rootItem;CHECK(root);
    if(paginationMode){
        CHECK(c->read_only);
        FSDirectoryCookie cookie=FSDirectoryCookieInitial;__block FSDirectoryVerifier verifier=FSDirectoryVerifierInitial;
        NSMutableArray<NSString *> *names=[NSMutableArray new];NSMutableArray<NSNumber *> *cookies=[NSMutableArray new];
        bool eof=false;
        for(unsigned call=0;call<200 && !eof;++call){
            auto packer=[PaginationPacker new];__block unsigned replies=0;
            [volume enumerateDirectory:root startingAtCookie:cookie verifier:verifier providingAttributes:[FSItemGetAttributesRequest new]
                usingPacker:(FSDirectoryEntryPacker *)packer replyHandler:^(FSDirectoryVerifier next,NSError *e){++replies;CHECK(!e && next!=FSDirectoryVerifierInitial);verifier=next;}];
            CHECK(replies==1 && packer.attributeCount==packer.names.count);
            [names addObjectsFromArray:packer.names];[cookies addObjectsFromArray:packer.cookies];
            if(packer.names.count){cookie=packer.cookies.lastObject.unsignedLongLongValue;}
            else eof=true;
        }
        CHECK(eof && names.count==4097 && cookies.count==4097);
        NSMutableArray<NSString *> *expected=[NSMutableArray new];for(unsigned i=0;i<4097;++i)[expected addObject:[NSString stringWithFormat:@"file-%05u",i]];
        CHECK([names isEqualToArray:expected]);
        for(unsigned i=0;i<4097;++i)CHECK(cookies[i].unsignedLongLongValue==uint64_t(i+1));
        CHECK(![volume revoke]);std::cout<<"OK"<<std::endl;return 0;
    }
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
    char invalid[]={'a','\0','b'};
    [volume createItemNamed:[FSFileName nameWithBytes:invalid length:sizeof(invalid)] type:FSItemTypeFile inDirectory:root attributes:[FSItemSetAttributesRequest new]
        replyHandler:^(FSItem *created,FSFileName *,NSError *e){CHECK(!created && e.code==EINVAL);}];
    // NUL is rejected locally; quote/backslash and Unicode names remain exact.
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
    unicodeCallbacks(volume,root,data);
    // Cursor copies share one pinned server snapshot and can replay old cookies.
    auto transport=std::make_shared<forkfs::MountTransport>(*c);forkfs::VolumeView view(transport);
    __block FSItem *removedDirectory=nil;__block unsigned directoryReplies=0;
    [volume createItemNamed:[FSFileName nameWithString:@"removed-enum"] type:FSItemTypeDirectory inDirectory:root attributes:[FSItemSetAttributesRequest new]
        replyHandler:^(FSItem *created,FSFileName *,NSError *e){++directoryReplies;CHECK(!e && created);removedDirectory=created;}];CHECK(directoryReplies==1 && removedDirectory);
    const auto rootInode=view.root_inode().id;
    const auto removedInode=view.lookup_child(rootInode,"removed-enum");
    directoryReplies=0;
    [volume removeItem:removedDirectory named:[FSFileName nameWithString:@"removed-enum"] fromDirectory:root
        replyHandler:^(NSError *e){++directoryReplies;CHECK(!e);}];CHECK(directoryReplies==1);
    bool missingOpen=false;try{view.open_directory_inode(removedInode.id);}
    catch(const std::system_error& e){missingOpen=e.code().value()==ENOENT;}CHECK(missingOpen && transport->healthy());
    CHECK(view.root_inode().directory);
    __block unsigned enumReplies=0;
    [volume enumerateDirectory:removedDirectory startingAtCookie:FSDirectoryCookieInitial verifier:FSDirectoryVerifierInitial
        providingAttributes:[FSItemGetAttributesRequest new] usingPacker:(FSDirectoryEntryPacker *)[PaginationPacker new]
        replyHandler:^(FSDirectoryVerifier,NSError *e){++enumReplies;CHECK(e.code==ENOENT);}];CHECK(enumReplies==1);
    calls=0;
    [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:item replyHandler:^(FSItemAttributes *a,NSError *e){++calls;CHECK(!e && a.size==sizeof(payload));}];CHECK(calls==1);
    std::vector<forkfs::VolumeView::DirectoryCursor> directoryCursors;directoryCursors.reserve(64);
    for(unsigned i=0;i<64;++i)directoryCursors.push_back(view.open_directory_inode(rootInode));
    bool cursorLimit=false;try{view.open_directory_inode(rootInode);}
    catch(const std::system_error& e){cursorLimit=e.code().value()==EMFILE;}CHECK(cursorLimit && transport->healthy());
    directoryCursors.erase(directoryCursors.begin());
    directoryCursors.push_back(view.open_directory_inode(rootInode));
    directoryCursors.clear();CHECK(transport->healthy());
    const auto quotaRoot=view.root_inode().id;
    // The initial escaped-name file owns one descriptor. Fill the session to
    // its 256-handle limit through real FSKit create callbacks.
    std::vector<FSItem *> quotaItems;quotaItems.reserve(255);
    for(unsigned i=0;i<255;++i){
        __block FSItem *created=nil;__block unsigned replies=0;
        auto component=[NSString stringWithFormat:@"quota-%u",i];
        [volume createItemNamed:[FSFileName nameWithString:component] type:FSItemTypeFile inDirectory:root attributes:[FSItemSetAttributesRequest new]
            replyHandler:^(FSItem *value,FSFileName *,NSError *e){++replies;CHECK(!e && value);created=value;}];
        CHECK(replies==1 && created);quotaItems.push_back(created);
    }
    const auto quotaZero=view.lookup_child(quotaRoot,"quota-0");
    const auto quotaOne=view.lookup_child(quotaRoot,"quota-1");
    __block unsigned quotaReplies=0;
    [volume createItemNamed:[FSFileName nameWithString:@"quota-rejected"] type:FSItemTypeFile inDirectory:root attributes:[FSItemSetAttributesRequest new]
        replyHandler:^(FSItem *created,FSFileName *,NSError *e){++quotaReplies;CHECK(!created && e.code==EMFILE);}];CHECK(quotaReplies==1);
    __block FSItem *missing=nil;quotaReplies=0;
    [volume lookupItemNamed:[FSFileName nameWithString:@"quota-rejected"] inDirectory:root
        replyHandler:^(FSItem *found,FSFileName *,NSError *e){++quotaReplies;CHECK(!found && e.code==ENOENT);missing=found;}];CHECK(quotaReplies==1 && !missing);
    bool openRejected=false;try{view.open_inode(quotaZero.id,forkfs::Namespace::Access::Read);}
    catch(const std::system_error& e){openRejected=e.code().value()==EMFILE;}CHECK(openRejected && transport->healthy());
    __block unsigned reclaimReplies=0;
    [volume reclaimItem:quotaItems.front() replyHandler:^(NSError *e){++reclaimReplies;CHECK(!e);}];CHECK(reclaimReplies==1);
    auto recovered=view.open_inode(quotaOne.id,forkfs::Namespace::Access::Read);view.close(recovered);
    __block FSItem *recoveredItem=nil;quotaReplies=0;
    [volume createItemNamed:[FSFileName nameWithString:@"quota-recovered"] type:FSItemTypeFile inDirectory:root attributes:[FSItemSetAttributesRequest new]
        replyHandler:^(FSItem *created,FSFileName *,NSError *e){++quotaReplies;CHECK(!e && created);recoveredItem=created;}];CHECK(quotaReplies==1 && recoveredItem);
    __block FSItem *lookupBlocked=nil;quotaReplies=0;
    [volume lookupItemNamed:[FSFileName nameWithString:@"quota-0"] inDirectory:root
        replyHandler:^(FSItem *found,FSFileName *,NSError *e){++quotaReplies;CHECK(!found && e.code==EMFILE);lookupBlocked=found;}];CHECK(quotaReplies==1 && !lookupBlocked);
    reclaimReplies=0;
    [volume reclaimItem:quotaItems[1] replyHandler:^(NSError *e){++reclaimReplies;CHECK(!e);}];CHECK(reclaimReplies==1);
    __block FSItem *lookupRecovered=nil;quotaReplies=0;
    [volume lookupItemNamed:[FSFileName nameWithString:@"quota-0"] inDirectory:root
        replyHandler:^(FSItem *found,FSFileName *,NSError *e){++quotaReplies;CHECK(!e && found);lookupRecovered=found;}];CHECK(quotaReplies==1 && lookupRecovered);
    __block FSItem *lookedUp=nil;quotaReplies=0;
    [volume lookupItemNamed:[FSFileName nameWithString:@"quota-recovered"] inDirectory:root
        replyHandler:^(FSItem *found,FSFileName *,NSError *e){++quotaReplies;CHECK(!e && found);lookedUp=found;}];CHECK(quotaReplies==1 && lookedUp);
    char recoveredByte='R';NSData *recoveredData=[NSData dataWithBytes:&recoveredByte length:1];quotaReplies=0;
    [volume writeContents:recoveredData toFile:lookedUp atOffset:0 replyHandler:^(size_t n,NSError *e){++quotaReplies;CHECK(!e && n==1);}];CHECK(quotaReplies==1);
    auto quotaBuffer=[TransportBuffer new];quotaBuffer.data=[NSMutableData dataWithLength:1];quotaReplies=0;
    [volume readFromFile:lookedUp offset:0 length:1 intoBuffer:(FSMutableFileDataBuffer *)quotaBuffer
        replyHandler:^(size_t n,NSError *e){++quotaReplies;CHECK(!e && n==1);}];
    CHECK(quotaReplies==1 && ((const char *)quotaBuffer.mutableBytes)[0]=='R' && transport->healthy());
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
