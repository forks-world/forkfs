#import <FSKit/FSKit.h>

// An immutable selector parsed without opening the metadata database.
@interface ForkFSResourceSelection : NSObject
@property(nonatomic,readonly,copy) NSString *name;
@property(nonatomic,readonly) BOOL readOnly;
+ (instancetype)selectionAtURL:(NSURL *)url;
- (NSUUID *)probeIdentityAtURL:(NSURL *)url;
@end

@interface ForkRevisionFileSystem : FSUnaryFileSystem <FSUnaryFileSystemOperations>
@end
