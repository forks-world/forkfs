#import "WorldVolume.h"
#import "WorldItem.h"
#import <os/log.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Per-op trace at debug level: invisible unless `log stream --level debug` is watching.
#define WFS_TRACE(fmt, ...) os_log_debug(wfs_log(), "op " fmt, ##__VA_ARGS__)

static NSError *perr(int negerrno) { return fs_errorForPOSIXError(negerrno < 0 ? -negerrno : negerrno); }

@implementation WorldVolume {
    wfs_store *_store;
    wfs_view *_view;
    NSString *_base;
    NSURL *_scoped;      // security-scoped backing URL; released on dealloc
    BOOL _readOnly;
    BOOL _inhibitOpen;   // mount option openclose=kernel (default) | forward
    NSLock *_lock;
    NSMutableDictionary<NSNumber *, WorldItem *> *_items; // ino → live item (FSKit needs identity)
}

- (nullable instancetype)initWithBasePath:(NSString *)base scopedURL:(NSURL *)scoped
                                  options:(NSArray<NSString *> *)opts error:(NSError **)err {
    // Options arrive as ["-o", "world=3,store=/x"]; parse both keys.
    NSString *storeDir = nil;
    unsigned long long world = 0;
    BOOL inhibitOpen = YES;
    for (NSString *o in opts) {
        for (NSString *kv in [o componentsSeparatedByString:@","]) {
            if ([kv hasPrefix:@"world="]) world = strtoull([kv substringFromIndex:6].UTF8String, NULL, 10);
            else if ([kv hasPrefix:@"store="]) storeDir = [kv substringFromIndex:6];
            else if ([kv isEqualToString:@"openclose=forward"]) inhibitOpen = NO;
        }
    }
    if (!storeDir) {
        // Sandboxed: resolves inside ~/Library/Containers/world.forks.fs.extension/Data/...
        NSString *as = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
        storeDir = [as stringByAppendingPathComponent:@"World/fs"];
    }
    wfs_store *s = NULL;
    int rc = wfs_store_open(storeDir.fileSystemRepresentation, &s);
    if (rc) { os_log_error(wfs_log(), "store open %{public}@: %d", storeDir, rc); if (err) *err = perr(rc); return nil; }
    wfs_world w = (wfs_world)world;
    if (w == 0) rc = wfs_world_init(s, base.fileSystemRepresentation, &w);
    if (rc) { wfs_store_close(s); if (err) *err = perr(rc); return nil; }
    // The resource path must be the base of the requested world.
    char wbase[4096] = {0};
    rc = wfs_world_info(s, w, NULL, NULL, wbase, sizeof wbase);
    if (rc) { wfs_store_close(s); if (err) *err = perr(rc); return nil; }
    char rbase[4096] = {0};
    if (!realpath(base.fileSystemRepresentation, rbase) || strcmp(rbase, wbase) != 0) {
        os_log_error(wfs_log(), "world %llu base is %{public}s, resource is %{public}s", world, wbase, rbase);
        wfs_store_close(s); if (err) *err = perr(EINVAL); return nil;
    }
    wfs_view *v = NULL;
    rc = wfs_view_open(s, w, &v);
    if (rc) { wfs_store_close(s); if (err) *err = perr(rc); return nil; }

    unsigned char b[16] = {0};
    const char *p = wbase;
    for (size_t i = 0; p[i]; ++i) b[i % 16] = (unsigned char)(b[i % 16] * 33 + (unsigned char)p[i]);
    b[0] ^= (unsigned char)w; b[1] ^= (unsigned char)(w >> 8);
    b[6] = (b[6] & 0x0f) | 0x50; b[8] = (b[8] & 0x3f) | 0x80;
    NSUUID *u = [[NSUUID alloc] initWithUUIDBytes:b];
    NSString *name = [NSString stringWithFormat:@"%@-W%llu", base.lastPathComponent, (unsigned long long)w];
    self = [super initWithVolumeID:[[FSVolumeIdentifier alloc] initWithUUID:u] volumeName:[FSFileName nameWithString:name]];
    if (!self) return nil;
    _store = s; _view = v; _base = base; _scoped = scoped; _lock = [NSLock new]; _items = [NSMutableDictionary new];
    _readOnly = NO;
    _inhibitOpen = inhibitOpen;
    os_log_info(wfs_log(), "volume W%llu over %{public}s store=%{public}@ inhibitOpen=%d", (unsigned long long)w, wbase, storeDir, inhibitOpen);
    return self;
}

