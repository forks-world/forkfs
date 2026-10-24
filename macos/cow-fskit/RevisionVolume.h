#import <FSKit/FSKit.h>
#ifdef __cplusplus
#include "container.h"
#include <memory>
#include "MountTransport.h"
// Experimental inode adapter. Resource selection supports revisions and live Worlds.
@interface ForkRevisionVolume : FSVolume <FSVolumeOperations, FSVolumeReadWriteOperations, FSVolumeXattrOperations>
@property(nonatomic,readonly) FSItem *rootItem;
- (NSError *)revoke;
- (instancetype)initWithMountCredentials:(const forkfs::MountCredentials&)credentials error:(NSError **)error;
- (instancetype)initWithStore:(std::shared_ptr<forkfs::Container>)store
                     revision:(NSString *)revision error:(NSError **)error;
- (instancetype)initWithStore:(std::shared_ptr<forkfs::Container>)store
                        world:(NSString *)world error:(NSError **)error;
@end
#endif
