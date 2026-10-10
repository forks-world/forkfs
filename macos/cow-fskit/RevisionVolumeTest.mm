#import "RevisionVolume.h"
#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>
void check(bool ok){if(!ok)throw std::runtime_error("FSKit callback regression failed");}
int main(){@autoreleasepool {
    auto pattern=(std::filesystem::temp_directory_path()/"ff-fskit-XXXXXX").string();auto dir=mkdtemp(pattern.data());if(!dir)return 1;
    try {
        auto path=std::string(dir)+"/store";forkfs::Container::create(path);auto store=std::make_shared<forkfs::Container>(path);
        forkfs::Namespace fs(*store);fs.initialize();fs.write("/a",{1,2});fs.link("/a","/b");fs.symlink("a","/link");fs.snapshot("base");
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
        fs.write("/a",{3});[volume getAttributes:[FSItemGetAttributesRequest new] ofItem:a replyHandler:^(FSItemAttributes *attrs,NSError *e){check(!e && attrs.size==2);}];
        check(!volume.supportedVolumeCapabilities.supportsPersistentObjectIDs);store->verify();
        volume=nil;store.reset();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(dir);return 1;}
    std::filesystem::remove_all(dir);return 0;
}}
