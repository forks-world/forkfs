#import <FSKit/FSKit.h>
#import "RevisionVolume.h"
#import "RevisionConfig.h"
@interface RevisionFileSystem : FSUnaryFileSystem <FSUnaryFileSystemOperations>
@end
@implementation RevisionFileSystem
- (void)probeResource:(FSResource*)resource replyHandler:(void(^)(FSProbeResult*,NSError*))reply {
    if(![resource isKindOfClass:FSPathURLResource.class]){reply(FSProbeResult.notRecognizedProbeResult,nil);return;}
    FSPathURLResource* path=(FSPathURLResource*)resource;
    BOOL scoped=[path.url startAccessingSecurityScopedResource];
    // JSON is validated by the volume loader. Probing does not acquire a daemon lease.
    NSDictionary* config=RevisionLoadDescriptor(path.url,nil);
    if(scoped)[path.url stopAccessingSecurityScopedResource];
    if(!config){reply(FSProbeResult.notRecognizedProbeResult,nil);return;}
    reply([FSProbeResult usableProbeResultWithName:@"ForkFS" containerID:[[FSContainerIdentifier alloc] initWithUUID:RevisionDescriptorUUID(config)]],nil);
}
- (void)loadResource:(FSResource*)resource options:(FSTaskOptions*)options replyHandler:(void(^)(FSVolume*,NSError*))reply {
    if(![resource isKindOfClass:FSPathURLResource.class]){reply(nil,fs_errorForPOSIXError(EINVAL));return;}
    // This URL grants access to a private descriptor, not to repository data.
    // FSKit supplies a read-only descriptor URL even for a writable World.
    // Revision immutability and mount options determine volume writability.
    NSError* error=nil;RevisionVolume* volume=[[RevisionVolume alloc] initWithRuntimeURL:((FSPathURLResource*)resource).url error:&error];
    if(volume)self.containerStatus=FSContainerStatus.ready;reply(volume,error);
}
- (void)unloadResource:(FSResource*)resource options:(FSTaskOptions*)options replyHandler:(void(^)(NSError*))reply {
    self.containerStatus=[FSContainerStatus notReadyWithStatus:fs_errorForPOSIXError(ENOTCONN)];reply(nil);
}
- (void)didFinishLoading {}
@end