- (void)dealloc {
    if (_view) wfs_view_close(_view);
    if (_store) wfs_store_close(_store);
    if (_scoped) [_scoped stopAccessingSecurityScopedResource];
}

#pragma mark - item table

- (WorldItem *)itemFor:(const wfs_attr *)a {
    [_lock lock];
    WorldItem *it = _items[@(a->ino)];
    if (!it) { it = [[WorldItem alloc] initWithIno:a->ino type:a->type]; _items[@(a->ino)] = it; }
    else it.type = a->type;
    [_lock unlock];
    return it;
}

static inline WorldItem *W(FSItem *i) { return [i isKindOfClass:[WorldItem class]] ? (WorldItem *)i : nil; }

#pragma mark - FSVolumePathConfOperations

- (NSInteger)maximumLinkCount { return 32767; }
- (NSInteger)maximumNameLength { return 255; }
- (BOOL)restrictsOwnershipChanges { return NO; }
- (BOOL)truncatesLongNames { return NO; }
- (NSInteger)maximumXattrSize { return 64 * 1024; }
- (uint64_t)maximumFileSize { return UINT64_MAX; }

#pragma mark - FSVolumeOperations

- (FSVolumeSupportedCapabilities *)supportedVolumeCapabilities {
    FSVolumeSupportedCapabilities *c = [FSVolumeSupportedCapabilities new];
    c.supportsHardLinks = YES;
    c.supportsSymbolicLinks = YES;
    c.supportsPersistentObjectIDs = YES;
    c.supports64BitObjectIDs = YES;
    c.supportsSparseFiles = YES;
    c.supports2TBFiles = YES;
    c.supportsHiddenFiles = YES;
    c.supportsFastStatFS = YES;
    c.caseFormat = FSVolumeCaseFormatInsensitiveCasePreserving; // default APFS backing
    return c;
}

- (FSStatFSResult *)volumeStatistics {
    FSStatFSResult *r = [[FSStatFSResult alloc] initWithFileSystemTypeName:@WFS_FS_SHORT_NAME];
    wfs_statfs_info s = {0};
    if (wfs_statfs(_view, &s) == 0) {
        r.blockSize = (NSInteger)s.block_size;
        r.ioSize = 1 << 20;
        r.totalBlocks = s.total_blocks; r.freeBlocks = s.free_blocks; r.availableBlocks = s.avail_blocks;
        r.usedBlocks = s.total_blocks - s.free_blocks;
        r.totalBytes = s.total_blocks * s.block_size; r.freeBytes = s.free_blocks * s.block_size;
        r.availableBytes = s.avail_blocks * s.block_size; r.usedBytes = r.totalBytes - r.freeBytes;
        r.totalFiles = s.total_files; r.freeFiles = s.free_files;
    }
    return r;
}

- (void)activateWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(FSItem *_Nullable, NSError *_Nullable))reply {
    wfs_attr a;
    int rc = wfs_getattr(_view, WFS_INO_ROOT, &a);
    if (rc) { reply(nil, perr(rc)); return; }
    os_log_info(wfs_log(), "activate");
    reply([self itemFor:&a], nil);
}

- (void)deactivateWithOptions:(FSDeactivateOptions)options replyHandler:(void (^)(NSError *_Nullable))reply {
    [_lock lock];
    [_items removeAllObjects];
    [_lock unlock];
    os_log_info(wfs_log(), "deactivate");
    reply(nil);
}

- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(NSError *_Nullable))reply {
    os_log_info(wfs_log(), "mount opts=%{public}@", options.taskOptions);
    reply(nil);
}
- (void)unmountWithReplyHandler:(void (^)(void))reply { os_log_info(wfs_log(), "unmount"); reply(); }
- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError *_Nullable))reply {
    WFS_TRACE("sync flags=%ld", (long)flags);
    reply(nil);
}

