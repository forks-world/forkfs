#import <FSKit/FSKit.h>
#import "worldfs/worldfs.h"
#import "worldfs/worldfs_fskit.h"

NS_ASSUME_NONNULL_BEGIN

/// One live FSItem. Identity is the core's logical inode; the fd is the data plane.
@interface WorldItem : FSItem
@property (nonatomic, readonly) wfs_ino ino;
@property (nonatomic) wfs_type type;
@property (nonatomic) int fd;
@property (nonatomic) BOOL fdWritable;
@property (nonatomic) int openCount;
/// Last attributes the core reported for this inode. Served when the core has since dropped the
/// node record (-ESTALE); see -getAttributes:ofItem:replyHandler: in WorldVolume.mm.
@property (nonatomic) BOOL hasLastAttrs;
@property (nonatomic) wfs_attr lastAttrs;
- (instancetype)initWithIno:(wfs_ino)ino type:(wfs_type)type;
/// Remember `a` as the last known attributes of this item.
- (void)rememberAttrs:(const wfs_attr *)a;
@end

/// Logical inode <-> FSKit item id. Core root is 1; FSKit root is 2 and parent-of-root is 1.
static inline FSItemID wfs_fsid(wfs_ino ino) { return ino == WFS_INO_ROOT ? FSItemIDRootDirectory : (FSItemID)ino; }
static inline FSItemID wfs_fsparent(const wfs_attr *a) {
    return a->ino == WFS_INO_ROOT ? FSItemIDParentOfRoot : wfs_fsid(a->parent);
}
static inline FSItemType wfs_fstype(wfs_type t) {
    switch (t) {
    case WFS_T_FILE: return FSItemTypeFile;
    case WFS_T_DIR: return FSItemTypeDirectory;
    case WFS_T_SYMLINK: return FSItemTypeSymlink;
    case WFS_T_FIFO: return FSItemTypeFIFO;
    case WFS_T_CHR: return FSItemTypeCharDevice;
    case WFS_T_BLK: return FSItemTypeBlockDevice;
    case WFS_T_SOCK: return FSItemTypeSocket;
    default: return FSItemTypeUnknown;
    }
}
FSItemAttributes *wfs_attributes(const wfs_attr *a);

NS_ASSUME_NONNULL_END
