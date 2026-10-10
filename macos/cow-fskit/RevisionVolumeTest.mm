#import "RevisionVolume.h"
#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <fstream>
extern NSString *ForkRevisionResourceName(NSURL *url);
extern NSUUID *ForkRevisionProbeIdentity(NSURL *url,NSString *revision);
#include <unistd.h>
// Recording test double for FSKit's nonconstructible system packer.
@interface RevisionRecordingPacker : NSObject
@property(nonatomic) NSUInteger capacity;
@property(nonatomic,strong) NSMutableArray<NSString *> *names;
@property(nonatomic,strong) NSMutableArray<NSNumber *> *ids;
@property(nonatomic,strong) NSMutableArray<NSNumber *> *cookies;
@property(nonatomic) BOOL hasAttributes;
@end
@implementation RevisionRecordingPacker
- (instancetype)init {if((self=[super init])){_names=[NSMutableArray new];_ids=[NSMutableArray new];_cookies=[NSMutableArray new];_capacity=1;}return self;}
- (BOOL)packEntryWithName:(FSFileName *)name itemType:(FSItemType)type itemID:(FSItemID)itemID nextCookie:(FSDirectoryCookie)cookie attributes:(FSItemAttributes *)attributes {
    if(self.names.count==self.capacity)return NO;
    [self.names addObject:name.string];[self.ids addObject:@(itemID)];[self.cookies addObject:@(cookie)];self.hasAttributes=attributes!=nil;return YES;
}
@end
void check(bool ok){if(!ok)throw std::runtime_error("FSKit callback regression failed");}
int main(){@autoreleasepool {
    auto pattern=(std::filesystem::temp_directory_path()/"ff-fskit-XXXXXX").string();auto dir=mkdtemp(pattern.data());if(!dir)return 1;
    try {
        auto path=std::string(dir)+"/store";forkfs::Container::create(path);auto store=std::make_shared<forkfs::Container>(path);
        forkfs::Namespace fs(*store);fs.initialize();fs.write("/a",{1,2});fs.link("/a","/b");fs.symlink("a","/link");fs.mkdir("/dir");fs.snapshot("base");
        auto resource=[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()] isDirectory:YES];
        auto marker=path+"/.forkfs-fskit-revision";
        check([ForkRevisionProbeIdentity(resource,@"base") isEqual:ForkRevisionProbeIdentity(resource,@"base")]);
        check(![ForkRevisionProbeIdentity(resource,@"base") isEqual:ForkRevisionProbeIdentity(resource,@"other")]);
        check(!ForkRevisionResourceName(resource));
        {std::ofstream out(marker);out<<"base\n";}check([ForkRevisionResourceName(resource) isEqualToString:@"base"]);
        {std::ofstream out(marker);out<<"../base";}check(!ForkRevisionResourceName(resource));
        {std::ofstream out(marker);out<<std::string(66,'a');}check(!ForkRevisionResourceName(resource));
        std::filesystem::remove(marker);std::filesystem::create_symlink("CURRENT",marker);check(!ForkRevisionResourceName(resource));
        std::filesystem::remove(marker);
        NSError *error=nil;auto volume=[[ForkRevisionVolume alloc] initWithStore:store revision:@"base" error:&error];check(volume && !error);
        FSItem *root=volume.rootItem;check(root);
        __block FSItem *a=nil,*b=nil,*link=nil;
        [volume lookupItemNamed:[FSFileName nameWithString:@"a"] inDirectory:root replyHandler:^(FSItem *item,FSFileName *name,NSError *e){check(!e);a=item;}];
        [volume lookupItemNamed:[FSFileName nameWithString:@"b"] inDirectory:root replyHandler:^(FSItem *item,FSFileName *name,NSError *e){check(!e);b=item;}];check(a && a==b);
        [volume lookupItemNamed:[FSFileName nameWithString:@"missing"] inDirectory:root replyHandler:^(FSItem *item,FSFileName *name,NSError *e){check(!item && e.code==ENOENT);}];
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:a replyHandler:^(FSItemAttributes *attrs,NSError *e){check(!e && attrs.size==2 && attrs.linkCount==2 && attrs.inhibitKernelOffloadedIO);}];
        [volume lookupItemNamed:[FSFileName nameWithString:@"link"] inDirectory:root replyHandler:^(FSItem *item,FSFileName *name,NSError *e){check(!e);link=item;}];
        [volume readSymbolicLink:link replyHandler:^(FSFileName *name,NSError *e){check(!e && [name.string isEqualToString:@"a"]);}];
        [volume writeContents:[NSData data] toFile:a atOffset:0 replyHandler:^(size_t n,NSError *e){check(n==0 && e.code==EROFS);}];
        for(bool attributes : {false,true}) {
            FSDirectoryCookie cookie=FSDirectoryCookieInitial;__block FSDirectoryVerifier verifier=FSDirectoryVerifierInitial;
            auto names=[NSMutableArray<NSString *> new];auto ids=[NSMutableArray<NSNumber *> new];
            for(unsigned call=0;call<10;++call){auto packer=[RevisionRecordingPacker new];
                [volume enumerateDirectory:root startingAtCookie:cookie verifier:verifier providingAttributes:attributes?[FSItemGetAttributesRequest new]:nil
                    usingPacker:(FSDirectoryEntryPacker *)packer replyHandler:^(FSDirectoryVerifier v,NSError *e){check(!e && v!=FSDirectoryVerifierInitial);verifier=v;}];
                if(packer.names.count==0)break;check(packer.hasAttributes==attributes);
                [names addObjectsFromArray:packer.names];[ids addObjectsFromArray:packer.ids];cookie=packer.cookies.lastObject.unsignedLongLongValue;
            }
            check([names isEqualToArray:attributes?@[@"a",@"b",@"dir",@"link"]:@[@".",@"..",@"a",@"b",@"dir",@"link"]]);
            if(!attributes)check([ids[0] isEqual:ids[1]] && ids[0].unsignedLongLongValue==FSItemIDRootDirectory);
            auto bad=[RevisionRecordingPacker new];
            [volume enumerateDirectory:root startingAtCookie:1 verifier:attributes?2:1 providingAttributes:attributes?[FSItemGetAttributesRequest new]:nil
                usingPacker:(FSDirectoryEntryPacker *)bad replyHandler:^(FSDirectoryVerifier v,NSError *e){check(e.code==FSErrorInvalidDirectoryCookie);}];
        }
        __block FSItem *directory=nil;
        [volume lookupItemNamed:[FSFileName nameWithString:@"dir"] inDirectory:root replyHandler:^(FSItem *item,FSFileName *name,NSError *e){check(!e);directory=item;}];
        auto dots=[RevisionRecordingPacker new];dots.capacity=2;
        [volume enumerateDirectory:directory startingAtCookie:FSDirectoryCookieInitial verifier:FSDirectoryVerifierInitial providingAttributes:nil usingPacker:(FSDirectoryEntryPacker *)dots
            replyHandler:^(FSDirectoryVerifier v,NSError *e){check(!e && v!=FSDirectoryVerifierInitial);}];
        check([dots.names isEqualToArray:@[@".",@".."]] && dots.ids[1].unsignedLongLongValue==FSItemIDRootDirectory && ![dots.ids[0] isEqual:dots.ids[1]]);
        fs.write("/a",{3});[volume getAttributes:[FSItemGetAttributesRequest new] ofItem:a replyHandler:^(FSItemAttributes *attrs,NSError *e){check(!e && attrs.size==2);}];
        check(!volume.supportedVolumeCapabilities.supportsPersistentObjectIDs);store->verify();
        [volume revoke];check(!volume.rootItem);
        [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:a replyHandler:^(FSItemAttributes *attrs,NSError *e){check(!attrs && e.code==ENOTCONN);}];
        [volume lookupItemNamed:[FSFileName nameWithString:@"a"] inDirectory:root replyHandler:^(FSItem *item,FSFileName *name,NSError *e){check(!item && e.code==ENOTCONN);}];
        [volume revoke];
        volume=nil;store.reset();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(dir);return 1;}
    std::filesystem::remove_all(dir);return 0;
}}