// The kernel asks for an item's attributes once more immediately after removing it, before it
// reclaims the item (measured on 27.0: lookup, getattr, remove, *getattr*, getattr(dir), sync,
// reclaim -- exactly one such getattr per create+unlink cycle). By then the core has dropped the
// node record, path_of() has no chain to walk and answers -ESTALE, and FSKit logs that at error
// level: `-[FSVolumeConnector getStandardItemAttributesForItem:...]...error:70`, 150k+ lines in a
// 45-minute run. Replying ENOENT instead only changes the number in the same error line (measured),
// so serve the last attributes the core reported for this inode -- captured microseconds earlier by
// the pre-remove getattr in the very same cycle, so it is a stale snapshot, not an invented one.
// Nothing is cached beyond that snapshot: no per-item fd, no path.
- (void)getAttributes:(FSItemGetAttributesRequest *)desired ofItem:(FSItem *)item
         replyHandler:(void (^)(FSItemAttributes *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("getattr ino=%llu", W(item).ino);
    WorldItem *it = W(item);
    if (!it) { reply(nil, perr(EINVAL)); return; }
    wfs_attr a;
    int rc = wfs_getattr(_view, it.ino, &a);
    if (rc == -ESTALE && it.hasLastAttrs) { a = it.lastAttrs; rc = 0; }
    else if (rc == 0) [it rememberAttrs:&a];
    if (rc) { reply(nil, perr(rc)); return; }
    reply(wfs_attributes(&a), nil);
}

- (void)setAttributes:(FSItemSetAttributesRequest *)req onItem:(FSItem *)item
         replyHandler:(void (^)(FSItemAttributes *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("setattr ino=%llu size=%d mode=%d times=%d", W(item).ino, [req isValid:FSItemAttributeSize], [req isValid:FSItemAttributeMode], [req isValid:FSItemAttributeModifyTime]);
    WorldItem *it = W(item);
    if (!it) { reply(nil, perr(EINVAL)); return; }
    wfs_setattr_req s = {0};
    FSItemAttribute consumed = 0;
    if ([req isValid:FSItemAttributeSize]) { s.valid |= WFS_SET_SIZE; s.size = req.size; consumed |= FSItemAttributeSize; }
    if ([req isValid:FSItemAttributeMode]) { s.valid |= WFS_SET_MODE; s.mode = req.mode; consumed |= FSItemAttributeMode; }
    if ([req isValid:FSItemAttributeUID]) { s.valid |= WFS_SET_UID; s.uid = req.uid; consumed |= FSItemAttributeUID; }
    if ([req isValid:FSItemAttributeGID]) { s.valid |= WFS_SET_GID; s.gid = req.gid; consumed |= FSItemAttributeGID; }
    if ([req isValid:FSItemAttributeFlags]) { s.valid |= WFS_SET_FLAGS; s.flags = req.flags; consumed |= FSItemAttributeFlags; }
    if ([req isValid:FSItemAttributeAccessTime]) { s.valid |= WFS_SET_ATIME; s.atime = {req.accessTime.tv_sec, req.accessTime.tv_nsec}; consumed |= FSItemAttributeAccessTime; }
    if ([req isValid:FSItemAttributeModifyTime]) { s.valid |= WFS_SET_MTIME; s.mtime = {req.modifyTime.tv_sec, req.modifyTime.tv_nsec}; consumed |= FSItemAttributeModifyTime; }
    if ([req isValid:FSItemAttributeBirthTime]) { consumed |= FSItemAttributeBirthTime; } // accepted, not stored (M0)
    wfs_attr a;
    int rc = wfs_setattr(_view, it.ino, &s, &a);
    if (rc) { reply(nil, perr(rc)); return; }
    [it rememberAttrs:&a];
    req.consumedAttributes = consumed;
    reply(wfs_attributes(&a), nil);
}

- (void)lookupItemNamed:(FSFileName *)name inDirectory:(FSItem *)directory
           replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("lookup dir=%llu name=%{public}@", W(directory).ino, name.string);
    WorldItem *dir = W(directory);
    if (!dir) { reply(nil, nil, perr(EINVAL)); return; }
    wfs_attr a;
    int rc = wfs_lookup(_view, dir.ino, (const char *)name.data.bytes, name.data.length, &a);
    if (rc) { reply(nil, nil, perr(rc)); return; }
    reply([self itemFor:&a], name, nil);
}

- (void)reclaimItem:(FSItem *)item replyHandler:(void (^)(NSError *_Nullable))reply {
    WFS_TRACE("reclaim ino=%llu", W(item).ino);
    WorldItem *it = W(item);
    if (it) {
        [_lock lock];
        if (_items[@(it.ino)] == it) [_items removeObjectForKey:@(it.ino)];
        [_lock unlock];
        wfs_forget(_view, it.ino);
    }
    reply(nil);
}

- (void)readSymbolicLink:(FSItem *)item replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply {
    WorldItem *it = W(item);
    if (!it) { reply(nil, perr(EINVAL)); return; }
    char buf[PATH_MAX]; size_t len = 0;
    int rc = wfs_readlink(_view, it.ino, buf, sizeof buf, &len);
    if (rc) { reply(nil, perr(rc)); return; }
    reply([FSFileName nameWithBytes:buf length:len], nil);
}

- (void)createItemNamed:(FSFileName *)name type:(FSItemType)type inDirectory:(FSItem *)directory
             attributes:(FSItemSetAttributesRequest *)req
           replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("create dir=%llu name=%{public}@ type=%ld", W(directory).ino, name.string, (long)type);
    WorldItem *dir = W(directory);
    if (!dir) { reply(nil, nil, perr(EINVAL)); return; }
    wfs_type t = type == FSItemTypeFile ? WFS_T_FILE : type == FSItemTypeDirectory ? WFS_T_DIR : type == FSItemTypeFIFO ? WFS_T_FIFO : WFS_T_UNKNOWN;
    if (t == WFS_T_UNKNOWN) { reply(nil, nil, perr(ENOTSUP)); return; }
    uint32_t mode = [req isValid:FSItemAttributeMode] ? (req.mode & 07777) : (t == WFS_T_DIR ? 0755 : 0644);
    wfs_attr a;
    int rc = wfs_create(_view, dir.ino, (const char *)name.data.bytes, name.data.length, t, mode, &a);
    if (rc) { reply(nil, nil, perr(rc)); return; }
    req.consumedAttributes = [req isValid:FSItemAttributeMode] ? FSItemAttributeMode : 0;
    reply([self itemFor:&a], name, nil);
}

