// FSKit unary file system: one FSPathURLResource (base dir) → one WorldVolume.
#import <FSKit/FSKit.h>
#import <os/log.h>
#import "WorldVolume.h"
#include "worldfs/worldfs.h"
#include "worldfs/worldfs_fskit.h"

os_log_t wfs_log(void) {
    static os_log_t l;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ l = os_log_create("world.forks.fs", "extension"); });
    return l;
}

@interface WorldFileSystem : FSUnaryFileSystem <FSUnaryFileSystemOperations>
@end

@implementation WorldFileSystem

- (void)probeResource:(FSResource *)resource
         replyHandler:(void (^)(FSProbeResult *_Nullable, NSError *_Nullable))reply {
    if (![resource isKindOfClass:[FSPathURLResource class]]) {
        os_log_error(wfs_log(), "probe: unsupported resource %{public}@", resource);
        reply(FSProbeResult.notRecognizedProbeResult, nil);
        return;
    }
    FSPathURLResource *r = (FSPathURLResource *)resource;
    os_log_info(wfs_log(), "probe %{public}@", r.url.path);
    // Container id derived from the path, so re-probing is stable.
    unsigned char b[16] = {0};
    NSData *d = [r.url.path dataUsingEncoding:NSUTF8StringEncoding];
    const unsigned char *p = (const unsigned char *)d.bytes;
    for (NSUInteger i = 0; i < d.length; ++i) b[i % 16] = (unsigned char)(b[i % 16] * 31 + p[i]);
    b[6] = (b[6] & 0x0f) | 0x50; b[8] = (b[8] & 0x3f) | 0x80;
    NSUUID *u = [[NSUUID alloc] initWithUUIDBytes:b];
    reply([FSProbeResult usableProbeResultWithName:r.url.lastPathComponent
                                       containerID:[[FSContainerIdentifier alloc] initWithUUID:u]], nil);
}

- (void)loadResource:(FSResource *)resource
             options:(FSTaskOptions *)options
        replyHandler:(void (^)(FSVolume *_Nullable, NSError *_Nullable))reply {
    if (![resource isKindOfClass:[FSPathURLResource class]]) { reply(nil, fs_errorForPOSIXError(EINVAL)); return; }
    FSPathURLResource *r = (FSPathURLResource *)resource;
    os_log_info(wfs_log(), "load %{public}@ writable=%d opts=%{public}@", r.url.path, r.writable, options.taskOptions);
    // Sandboxed: the path URL is security-scoped; consume it before touching the tree.
    BOOL scoped = [r.url startAccessingSecurityScopedResource];
    os_log_info(wfs_log(), "security-scoped access: %d", scoped);
    NSError *err = nil;
    WorldVolume *v = [[WorldVolume alloc] initWithBasePath:r.url.path scopedURL:(scoped ? r.url : nil)
                                                   options:options.taskOptions error:&err];
    if (!v) { reply(nil, err); return; }
    self.containerStatus = FSContainerStatus.ready;
    reply(v, nil);
}

- (void)unloadResource:(FSResource *)resource options:(FSTaskOptions *)options replyHandler:(void (^)(NSError *_Nullable))reply {
    os_log_info(wfs_log(), "unload");
    self.containerStatus = [FSContainerStatus notReadyWithStatus:fs_errorForPOSIXError(ENOTCONN)];
    reply(nil);
}

- (void)didFinishLoading {
    os_log_info(wfs_log(), "didFinishLoading core=%{public}s", wfs_version());
}

@end
