#import <FSKit/FSKit.h>
#ifdef __cplusplus
#include "container.h"
#include <memory>
// Experimental read-only adapter. Owns repository lifetime; no host-file I/O.
@interface ForkRevisionVolume : FSVolume <FSVolumeOperations, FSVolumeReadWriteOperations>
@property(nonatomic,readonly) FSItem *rootItem;
- (instancetype)initWithStore:(std::shared_ptr<forkfs::Container>)store
                     revision:(NSString *)revision error:(NSError **)error;
@end
#endif
