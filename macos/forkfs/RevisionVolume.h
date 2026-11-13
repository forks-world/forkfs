#import <FSKit/FSKit.h>
#if __MAC_OS_X_VERSION_MAX_ALLOWED < 260000
#error "The revision FSKit frontend requires the macOS 26 SDK or newer"
#endif
@interface RevisionVolume : FSVolume <FSVolumeOperations, FSVolumePathConfOperations,
    FSVolumeOpenCloseOperations, FSVolumeReadWriteOperations, FSVolumeXattrOperations, FSVolumeRenameOperations
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
    , FSVolumeSeekRegionHandler
#endif
    >
- (instancetype)initWithRuntimeURL:(NSURL *)url readOnly:(BOOL)readOnly error:(NSError **)error;
- (instancetype)initWithRuntimeURL:(NSURL *)url error:(NSError **)error;
@end