- (void)createSymbolicLinkNamed:(FSFileName *)name inDirectory:(FSItem *)directory
                     attributes:(FSItemSetAttributesRequest *)req linkContents:(FSFileName *)contents
                   replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {
    WorldItem *dir = W(directory);
    if (!dir) { reply(nil, nil, perr(EINVAL)); return; }
    wfs_attr a;
    int rc = wfs_symlink(_view, dir.ino, (const char *)name.data.bytes, name.data.length,
                         (const char *)contents.data.bytes, contents.data.length, &a);
    if (rc) { reply(nil, nil, perr(rc)); return; }
    req.consumedAttributes = 0;
    reply([self itemFor:&a], name, nil);
}

- (void)createLinkToItem:(FSItem *)item named:(FSFileName *)name inDirectory:(FSItem *)directory
            replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply {
    WorldItem *it = W(item), *dir = W(directory);
    if (!it || !dir) { reply(nil, perr(EINVAL)); return; }
    int rc = wfs_link(_view, it.ino, dir.ino, (const char *)name.data.bytes, name.data.length);
    if (rc) { reply(nil, perr(rc)); return; }
    reply(name, nil);
}

- (void)removeItem:(FSItem *)item named:(FSFileName *)name fromDirectory:(FSItem *)directory
      replyHandler:(void (^)(NSError *_Nullable))reply {
    WFS_TRACE("remove dir=%llu name=%{public}@", W(directory).ino, name.string);
    WorldItem *it = W(item), *dir = W(directory);
    if (!it || !dir) { reply(perr(EINVAL)); return; }
    int rc = wfs_unlink(_view, dir.ino, (const char *)name.data.bytes, name.data.length, it.ino);
    reply(rc ? perr(rc) : nil);
}

- (void)renameItem:(FSItem *)item inDirectory:(FSItem *)sourceDirectory named:(FSFileName *)sourceName
         toNewName:(FSFileName *)destinationName inDirectory:(FSItem *)destinationDirectory
          overItem:(FSItem *_Nullable)overItem replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("rename ino=%llu sdir=%llu %{public}@ -> ddir=%llu %{public}@ over=%llu", W(item).ino, W(sourceDirectory).ino, sourceName.string, W(destinationDirectory).ino, destinationName.string, overItem ? W(overItem).ino : 0);
    WorldItem *it = W(item), *src = W(sourceDirectory), *dst = W(destinationDirectory);
    if (!it || !src || !dst) { reply(nil, perr(EINVAL)); return; }
    int rc = wfs_rename(_view, src.ino, (const char *)sourceName.data.bytes, sourceName.data.length,
                        dst.ino, (const char *)destinationName.data.bytes, destinationName.data.length, it.ino);
    if (rc) { os_log_error(wfs_log(), "rename %{public}@ -> %{public}@ failed: %{errno}d", sourceName.string, destinationName.string, -rc); reply(nil, perr(rc)); return; }
    reply(destinationName, nil);
}

struct PackCtx {
    FSDirectoryEntryPacker *packer;
    BOOL wantAttrs;
    WorldVolume *volume;
};

