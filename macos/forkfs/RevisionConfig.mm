#import "RevisionConfig.h"
#import <CommonCrypto/CommonDigest.h>
#include "rpc.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

NSDictionary* RevisionLoadDescriptor(NSURL* url,NSError** error) {
    auto require=[](bool ok,int code,const char* message){if(!ok)throw forkfs::RpcError(code,message);};
    try {
        struct stat st{};require(lstat(url.path.fileSystemRepresentation,&st)==0 && S_ISDIR(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&0077),EACCES,"runtime directory must be private and owned");
        NSString* path=[url.path stringByAppendingPathComponent:@"forkfs.json"];
        int fd=open(path.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);require(fd>=0,EINVAL,"mount descriptor missing");
        struct Guard{int fd;~Guard(){close(fd);}}guard{fd};
        require(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&0077) && st.st_size>0 && st.st_size<=16384,EACCES,"invalid mount descriptor");
        NSMutableData* data=[NSMutableData dataWithLength:static_cast<NSUInteger>(st.st_size)];size_t at=0;
        while(at<data.length){auto n=read(fd,static_cast<char*>(data.mutableBytes)+at,data.length-at);if(n<0 && errno==EINTR)continue;require(n>0,EIO,"cannot read mount descriptor");at+=n;}
        id config=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        require([config isKindOfClass:NSDictionary.class],EINVAL,"invalid mount descriptor JSON");
        require([config[@"schema_version"] isEqual:@1],EINVAL,"unsupported mount descriptor");
        id view=config[config[@"revision"]!=nil?@"revision":@"world"],repository=config[@"repository"];
        require([view isKindOfClass:NSString.class] && [view length]>0 && (config[@"world"]==nil || config[@"revision"]==nil),EINVAL,"select exactly one view");
        require([repository isKindOfClass:NSString.class] && [repository length]==32,EINVAL,"invalid repository identity");
        return config;
    }catch(const forkfs::RpcError& e){if(error)*error=[NSError errorWithDomain:NSPOSIXErrorDomain code:e.code userInfo:@{NSLocalizedDescriptionKey:@(e.what())}];return nil;}
    catch(const std::exception&){if(error)*error=[NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];return nil;}
}
NSUUID* RevisionDescriptorUUID(NSDictionary* config) {
    NSString* identity=[NSString stringWithFormat:@"forkfs.v1:%@:%@:%@",config[@"repository"],config[@"revision"]?@"revision":@"world",config[config[@"revision"]?@"revision":@"world"]];
    NSData* data=[identity dataUsingEncoding:NSUTF8StringEncoding];unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes,static_cast<CC_LONG>(data.length),hash);hash[6]=(hash[6]&15)|0x50;hash[8]=(hash[8]&63)|0x80;
    return [[NSUUID alloc] initWithUUIDBytes:hash];
}
