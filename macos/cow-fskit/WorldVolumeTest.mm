#import "RevisionVolume.h"
#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <cstring>
#include <unistd.h>
#define CHECK(value) do {if(!(value))throw std::runtime_error("World callback failure at line "+std::to_string(__LINE__));}while(0)
@interface WorldBuffer : NSObject
@property(nonatomic,strong) NSMutableData *data;
- (NSUInteger)length;
- (void *)mutableBytes;
@end
@implementation WorldBuffer
- (NSUInteger)length {return self.data.length;}
- (void *)mutableBytes {return self.data.mutableBytes;}
@end
static FSItem *lookup(ForkRevisionVolume *volume,FSItem *root,NSString *name){
    __block FSItem *found=nil;__block unsigned calls=0;
    [volume lookupItemNamed:[FSFileName nameWithString:name] inDirectory:root replyHandler:^(FSItem *item,FSFileName *,NSError *e){++calls;CHECK(!e);found=item;}];
    CHECK(calls==1 && found);return found;
}
static FSItemAttributes *attrs(ForkRevisionVolume *volume,FSItem *item){
    __block FSItemAttributes *found=nil;__block unsigned calls=0;
    [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:item replyHandler:^(FSItemAttributes *a,NSError *e){++calls;CHECK(!e);found=a;}];
    CHECK(calls==1 && found);return found;
}
static void xattr(ForkRevisionVolume *volume,FSItem *item,NSData *data,FSSetXattrPolicy policy,int error=0){
    __block unsigned calls=0;
    [volume setXattrNamed:[FSFileName nameWithString:@"user.binary"] toData:data onItem:item policy:policy replyHandler:^(NSError *e){++calls;CHECK(e.code==error);}];CHECK(calls==1);
}
static void read(ForkRevisionVolume *volume,FSItem *item,const std::vector<unsigned char>& expected){
    auto buffer=[WorldBuffer new];buffer.data=[NSMutableData dataWithLength:16];memset(buffer.mutableBytes,0xcc,buffer.length);
    __block unsigned calls=0;
    [volume readFromFile:item offset:0 length:16 intoBuffer:(FSMutableFileDataBuffer *)buffer replyHandler:^(size_t n,NSError *e){++calls;CHECK(!e && n==expected.size());}];
    CHECK(calls==1 && !memcmp(buffer.mutableBytes,expected.data(),expected.size()));
    CHECK(((unsigned char *)buffer.mutableBytes)[expected.size()]==0xcc);
}
int main(){@autoreleasepool{
    auto pattern=(std::filesystem::temp_directory_path()/"ff-world-XXXXXX").string();auto dir=mkdtemp(pattern.data());if(!dir)return 1;
    try{
        auto path=std::string(dir)+"/db";forkfs::Container::create(path);
        {
            auto store=std::make_shared<forkfs::Container>(path);forkfs::Namespace main(*store);main.initialize();main.write("/a",{1,2});main.link("/a","/b");main.mkdir("/dir");main.symlink("a","/link");
            main.snapshot("base");main.fork("base","work");forkfs::Namespace work(*store,"work");
            NSError *error=nil;auto volume=[[ForkRevisionVolume alloc] initWithStore:store world:@"work" error:&error];CHECK(volume && !error);
            auto revision=[[ForkRevisionVolume alloc] initWithStore:store revision:@"base" error:&error];CHECK(revision && !error);
            FSItem *root=volume.rootItem,*a=lookup(volume,root,@"a"),*b=lookup(volume,root,@"b");CHECK(a==b);
            FSItem *directory=lookup(volume,root,@"dir"),*link=lookup(volume,root,@"link");
            [volume enumerateDirectory:root startingAtCookie:1 verifier:FSDirectoryVerifierInitial providingAttributes:nil
                usingPacker:(FSDirectoryEntryPacker *)[NSObject new] replyHandler:^(FSDirectoryVerifier,NSError *e){CHECK(e.code==FSErrorInvalidDirectoryCookie);}];
            [volume createItemNamed:[FSFileName nameWithString:@"new"] type:FSItemTypeSymlink inDirectory:root attributes:[FSItemSetAttributesRequest new]
                replyHandler:^(FSItem *item,FSFileName *,NSError *e){CHECK(!item && e.code==ENOTSUP);}];
            auto number=attrs(volume,a).fileID;
            auto patch=[FSItemSetAttributesRequest new];patch.size=6;patch.mode=0600;patch.uid=501;patch.gid=20;
            patch.modifyTime=(struct timespec){3,7};patch.accessTime=(struct timespec){4,8};patch.birthTime=(struct timespec){5,9};
            __block unsigned calls=0;
            [volume setAttributes:patch onItem:a replyHandler:^(FSItemAttributes *v,NSError *e){++calls;CHECK(!e && v.size==6 && v.uid==501 && v.gid==20 && v.mode==0600);}];CHECK(calls==1);
            CHECK([patch wasAttributeConsumed:FSItemAttributeSize] && [patch wasAttributeConsumed:FSItemAttributeBirthTime]);
            auto n=attrs(volume,b);CHECK(n.modifyTime.tv_sec==3 && n.modifyTime.tv_nsec==7 && n.accessTime.tv_sec==4 && n.birthTime.tv_nsec==9);
            read(volume,a,{1,2,0,0,0,0});
            unsigned char raw[]={0,255};NSData *data=[NSData dataWithBytes:raw length:2];
            [volume writeContents:data toFile:a atOffset:1 replyHandler:^(size_t count,NSError *e){CHECK(!e && count==2);}];read(volume,b,{1,0,255,0,0,0});
            xattr(volume,a,data,FSSetXattrPolicyMustCreate);
            auto before=store->status();
            xattr(volume,a,data,FSSetXattrPolicyMustCreate,EEXIST);
            xattr(volume,a,nil,FSSetXattrPolicyAlwaysSet,EINVAL);
            xattr(volume,a,data,(FSSetXattrPolicy)99,EINVAL);
            xattr(volume,a,[NSMutableData dataWithLength:65537],FSSetXattrPolicyAlwaysSet,E2BIG);
            for(unsigned which=0;which<6;++which){
                auto invalid=[FSItemSetAttributesRequest new];invalid.size=2;invalid.uid=42;
                if(which==0)invalid.accessTime=(struct timespec){0,1000000000};
                if(which==1)invalid.modifyTime=(struct timespec){-1,0};
                if(which==2)invalid.birthTime=(struct timespec){INT64_MAX,0};
                if(which==3)invalid.mode=01000;
                if(which==4)invalid.flags=1;
                if(which==5)invalid.size=262145;
                __block unsigned replies=0;
                [volume setAttributes:invalid onItem:a replyHandler:^(FSItemAttributes *v,NSError *e){++replies;CHECK(!v && e.code==(which==4?ENOTSUP:which==5?EFBIG:EINVAL));}];
                CHECK(replies==1 && invalid.consumedAttributes==0);
            }
            [volume writeContents:data toFile:a atOffset:-1 replyHandler:^(size_t count,NSError *e){CHECK(count==0 && e.code==EINVAL);}];
            [volume writeContents:data toFile:a atOffset:262144 replyHandler:^(size_t count,NSError *e){CHECK(count==0 && e.code==EFBIG);}];
            [volume writeContents:data toFile:root atOffset:0 replyHandler:^(size_t count,NSError *e){CHECK(count==0 && e.code==EISDIR);}];
            [volume writeContents:data toFile:link atOffset:0 replyHandler:^(size_t count,NSError *e){CHECK(count==0 && e.code==EINVAL);}];
            [volume writeContents:data toFile:revision.rootItem atOffset:0 replyHandler:^(size_t count,NSError *e){CHECK(count==0 && e.code==ESTALE);}];
            CHECK(store->status()==before);CHECK(attrs(volume,a).uid==501 && attrs(volume,a).size==6);read(volume,a,{1,0,255,0,0,0});
            // All inode types store xattrs; hardlinks share the regular file's value.
            for(FSItem *it in @[root,directory,link])xattr(volume,it,data,FSSetXattrPolicyAlwaysSet);
            [volume getXattrNamed:[FSFileName nameWithString:@"user.binary"] ofItem:b replyHandler:^(NSData *value,NSError *e){CHECK(!e && [value isEqualToData:data]);}];
            CHECK(main.read("/a")==std::vector<unsigned char>({1,2}));CHECK(forkfs::Namespace(*store,"base",true).read("/a")==std::vector<unsigned char>({1,2}));
            auto frozen=lookup(revision,revision.rootItem,@"a");
            [revision setAttributes:patch onItem:frozen replyHandler:^(FSItemAttributes *v,NSError *e){CHECK(!v && e.code==EROFS);}];
            xattr(revision,frozen,data,FSSetXattrPolicyAlwaysSet,EROFS);
            // Reclaim releases the backend pin; a subsequent lookup keeps numeric identity.
            [volume reclaimItem:a replyHandler:^(NSError *e){CHECK(!e);}];
            [volume getAttributes:[FSItemGetAttributesRequest new] ofItem:a replyHandler:^(FSItemAttributes *v,NSError *e){CHECK(!v && e.code==ESTALE);}];
            a=lookup(volume,root,@"b");CHECK(attrs(volume,a).fileID==number);
            [volume reclaimItem:root replyHandler:^(NSError *e){CHECK(!e);}];root=volume.rootItem;
            CHECK(lookup(volume,directory,@"..")==root && attrs(volume,directory).parentID==FSItemIDRootDirectory);
            work.remove("/a",false);work.remove("/b",false);
            CHECK(attrs(volume,a).linkCount==0);read(volume,a,{1,0,255,0,0,0});
            auto orphan=[FSItemSetAttributesRequest new];orphan.size=3;orphan.uid=0;
            [volume setAttributes:orphan onItem:a replyHandler:^(FSItemAttributes *v,NSError *e){CHECK(!e && v.size==3 && v.uid==0);}];
            xattr(volume,a,[NSData data],FSSetXattrPolicyMustReplace);
            [volume getXattrNamed:[FSFileName nameWithString:@"user.binary"] ofItem:a replyHandler:^(NSData *v,NSError *e){CHECK(!e && v && v.length==0);}];
            xattr(volume,a,nil,FSSetXattrPolicyDelete);xattr(volume,a,nil,FSSetXattrPolicyDelete,ENOATTR);
            [volume writeContents:data toFile:a atOffset:0 replyHandler:^(size_t count,NSError *e){CHECK(!e && count==2);}];read(volume,a,{0,255,255});
            CHECK([volume revoke]==nil);CHECK([volume revoke]==nil);
            [volume writeContents:data toFile:a atOffset:0 replyHandler:^(size_t count,NSError *e){CHECK(count==0 && e.code==ENOTCONN);}];
            [volume setAttributes:orphan onItem:a replyHandler:^(FSItemAttributes *v,NSError *e){CHECK(!v && e.code==ENOTCONN);}];
            xattr(volume,a,data,FSSetXattrPolicyAlwaysSet,ENOTCONN);
            [volume reclaimItem:a replyHandler:^(NSError *e){CHECK(e.code==ENOTCONN);}];
            CHECK([revision revoke]==nil);store->verify();
        }
        {
            auto store=std::make_shared<forkfs::Container>(path);forkfs::Namespace work(*store,"work");
            CHECK(work.get_inode_xattr(work.stat("/").id,"user.binary")==std::vector<unsigned char>({0,255}));
            CHECK(work.list("/")==std::vector<std::string>({"dir","link"}));work.verify();store->verify();
        }
        {
            auto preview=std::make_shared<forkfs::Container>(path,forkfs::Container::OpenMode::RevisionPreview);
            NSError *error=nil;CHECK(![[ForkRevisionVolume alloc] initWithStore:preview world:@"work" error:&error] && error.code==EROFS);
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(dir);return 1;}
    std::filesystem::remove_all(dir);return 0;
}}