static int pack_entry(void *cp, const wfs_dirent *e) {
    PackCtx *c = static_cast<PackCtx *>(cp);
    FSItemAttributes *attrs = (c->wantAttrs && e->attr) ? wfs_attributes(e->attr) : nil;
    BOOL ok = [c->packer packEntryWithName:[FSFileName nameWithBytes:e->name length:e->name_len]
                                  itemType:wfs_fstype(e->type)
                                    itemID:wfs_fsid(e->ino)
                                nextCookie:e->next_cookie
                                attributes:attrs];
    return ok ? 0 : 1;
}

- (void)enumerateDirectory:(FSItem *)directory startingAtCookie:(FSDirectoryCookie)cookie verifier:(FSDirectoryVerifier)verifier
       providingAttributes:(FSItemGetAttributesRequest *_Nullable)attributes usingPacker:(FSDirectoryEntryPacker *)packer
              replyHandler:(void (^)(FSDirectoryVerifier, NSError *_Nullable))reply {
    WFS_TRACE("readdir dir=%llu cookie=%llu attrs=%d", W(directory).ino, (unsigned long long)cookie, attributes != nil);
    WorldItem *dir = W(directory);
    if (!dir) { reply(FSDirectoryVerifierInitial, perr(EINVAL)); return; }
    PackCtx c{packer, attributes != nil, self};
    int rc = wfs_readdir(_view, dir.ino, cookie == FSDirectoryCookieInitial ? 0 : cookie, attributes != nil, pack_entry, &c);
    reply(FSDirectoryVerifierInitial, rc ? perr(rc) : nil);
}

#pragma mark - FSVolumeOpenCloseOperations

// open/close are the most expensive round trips (~70us each via XPC) and carry no information a
// passthrough needs, so tell the kernel not to send them. Reads and writes that reach us (page-cache
// misses and writeback) open the backing file per request: the open costs ~5us against a ~70us XPC
// round trip, and caching fds per item leaks them because the kernel reclaims items lazily.
- (BOOL)isOpenCloseInhibited { return _inhibitOpen; }

// A POSIX fd keeps the access rights it was opened with, so a file that is chmod'ed read-only while
// open still takes its writeback; the kernel has already done the permission check before it sends us
// the request. Because we open the backing file per request, that open is what gets EACCES instead —
// git's loose-object write (write, fchmod 0444, close) hits this and fails the commit. So lift the bit
// for the open and put the mode straight back on the fd, whose rights are settled by then.
- (int)openFDFor:(WorldItem *)it writable:(BOOL)writable {
    char path[PATH_MAX];
    int rc = wfs_backing_path(_view, it.ino, writable ? 1 : 0, path, sizeof path);
    if (rc) return rc;
    int flags = writable ? O_RDWR : O_RDONLY;
    int fd = open(path, flags);
    if (fd >= 0) return fd;
    if (errno != EACCES) return -errno;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -EACCES;
    mode_t keep = st.st_mode & 07777;
    if (chmod(path, keep | S_IRUSR | (writable ? S_IWUSR : 0)) != 0) return -errno;
    fd = open(path, flags);
    int e = errno;
    if (fd >= 0) fchmod(fd, keep); else chmod(path, keep);
    return fd >= 0 ? fd : -e;
}

- (void)openItem:(FSItem *)item withModes:(FSVolumeOpenModes)modes replyHandler:(void (^)(NSError *_Nullable))reply {
    WFS_TRACE("open ino=%llu modes=%lu", W(item).ino, (unsigned long)modes);
    reply(nil);   // fds are opened per read/write; nothing to do here
}

- (void)closeItem:(FSItem *)item keepingModes:(FSVolumeOpenModes)modes replyHandler:(void (^)(NSError *_Nullable))reply {
    WFS_TRACE("close ino=%llu keep=%lu", W(item).ino, (unsigned long)modes);
    reply(nil);
}

#pragma mark - FSVolumeReadWriteOperations

- (void)readFromFile:(FSItem *)item offset:(off_t)offset length:(size_t)length intoBuffer:(FSMutableFileDataBuffer *)buffer
        replyHandler:(void (^)(size_t, NSError *_Nullable))reply {
    WFS_TRACE("read ino=%llu off=%lld len=%zu", W(item).ino, (long long)offset, length);
    WorldItem *it = W(item);
    if (!it) { reply(0, perr(EINVAL)); return; }
    int fd = [self openFDFor:it writable:NO];
    if (fd < 0) { os_log_error(wfs_log(), "read ino=%llu: cannot open backing: %{errno}d", it.ino, -fd); reply(0, perr(fd)); return; }
    ssize_t n = pread(fd, buffer.mutableBytes, MIN(length, buffer.length), offset);
    int e = errno;
    close(fd);
    if (n < 0) { os_log_error(wfs_log(), "read ino=%llu off=%lld failed: %{errno}d", it.ino, (long long)offset, e); reply(0, perr(e)); }
    else reply((size_t)n, nil);
}

