#import "RevisionFileSystem.h"
#import "RevisionVolume.h"
#include "metadata_view.h"
#include <fcntl.h>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>
// Exactly one bounded selector is accepted. Neither selector can redirect the
// security-scoped repository path. Invalid/conflicting markers fail closed.
@implementation ForkFSResourceSelection
+ (instancetype)selectionAtURL:(NSURL *)url {
    if(!url.isFileURL)return nil;
    int directory=open(url.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(directory<0)return nil;
    const char *markers[]={".forkfs-fskit-revision",".forkfs-fskit-world"};
    int selected=-1;
    for(int i=0;i<2;++i){
        struct stat st{};
        if(fstatat(directory,markers[i],&st,AT_SYMLINK_NOFOLLOW)==0){
            if(selected>=0 || !S_ISREG(st.st_mode)){close(directory);return nil;}
            selected=i;
        }else if(errno!=ENOENT){close(directory);return nil;}
    }
    if(selected<0){close(directory);return nil;}
    int fd=openat(directory,markers[selected],O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);close(directory);
    if(fd<0)return nil;
    struct stat st{};char data[66];ssize_t n=-1;
    if(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size>0 && st.st_size<=65){
        do{n=read(fd,data,sizeof(data));}while(n<0 && errno==EINTR);
    }
    close(fd);
    if(n<=0 || n>65 || n!=st.st_size)return nil;if(data[n-1]=='\n')--n;if(n<=0 || n>64)return nil;
    try{forkfs::NamespaceView::valid_name(std::string(data,n));}catch(const std::exception&){return nil;}
    auto result=[ForkFSResourceSelection new];
    result->_name=[[NSString alloc] initWithBytes:data length:n encoding:NSASCIIStringEncoding];
    if(!result->_name)return nil;
    result->_readOnly=selected==0;return result;
}
- (NSUUID *)probeIdentityAtURL:(NSURL *)url {
    // Preserve existing revision identities, and give Worlds a distinct domain.
    std::string input=self.readOnly?"forkfs.fskit.resource.v1":"forkfs.fskit.world.v1";
    input+=url.fileSystemRepresentation;input.push_back('\0');input+=self.name.UTF8String;
    auto hash=forkfs::Journal::object_id(std::vector<unsigned char>(input.begin(),input.end()));
    unsigned char uuid[16];std::copy_n(hash.begin(),16,uuid);uuid[6]=(uuid[6]&15)|0x80;uuid[8]=(uuid[8]&63)|0x80;
    return [[NSUUID alloc] initWithUUIDBytes:uuid];
}
@end
@implementation ForkRevisionFileSystem {
    ForkRevisionVolume *_volume;
    NSURL *_grantedURL;
}
- (NSError *)releaseResource {
    NSError *error=[_volume revoke];if(error)return error;
    _volume=nil;
    if(_grantedURL){[_grantedURL stopAccessingSecurityScopedResource];_grantedURL=nil;}
    return nil;
}
- (void)dealloc {
    // Final teardown cannot report/retry an error. Drop this loader's volume
    // ownership before balancing its grant, even if pin cleanup failed.
    [_volume revoke];_volume=nil;
    if(_grantedURL)[_grantedURL stopAccessingSecurityScopedResource];
}
- (void)probeResource:(FSResource *)resource replyHandler:(void (^)(FSProbeResult *,NSError *))reply {
    if(![resource isKindOfClass:[FSPathURLResource class]]){reply(FSProbeResult.notRecognizedProbeResult,nil);return;}
    NSURL *url=((FSPathURLResource *)resource).url;
    if(![url startAccessingSecurityScopedResource]){reply(FSProbeResult.notRecognizedProbeResult,fs_errorForPOSIXError(EPERM));return;}
    ForkFSResourceSelection *selection=[ForkFSResourceSelection selectionAtURL:url];[url stopAccessingSecurityScopedResource];
    if(!selection){reply(FSProbeResult.notRecognizedProbeResult,nil);return;}
    reply([FSProbeResult usableProbeResultWithName:selection.name containerID:[[FSContainerIdentifier alloc] initWithUUID:[selection probeIdentityAtURL:url]]],nil);
}
- (void)loadResource:(FSResource *)resource options:(FSTaskOptions *)options replyHandler:(void (^)(FSVolume *,NSError *))reply {
    @synchronized(self){
        if(_volume){reply(nil,fs_errorForPOSIXError(EBUSY));return;}
        if(![resource isKindOfClass:[FSPathURLResource class]]){reply(nil,fs_errorForPOSIXError(EINVAL));return;}
        if(!((FSPathURLResource *)resource).writable){reply(nil,fs_errorForPOSIXError(EROFS));return;}
        NSURL *url=((FSPathURLResource *)resource).url;
        if(![url startAccessingSecurityScopedResource]){reply(nil,fs_errorForPOSIXError(EPERM));return;}
        _grantedURL=url;
        ForkFSResourceSelection *selection=[ForkFSResourceSelection selectionAtURL:url];if(!selection){[self releaseResource];reply(nil,fs_errorForPOSIXError(EINVAL));return;}
        NSError *error=nil;
        try{
            auto mode=selection.readOnly?forkfs::Container::OpenMode::RevisionPreview:forkfs::Container::OpenMode::ReadWrite;
            auto store=std::make_shared<forkfs::Container>(url.fileSystemRepresentation,mode);
            _volume=selection.readOnly?[[ForkRevisionVolume alloc] initWithStore:store revision:selection.name error:&error]
                                      :[[ForkRevisionVolume alloc] initWithStore:store world:selection.name error:&error];
        }
        catch(const std::system_error& e){error=fs_errorForPOSIXError(e.code().value());}
        catch(const std::exception&){error=fs_errorForPOSIXError(EIO);}
        if(!_volume){[self releaseResource];reply(nil,error?:fs_errorForPOSIXError(EIO));return;}
        self.containerStatus=FSContainerStatus.ready;reply(_volume,nil);
    }
}
- (void)unloadResource:(FSResource *)resource options:(FSTaskOptions *)options replyHandler:(void (^)(NSError *))reply {
    @synchronized(self){
        NSError *error=[self releaseResource];
        self.containerStatus=[FSContainerStatus notReadyWithStatus:error?:fs_errorForPOSIXError(ENOTCONN)];
        reply(error);
    }
}
@end
