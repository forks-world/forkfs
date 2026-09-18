#import <FSKit/FSKit.h>

NS_ASSUME_NONNULL_BEGIN

os_log_t wfs_log(void);

/// FSKit volume over one WorldFS view. Thin: every namespace decision goes to the C++ core;
/// this class only does FSItem identity, fd lifetime, and pread/pwrite.
@interface WorldVolume : FSVolume <FSVolumeOperations, FSVolumePathConfOperations,
                                   FSVolumeOpenCloseOperations, FSVolumeReadWriteOperations,
                                   FSVolumeXattrOperations>
- (nullable instancetype)initWithBasePath:(NSString *)base scopedURL:(nullable NSURL *)scoped
                                  options:(NSArray<NSString *> *)opts error:(NSError **)err;
@end

NS_ASSUME_NONNULL_END
