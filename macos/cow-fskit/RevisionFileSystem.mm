#import "RevisionVolume.h"
#include "metadata_view.h"
#include <fcntl.h>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>
// The only descriptor field is a bounded revision name. Repository selection
// comes exclusively from FSKit's granted path resource, never a marker pathname.
NSString *ForkRevisionResourceName(NSURL *url) {
    int directory=open(url.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(directory<0)return nil;
    int fd=openat(directory,".forkfs-fskit-revision",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);close(directory);
    if(fd<0)return nil;
    struct stat st{};char data[66];ssize_t n=-1;
    if(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size>0 && st.st_size<=65)n=read(fd,data,sizeof(data));close(fd);
    if(n<=0 || n>65 || n!=st.st_size)return nil;if(data[n-1]=='\n')--n;if(n<=0 || n>64)return nil;
    try{forkfs::NamespaceView::valid_name(std::string(data,n));}catch(const std::exception&){return nil;}
    return [[NSString alloc] initWithBytes:data length:n encoding:NSASCIIStringEncoding];
}
NSUUID *ForkRevisionProbeIdentity(NSURL *url,NSString *revision) {
    std::string input="forkfs.fskit.resource.v1";input+=url.fileSystemRepresentation;input.push_back('\0');input+=revision.UTF8String;
    // The namespace separator prevents ambiguous path/revision concatenation.
    auto hash=forkfs::Journal::object_id(std::vector<unsigned char>(input.begin(),input.end()));
    unsigned char uuid[16];std::copy_n(hash.begin(),16,uuid);uuid[6]=(uuid[6]&15)|0x80;uuid[8]=(uuid[8]&63)|0x80;
    return [[NSUUID alloc] initWithUUIDBytes:uuid];
}
@interface ForkRevisionFileSystem : FSUnaryFileSystem <FSUnaryFileSystemOperations>
@end
@implementation ForkRevisionFileSystem {
    ForkRevisionVolume *_volume;
    NSURL *_grantedURL;
}
- (void)releaseResource {
    [_volume revoke];_volume=nil;
    if(_grantedURL){[_grantedURL stopAccessingSecurityScopedResource];_grantedURL=nil;}
}
- (void)dealloc {[self releaseResource];}
- (void)probeResource:(FSResource *)resource replyHandler:(void (^)(FSProbeResult *,NSError *))reply {
    if(![resource isKindOfClass:[FSPathURLResource class]]){reply(FSProbeResult.notRecognizedProbeResult,nil);return;}
    NSURL *url=((FSPathURLResource *)resource).url;
    if(![url startAccessingSecurityScopedResource]){reply(FSProbeResult.notRecognizedProbeResult,fs_errorForPOSIXError(EPERM));return;}
    NSString *revision=ForkRevisionResourceName(url);[url stopAccessingSecurityScopedResource];
    if(!revision){reply(FSProbeResult.notRecognizedProbeResult,nil);return;}
    reply([FSProbeResult usableProbeResultWithName:revision containerID:[[FSContainerIdentifier alloc] initWithUUID:ForkRevisionProbeIdentity(url,revision)]],nil);
}
- (void)loadResource:(FSResource *)resource options:(FSTaskOptions *)options replyHandler:(void (^)(FSVolume *,NSError *))reply {
    @synchronized(self){
        if(_volume){reply(nil,fs_errorForPOSIXError(EBUSY));return;}
        if(![resource isKindOfClass:[FSPathURLResource class]]){reply(nil,fs_errorForPOSIXError(EINVAL));return;}
        NSURL *url=((FSPathURLResource *)resource).url;
        if(![url startAccessingSecurityScopedResource]){reply(nil,fs_errorForPOSIXError(EPERM));return;}
        _grantedURL=url;
        NSString *revision=ForkRevisionResourceName(url);if(!revision){[self releaseResource];reply(nil,fs_errorForPOSIXError(EINVAL));return;}
        NSError *error=nil;
        try{auto store=std::make_shared<forkfs::Container>(url.fileSystemRepresentation);_volume=[[ForkRevisionVolume alloc] initWithStore:store revision:revision error:&error];}
        catch(const std::exception&){error=fs_errorForPOSIXError(EIO);}
        if(!_volume){[self releaseResource];reply(nil,error?:fs_errorForPOSIXError(EIO));return;}
        self.containerStatus=FSContainerStatus.ready;reply(_volume,nil);
    }
}
- (void)unloadResource:(FSResource *)resource options:(FSTaskOptions *)options replyHandler:(void (^)(NSError *))reply {
    @synchronized(self){[self releaseResource];self.containerStatus=[FSContainerStatus notReadyWithStatus:fs_errorForPOSIXError(ENOTCONN)];reply(nil);}
}
@end
