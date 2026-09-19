#import <FSKit/FSKit.h>

NS_ASSUME_NONNULL_BEGIN

os_log_t wfs_log(void);

/// macOS 27 Handler-style FSKit volume over one WorldFS view.
///
/// Same passthrough semantics as the frozen `WorldVolume` (per-request backing open, quarantine
/// hiding, no fd cache); the difference is the API surface: every reply is an `FSVolumeHandlerResult`
/// carrying the `FSItem.Attributes` FSKit says it caches, so the kernel should not have to come back
/// with a `getattr` after every lookup/create/remove. Kernel data caching is negotiated through
/// `FSVolumeDataCacheHandler` instead of being switched off wholesale with `openCloseInhibited`.
///
/// Kernel data caching is inhibited by default (`dataCacheInhibited = YES`), which behaves exactly
/// like the old `openCloseInhibited = YES` path; a `wfs_datacache` marker file in the store
/// directory turns the protocol back on, so the A/B of docs/FSKIT_HANDLER_API_MACOS27.md §2.3
/// stays reproducible without reinstalling the appex (which drops every live mount).
API_AVAILABLE(macos(27.0))
@interface WorldVolumeH : FSVolume <FSVolumeHandler, FSVolumePathConfOperations,
                                    FSVolumeReadWriteHandler, FSVolumeXattrHandler,
                                    FSVolumeDataCacheHandler>
- (nullable instancetype)initWithBasePath:(NSString *)base scopedURL:(nullable NSURL *)scoped
                                  options:(NSArray<NSString *> *)opts error:(NSError **)err;
@end

NS_ASSUME_NONNULL_END
