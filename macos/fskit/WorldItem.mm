#import "WorldItem.h"
#import <unistd.h>

@implementation WorldItem
- (instancetype)initWithIno:(wfs_ino)ino type:(wfs_type)type {
    if ((self = [super init])) { _ino = ino; _type = type; _fd = -1; }
    return self;
}
- (void)dealloc { if (_fd >= 0) close(_fd); }
@end

FSItemAttributes *wfs_attributes(const wfs_attr *a) {
    FSItemAttributes *r = [FSItemAttributes new];
    r.type = wfs_fstype(a->type);
    r.mode = a->mode;
    r.linkCount = a->nlink;
    r.uid = a->uid;
    r.gid = a->gid;
    r.flags = a->flags;
    r.size = a->size;
    r.allocSize = a->alloc_size;
    r.fileID = wfs_fsid(a->ino);
    r.parentID = wfs_fsparent(a);
    r.accessTime = (struct timespec){(time_t)a->atime.sec, (long)a->atime.nsec};
    r.modifyTime = (struct timespec){(time_t)a->mtime.sec, (long)a->mtime.nsec};
    r.changeTime = (struct timespec){(time_t)a->ctime.sec, (long)a->ctime.nsec};
    r.birthTime = (struct timespec){(time_t)a->btime.sec, (long)a->btime.nsec};
    r.supportsLimitedXAttrs = NO;
    r.inhibitKernelOffloadedIO = NO;
    return r;
}