- (void)writeContents:(NSData *)contents toFile:(FSItem *)item atOffset:(off_t)offset
         replyHandler:(void (^)(size_t, NSError *_Nullable))reply {
    WFS_TRACE("write ino=%llu off=%lld len=%lu", W(item).ino, (long long)offset, (unsigned long)contents.length);
    WorldItem *it = W(item);
    if (!it) { reply(0, perr(EINVAL)); return; }
    int fd = [self openFDFor:it writable:YES];
    if (fd < 0) { os_log_error(wfs_log(), "write ino=%llu: cannot open backing: %{errno}d", it.ino, -fd); reply(0, perr(fd)); return; }
    ssize_t n = pwrite(fd, contents.bytes, contents.length, offset);
    int e = errno;
    close(fd);
    if (n < 0) { os_log_error(wfs_log(), "write ino=%llu off=%lld failed: %{errno}d", it.ino, (long long)offset, e); reply(0, perr(e)); }
    else reply((size_t)n, nil);
}

#pragma mark - FSVolumeXattrOperations

// Do NOT implement supportedXattrNamesForItem: / xattrOperationsInhibited. Telling the kernel an item
// has no xattrs makes it fall back to AppleDouble "._" files for com.apple.provenance on every create,
// which doubles the create/unlink cost (measured: create 548us -> 1046us).
- (void)getXattrNamed:(FSFileName *)name ofItem:(FSItem *)item replyHandler:(void (^)(NSData *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("getxattr ino=%llu %{public}@", W(item).ino, name.string);
    WorldItem *it = W(item);
    if (!it) { reply(nil, perr(EINVAL)); return; }
    size_t len = 0;
    int rc = wfs_getxattr(_view, it.ino, name.string.UTF8String, NULL, 0, &len);
    if (rc) { reply(nil, perr(rc)); return; }
    NSMutableData *d = [NSMutableData dataWithLength:len];
    rc = wfs_getxattr(_view, it.ino, name.string.UTF8String, d.mutableBytes, len, &len);
    if (rc) { reply(nil, perr(rc)); return; }
    d.length = len;
    reply(d, nil);
}

- (void)setXattrNamed:(FSFileName *)name toData:(NSData *_Nullable)value onItem:(FSItem *)item
               policy:(FSSetXattrPolicy)policy replyHandler:(void (^)(NSError *_Nullable))reply {
    WorldItem *it = W(item);
    if (!it) { reply(perr(EINVAL)); return; }
    int rc;
    if (policy == FSSetXattrPolicyDelete) rc = wfs_removexattr(_view, it.ino, name.string.UTF8String);
    else {
        int flags = policy == FSSetXattrPolicyMustCreate ? WFS_XATTR_CREATE : policy == FSSetXattrPolicyMustReplace ? WFS_XATTR_REPLACE : 0;
        rc = wfs_setxattr(_view, it.ino, name.string.UTF8String, value.bytes, value.length, flags);
    }
    reply(rc ? perr(rc) : nil);
}

- (void)listXattrsOfItem:(FSItem *)item replyHandler:(void (^)(NSArray<FSFileName *> *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("listxattr ino=%llu", W(item).ino);
    WorldItem *it = W(item);
    if (!it) { reply(nil, perr(EINVAL)); return; }
    size_t len = 0;
    int rc = wfs_listxattr(_view, it.ino, NULL, 0, &len);
    if (rc) { reply(nil, perr(rc)); return; }
    NSMutableArray<FSFileName *> *names = [NSMutableArray new];
    if (len == 0) { reply(names, nil); return; }
    NSMutableData *buf = [NSMutableData dataWithLength:len];
    rc = wfs_listxattr(_view, it.ino, (char *)buf.mutableBytes, len, &len);
    if (rc) { reply(nil, perr(rc)); return; }
    const char *b = (const char *)buf.bytes;
    size_t start = 0;
    for (size_t i = 0; i < len; ++i) if (b[i] == 0) { [names addObject:[FSFileName nameWithBytes:b + start length:i - start]]; start = i + 1; }
    reply(names, nil);
}

@end
