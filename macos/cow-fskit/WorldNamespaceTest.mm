#import "RevisionVolume.h"
#include "namespace.h"
#include <filesystem>
#include <iostream>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#define CHECK(x) do{if(!(x))throw std::runtime_error("World namespace failure at "+std::to_string(__LINE__));}while(0)
@interface NamespacePacker : NSObject
@property(nonatomic) NSUInteger capacity;
@property(nonatomic,strong) NSMutableArray<NSString *> *names;
@property(nonatomic,strong) NSMutableArray<NSNumber *> *cookies;
@property(nonatomic,strong) NSMutableArray<NSNumber *> *ids;
@property(nonatomic,strong) NSMutableArray<FSItemAttributes *> *attrs;
@end
@implementation NamespacePacker
- (instancetype)init {if((self=[super init])){_capacity=1000;_names=[NSMutableArray new];_cookies=[NSMutableArray new];_ids=[NSMutableArray new];_attrs=[NSMutableArray new];}return self;}
- (BOOL)packEntryWithName:(FSFileName *)name itemType:(FSItemType)type itemID:(FSItemID)number nextCookie:(FSDirectoryCookie)cookie attributes:(FSItemAttributes *)attrs {
    if(self.names.count==self.capacity)return NO;
    [self.names addObject:name.string];[self.cookies addObject:@(cookie)];[self.ids addObject:@(number)];if(attrs)[self.attrs addObject:attrs];return YES;
}
@end
static FSFileName *name(NSString *s){return [FSFileName nameWithString:s];}
static FSItem *lookup(ForkRevisionVolume *v,FSItem *parent,NSString *n,int error=0){
    __block FSItem *out=nil;__block unsigned count=0;
    [v lookupItemNamed:name(n) inDirectory:parent replyHandler:^(FSItem *item,FSFileName *,NSError *e){++count;CHECK(e.code==error && (item!=nil)==(error==0));out=item;}];CHECK(count==1);return out;
}
static FSItem *create(ForkRevisionVolume *v,FSItem *parent,NSString *n,FSItemType type,FSItemSetAttributesRequest *a=nil,int error=0){
    __block FSItem *out=nil;__block unsigned count=0;
    [v createItemNamed:name(n) type:type inDirectory:parent attributes:a?:[FSItemSetAttributesRequest new] replyHandler:^(FSItem *item,FSFileName *actual,NSError *e){++count;CHECK(e.code==error && (item!=nil)==(error==0));if(!error)CHECK([actual.string isEqual:n]);out=item;}];CHECK(count==1);return out;
}
static void remove(ForkRevisionVolume *v,FSItem *parent,NSString *n,FSItem *item,int error=0){
    __block unsigned count=0;[v removeItem:item named:name(n) fromDirectory:parent replyHandler:^(NSError *e){++count;CHECK(e.code==error);}];CHECK(count==1);
}
static void rename(ForkRevisionVolume *v,FSItem *item,FSItem *src,NSString *old,FSItem *dst,NSString *next,FSItem *over=nil,int error=0){
    __block unsigned count=0;[v renameItem:item inDirectory:src named:name(old) toNewName:name(next) inDirectory:dst overItem:over replyHandler:^(FSFileName *actual,NSError *e){++count;CHECK(e.code==error);if(!error)CHECK([actual.string isEqual:next]);}];CHECK(count==1);
}
static FSItemAttributes *attrs(ForkRevisionVolume *v,FSItem *item,int error=0){
    __block FSItemAttributes *out=nil;__block unsigned count=0;
    [v getAttributes:[FSItemGetAttributesRequest new] ofItem:item replyHandler:^(FSItemAttributes *a,NSError *e){++count;CHECK(e.code==error && (a!=nil)==(error==0));out=a;}];CHECK(count==1);return out;
}
static NamespacePacker *page(ForkRevisionVolume *v,FSItem *dir,FSDirectoryCookie cookie,FSDirectoryVerifier& verifier,NSUInteger capacity,bool attributes=true,int error=0){
    auto p=[NamespacePacker new];p.capacity=capacity;__block unsigned count=0;__block FSDirectoryVerifier next=verifier;
    [v enumerateDirectory:dir startingAtCookie:cookie verifier:verifier providingAttributes:attributes?[FSItemGetAttributesRequest new]:nil usingPacker:(FSDirectoryEntryPacker *)p replyHandler:^(FSDirectoryVerifier returned,NSError *e){++count;CHECK(e.code==error);next=returned;}];CHECK(count==1);verifier=next;return p;
}
int main(){@autoreleasepool{
    auto pattern=(std::filesystem::temp_directory_path()/"ff-world-ns-XXXXXX").string();auto dir=mkdtemp(pattern.data());if(!dir)return 1;
    try{
        auto path=std::string(dir)+"/db";forkfs::Container::create(path);
        {
            auto store=std::make_shared<forkfs::Container>(path);forkfs::Namespace main(*store);main.initialize();main.snapshot("base");main.fork("base","work");forkfs::Namespace fs(*store,"work");
            NSError *error=nil;auto v=[[ForkRevisionVolume alloc] initWithStore:store world:@"work" error:&error];CHECK(v && !error);FSItem *root=v.rootItem;
            auto request=[FSItemSetAttributesRequest new];request.type=FSItemTypeFile;request.mode=0600;request.size=4;request.uid=501;request.gid=20;request.birthTime=(struct timespec){1,2};
            auto a=create(v,root,@"a",FSItemTypeFile,request);CHECK(attrs(v,a).size==4 && attrs(v,a).uid==501 && attrs(v,a).birthTime.tv_nsec==2);
            CHECK([request wasAttributeConsumed:FSItemAttributeType] && [request wasAttributeConsumed:FSItemAttributeUID]);
            auto stable=store->status();
            create(v,root,@"a",FSItemTypeFile,nil,EEXIST);
            create(v,a,@"bad",FSItemTypeFile,nil,ENOTDIR);
            auto bad=[FSItemSetAttributesRequest new];bad.size=262145;create(v,root,@"big",FSItemTypeFile,bad,EFBIG);CHECK(bad.consumedAttributes==0);
            bad=[FSItemSetAttributesRequest new];bad.uid=42;bad.flags=1;create(v,root,@"flags",FSItemTypeFile,bad,ENOTSUP);
            bad=[FSItemSetAttributesRequest new];bad.size=0;create(v,root,@"dirsize",FSItemTypeDirectory,bad,EISDIR);
            bad=[FSItemSetAttributesRequest new];bad.type=FSItemTypeDirectory;create(v,root,@"type",FSItemTypeFile,bad,EINVAL);
            create(v,root,@"../bad",FSItemTypeFile,nil,EINVAL);
            create(v,root,[@"" stringByPaddingToLength:256 withString:@"x" startingAtIndex:0],FSItemTypeFile,nil,ENAMETOOLONG);
            CHECK(store->status()==stable);
            auto d=create(v,root,@"d",FSItemTypeDirectory),e=create(v,root,@"e",FSItemTypeDirectory);
            CHECK(attrs(v,root).linkCount==4);
            __block unsigned calls=0;
            [v createLinkToItem:a named:name(@"alias") inDirectory:root replyHandler:^(FSFileName *,NSError *err){++calls;CHECK(!err);}];CHECK(calls==1 && lookup(v,root,@"alias")==a && attrs(v,a).linkCount==2);
            calls=0;[v createLinkToItem:d named:name(@"badlink") inDirectory:root replyHandler:^(FSFileName *,NSError *err){++calls;CHECK(err.code==ENOTSUP);}];CHECK(calls==1);
            __block FSItem *link=nil;calls=0;auto sa=[FSItemSetAttributesRequest new];sa.uid=42;
            [v createSymbolicLinkNamed:name(@"sym") inDirectory:root attributes:sa linkContents:name(@"a") replyHandler:^(FSItem *it,FSFileName *,NSError *err){++calls;CHECK(!err);link=it;}];CHECK(calls==1 && link && attrs(v,link).uid==42);
            calls=0;[v readSymbolicLink:link replyHandler:^(FSFileName *target,NSError *err){++calls;CHECK(!err && [target.string isEqual:@"a"]);}];CHECK(calls==1);
            // Wrong inode arguments never delete or replace the current entry.
            stable=store->status();remove(v,root,@"a",link,ESTALE);rename(v,link,root,@"a",root,@"new",nil,ESTALE);CHECK(store->status()==stable);
            auto b=create(v,root,@"b",FSItemTypeFile);unsigned char byte=9;
            calls=0;[v writeContents:[NSData dataWithBytes:&byte length:1] toFile:b atOffset:0 replyHandler:^(size_t n,NSError *err){++calls;CHECK(!err && n==1);}];CHECK(calls==1);
            stable=store->status();rename(v,a,root,@"a",root,@"b",nil,ESTALE);CHECK(store->status()==stable);
            rename(v,a,root,@"a",root,@"b",b);CHECK(lookup(v,root,@"b")==a && attrs(v,b).linkCount==0 && attrs(v,b).size==1);
            remove(v,root,@"alias",a);CHECK(attrs(v,a).linkCount==1);
            auto child=create(v,d,@"child",FSItemTypeDirectory);remove(v,root,@"d",d,ENOTEMPTY);
            rename(v,d,root,@"d",child,@"cycle",nil,EINVAL);
            rename(v,child,d,@"child",e,@"moved");CHECK(lookup(v,child,@"..")==e && attrs(v,child).parentID==attrs(v,e).fileID);
            remove(v,root,@"d",d);remove(v,e,@"moved",child);
            // Snapshot pagination retains removed, never-looked-up inode entries.
            auto listing=create(v,root,@"listing",FSItemTypeDirectory);
            fs.write("/listing/a",{1});fs.write("/listing/b",{2});fs.write("/listing/c",{3});
            FSDirectoryVerifier verifier=FSDirectoryVerifierInitial;
            auto first=page(v,listing,FSDirectoryCookieInitial,verifier,1);CHECK([first.names isEqual:@[@"a"]]);auto original=verifier;
            fs.remove("/listing/b",false);fs.write("/listing/c",{3,3});fs.write("/listing/d",{4});fs.compact();
            auto rest=page(v,listing,first.cookies.lastObject.unsignedLongLongValue,verifier,100);CHECK([rest.names isEqual:(@[@"b",@"c"])] && rest.attrs[1].size==1);
            auto repeat=page(v,listing,first.cookies.lastObject.unsignedLongLongValue,verifier,100);CHECK([repeat.names isEqual:rest.names] && [repeat.ids isEqual:rest.ids]);
            auto eof=page(v,listing,rest.cookies.lastObject.unsignedLongLongValue,verifier,10);CHECK(eof.names.count==0);
            page(v,listing,999,verifier,1,true,FSErrorInvalidDirectoryCookie);
            verifier=FSDirectoryVerifierInitial;auto refreshed=page(v,listing,FSDirectoryCookieInitial,verifier,100);CHECK([refreshed.names isEqual:(@[@"a",@"c",@"d"])] && refreshed.attrs[1].size==2);
            page(v,listing,1,original,1,true,FSErrorInvalidDirectoryCookie);
            // A rejected pack entry is retryable at the initial cookie/verifier.
            verifier=FSDirectoryVerifierInitial;CHECK(page(v,listing,FSDirectoryCookieInitial,verifier,0).names.count==0);
            fs.write("/listing/e",{5});auto retry=page(v,listing,FSDirectoryCookieInitial,verifier,100);CHECK([retry.names isEqual:refreshed.names]);
            auto wrongMode=verifier;page(v,listing,1,wrongMode,1,false,FSErrorInvalidDirectoryCookie);
            verifier=FSDirectoryVerifierInitial;auto dots=page(v,listing,FSDirectoryCookieInitial,verifier,2,false);CHECK([dots.names isEqual:(@[@".",@".."]) ] && dots.ids[1].unsignedLongLongValue==FSItemIDRootDirectory);
            auto names=page(v,listing,2,verifier,100,false);CHECK([names.names isEqual:(@[@"a",@"c",@"d",@"e"])] && names.attrs.count==0);
            // Cross the backend's 128-entry batch boundary.
            auto large=create(v,root,@"large",FSItemTypeDirectory);
            for(unsigned i=0;i<130;++i){char n[16];snprintf(n,sizeof(n),"n%03u",i);fs.create_child(fs.stat("/large").id,n,false,0644);}
            verifier=FSDirectoryVerifierInitial;auto prefix=page(v,large,FSDirectoryCookieInitial,verifier,127);CHECK(prefix.names.count==127);
            auto suffix=page(v,large,prefix.cookies.lastObject.unsignedLongLongValue,verifier,100);CHECK([suffix.names isEqual:(@[@"n127",@"n128",@"n129"])]);
            // Bounded cursor eviction returns an explicit invalid-cookie result.
            auto evicted=verifier;
            for(unsigned i=0;i<129;++i){NSString *n=[NSString stringWithFormat:@"cursor%u",i];auto folder=create(v,root,n,FSItemTypeDirectory);FSDirectoryVerifier fresh=FSDirectoryVerifierInitial;page(v,folder,FSDirectoryCookieInitial,fresh,1);}
            page(v,large,127,evicted,1,true,FSErrorInvalidDirectoryCookie);
            CHECK(main.list("/").empty() && forkfs::Namespace(*store,"base",true).list("/").empty());
            auto revision=[[ForkRevisionVolume alloc] initWithStore:store revision:@"base" error:&error];CHECK(revision);
            create(revision,revision.rootItem,@"ro",FSItemTypeFile,nil,EROFS);
            create(v,revision.rootItem,@"foreign",FSItemTypeFile,nil,ESTALE);
            CHECK([revision revoke]==nil);CHECK([v revoke]==nil);remove(v,root,@"b",a,ENOTCONN);fs.verify();store->verify();
        }
        {forkfs::Container store(path);forkfs::Namespace fs(store,"work");CHECK(fs.read("/b")==std::vector<unsigned char>(4,0));CHECK(fs.readlink("/sym")=="a");fs.verify();store.verify();}
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(dir);return 1;}
    std::filesystem::remove_all(dir);return 0;
}}
