#import "WorldVolumeHandler.h"
#import "WorldItem.h"
#import <os/log.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Per-op trace at debug level: invisible unless `log stream --level debug` is watching.
// Same "op <name>" shape as WorldVolume.mm, so the same optrace.sh histograms both builds.
#define WFS_TRACE(fmt, ...) os_log_debug(wfs_log(), "op " fmt, ##__VA_ARGS__)

static NSError *perr(int negerrno) { return fs_errorForPOSIXError(negerrno < 0 ? -negerrno : negerrno); }

API_AVAILABLE(macos(27.0))
@implementation WorldVolumeH {
    wfs_store *_store;
    wfs_view *_view;
    NSString *_base;
    NSURL *_scoped;      // security-scoped backing URL; released on dealloc
    BOOL _readOnly;
    BOOL _inhibitDataCache;
    NSLock *_lock;
    NSMutableDictionary<NSNumber *, WorldItem *> *_items; // ino → live item (FSKit needs identity)
}

- (nullable instancetype)initWithBasePath:(NSString *)base scopedURL:(NSURL *)scoped
                                  options:(NSArray<NSString *> *)opts error:(NSError **)err {
    NSString *storeDir = nil;
    unsigned long long world = 0;
    for (NSString *o in opts) {
        for (NSString *kv in [o componentsSeparatedByString:@","]) {
            if ([kv hasPrefix:@"world="]) world = strtoull([kv substringFromIndex:6].UTF8String, NULL, 10);
            else if ([kv hasPrefix:@"store="]) storeDir = [kv substringFromIndex:6];
        }
    }
    if (!storeDir) {
        NSString *as = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
        storeDir = [as stringByAppendingPathComponent:@"World/fs"];
    }
    // Kernel data caching is inhibited by default -- measured: the protocol's open/close reach the
    // extension on every open(2)/close(2) with no deferred close and no coalescing (+2 XPC round
    // trips each, open+close 11us -> 141us), while reads and writes behave exactly as they do
    // without it. See docs/FSKIT_HANDLER_API_MACOS27.md §2.3. The marker turns it back on so the
    // A/B stays reproducible. Read once, before the volume is handed to FSKit (FSKit reads
    // dataCacheInhibited right after loadResource replies and never again).
    BOOL noDataCache = ![[NSFileManager defaultManager]
        fileExistsAtPath:[storeDir stringByAppendingPathComponent:@"wfs_datacache"]];

    wfs_store *s = NULL;
    int rc = wfs_store_open(storeDir.fileSystemRepresentation, &s);
    if (rc) { os_log_error(wfs_log(), "store open %{public}@: %d", storeDir, rc); if (err) *err = perr(rc); return nil; }
    wfs_world w = (wfs_world)world;
    if (w == 0) {
        wfs_identity id;
        rc = wfs_world_verify_identity(s, base.fileSystemRepresentation, &id);
        if (rc) { wfs_store_close(s); if (err) *err = perr(rc); return nil; }
        w = id.world_id;
    }
    wfs_world_rec rec;
    rc = wfs_world_info(s, w, &rec);
    if (rc) { wfs_store_close(s); if (err) *err = perr(rc); return nil; }
    char wbase[WFS_PATH_MAX] = {0};
    snprintf(wbase, sizeof wbase, "%s", rec.path);
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
    _inhibitDataCache = noDataCache;
    os_log_info(wfs_log(), "volume(handler) W%llu over %{public}s store=%{public}@ noDataCache=%d",
                (unsigned long long)w, wbase, storeDir, noDataCache);
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
    [it rememberAttrs:a];
    return it;
}

static inline WorldItem *W(FSItem *i) { return [i isKindOfClass:[WorldItem class]] ? (WorldItem *)i : nil; }

// Attributes of an item we already hold, for the "directoryAttributes" / "renamedItemAttributes"
// slots of the result objects. The core answers these out of its own node table plus one lstat(2)
// (~2us), which is two orders of magnitude cheaper than letting the kernel come back over XPC.
- (FSItemAttributes *_Nullable)attrsOf:(WorldItem *)it {
    if (!it) return nil;
    wfs_attr a;
    int rc = wfs_getattr(_view, it.ino, &a);
    if (rc == 0) { [it rememberAttrs:&a]; return wfs_attributes(&a); }
    if (it.hasLastAttrs) { wfs_attr la = it.lastAttrs; return wfs_attributes(&la); }
    return nil;
}

#pragma mark - FSVolumePathConfOperations

- (NSInteger)maximumLinkCount { return 32767; }
- (NSInteger)maximumNameLength { return 255; }
- (BOOL)restrictsOwnershipChanges { return NO; }
- (BOOL)truncatesLongNames { return NO; }
- (NSInteger)maximumXattrSize { return 64 * 1024; }
- (uint64_t)maximumFileSize { return UINT64_MAX; }

#pragma mark - FSVolumeCommonOperations

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

// Every mutating result takes a free-space update. Recomputing it means a statfs(2) per mutation,
// and passing nil makes FSKit call -volumeStatistics itself ("may lead to degraded performance").
// A passthrough's free space is the backing volume's, which statfs(2) on the mount point still
// reports through -volumeStatistics, so tell FSKit that this operation changed nothing.
static FSFreeSpace *noFreeSpaceUpdate(void) { return FSFreeSpace.noUpdate; }

#pragma mark - FSVolumeHandler: lifecycle

- (void)activateVolumeWithOptions:(FSTaskOptions *)options
                     replyHandler:(void (^)(FSActivateResult *_Nullable, NSError *_Nullable))reply {
    wfs_attr a;
    int rc = wfs_getattr(_view, WFS_INO_ROOT, &a);
    if (rc) { reply(nil, perr(rc)); return; }
    os_log_info(wfs_log(), "activate(handler)");
    reply([[FSActivateResult alloc] initWithRootItem:[self itemFor:&a]], nil);
}

- (void)deactivateVolumeWithOptions:(FSDeactivateOptions)options replyHandler:(void (^)(NSError *_Nullable))reply {
    [_lock lock];
    [_items removeAllObjects];
    [_lock unlock];
    os_log_info(wfs_log(), "deactivate(handler)");
    reply(nil);
}

- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(NSError *_Nullable))reply {
    os_log_info(wfs_log(), "mount(handler) opts=%{public}@", options.taskOptions);
    reply(nil);
}
- (void)unmountWithReplyHandler:(void (^)(void))reply { os_log_info(wfs_log(), "unmount"); reply(); }
- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError *_Nullable))reply {
    WFS_TRACE("sync flags=%ld", (long)flags);
    reply(nil);
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

#pragma mark - FSVolumeHandler: namespace

- (void)lookupItemNamed:(FSFileName *)name inDirectory:(FSItem *)directory context:(FSContext *)context
           replyHandler:(void (^)(FSLookupItemResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("lookup dir=%llu name=%{public}@", W(directory).ino, name.string);
    WorldItem *dir = W(directory);
    if (!dir) { reply(nil, perr(EINVAL)); return; }
    wfs_attr a;
    int rc = wfs_lookup(_view, dir.ino, (const char *)name.data.bytes, name.data.length, &a);
    if (rc) { reply(nil, perr(rc)); return; }
    // The whole point of the 27 API: hand the attributes back with the item, so the kernel does not
    // have to send a getattr round trip after every successful lookup.
    reply([[FSLookupItemResult alloc] initWithFoundItem:[self itemFor:&a]
                                               itemName:name
                                         itemAttributes:wfs_attributes(&a)], nil);
}

- (void)createItemNamed:(FSFileName *)name type:(FSItemType)type inDirectory:(FSItem *)directory
             attributes:(FSItemSetAttributesRequest *)req context:(FSContext *)context
           replyHandler:(void (^)(FSCreateItemResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("create dir=%llu name=%{public}@ type=%ld", W(directory).ino, name.string, (long)type);
    WorldItem *dir = W(directory);
    if (!dir) { reply(nil, perr(EINVAL)); return; }
    wfs_type t = type == FSItemTypeFile ? WFS_T_FILE : type == FSItemTypeDirectory ? WFS_T_DIR : type == FSItemTypeFIFO ? WFS_T_FIFO : WFS_T_UNKNOWN;
    if (t == WFS_T_UNKNOWN) { reply(nil, perr(ENOTSUP)); return; }
    uint32_t mode = [req isValid:FSItemAttributeMode] ? (req.mode & 07777) : (t == WFS_T_DIR ? 0755 : 0644);
    wfs_attr a;
    int rc = wfs_create(_view, dir.ino, (const char *)name.data.bytes, name.data.length, t, mode, &a);
    if (rc) { reply(nil, perr(rc)); return; }
    req.consumedAttributes = [req isValid:FSItemAttributeMode] ? FSItemAttributeMode : 0;
    FSItemAttributes *da = [self attrsOf:dir];
    if (!da) { reply(nil, perr(EIO)); return; }
    reply([[FSCreateItemResult alloc] initWithNewItem:[self itemFor:&a]
                                          newItemName:name
                                    newItemAttributes:wfs_attributes(&a)
                                  directoryAttributes:da
                                            freeSpace:noFreeSpaceUpdate()], nil);
}

- (void)createSymbolicLinkNamed:(FSFileName *)name inDirectory:(FSItem *)directory
                     attributes:(FSItemSetAttributesRequest *)req linkContents:(FSFileName *)contents
                        context:(FSContext *)context
                   replyHandler:(void (^)(FSCreateSymlinkResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("symlink dir=%llu name=%{public}@", W(directory).ino, name.string);
    WorldItem *dir = W(directory);
    if (!dir) { reply(nil, perr(EINVAL)); return; }
    wfs_attr a;
    int rc = wfs_symlink(_view, dir.ino, (const char *)name.data.bytes, name.data.length,
                         (const char *)contents.data.bytes, contents.data.length, &a);
    if (rc) { reply(nil, perr(rc)); return; }
    req.consumedAttributes = 0;
    FSItemAttributes *da = [self attrsOf:dir];
    if (!da) { reply(nil, perr(EIO)); return; }
    reply((FSCreateSymlinkResult *)[[FSCreateSymlinkResult alloc]
              initWithNewItem:[self itemFor:&a]
                  newItemName:name
            newItemAttributes:wfs_attributes(&a)
          directoryAttributes:da
                    freeSpace:noFreeSpaceUpdate()], nil);
}

- (void)createLinkToItem:(FSItem *)item named:(FSFileName *)name inDirectory:(FSItem *)directory
                 context:(FSContext *)context
            replyHandler:(void (^)(FSCreateLinkResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("link ino=%llu dir=%llu name=%{public}@", W(item).ino, W(directory).ino, name.string);
    WorldItem *it = W(item), *dir = W(directory);
    if (!it || !dir) { reply(nil, perr(EINVAL)); return; }
    int rc = wfs_link(_view, it.ino, dir.ino, (const char *)name.data.bytes, name.data.length);
    if (rc) { reply(nil, perr(rc)); return; }
    FSItemAttributes *la = [self attrsOf:it], *da = [self attrsOf:dir];
    if (!la || !da) { reply(nil, perr(EIO)); return; }
    reply([[FSCreateLinkResult alloc] initWithLinkName:name
                                        linkAttributes:la
                                   directoryAttributes:da
                                             freeSpace:noFreeSpaceUpdate()], nil);
}

- (void)removeItem:(FSItem *)item named:(FSFileName *)name fromDirectory:(FSItem *)directory
           context:(FSContext *)context
      replyHandler:(void (^)(FSRemoveItemResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("remove dir=%llu name=%{public}@", W(directory).ino, name.string);
    WorldItem *it = W(item), *dir = W(directory);
    if (!it || !dir) { reply(nil, perr(EINVAL)); return; }
    // The result wants the removed item's attributes. After the unlink the core has dropped the node
    // (path_of answers -ESTALE), so snapshot them first and fix the link count up afterwards -- the
    // same stale-snapshot rule the old frontend used to answer the kernel's post-remove getattr.
    wfs_attr pre;
    BOOL havePre = (wfs_getattr(_view, it.ino, &pre) == 0);
    int rc = wfs_unlink(_view, dir.ino, (const char *)name.data.bytes, name.data.length, it.ino);
    if (rc) { reply(nil, perr(rc)); return; }
    wfs_attr post;
    FSItemAttributes *ia;
    if (wfs_getattr(_view, it.ino, &post) == 0) ia = wfs_attributes(&post);           // still hard-linked
    else if (havePre) { if (pre.nlink) pre.nlink--; ia = wfs_attributes(&pre); }
    else if (it.hasLastAttrs) { wfs_attr la = it.lastAttrs; if (la.nlink) la.nlink--; ia = wfs_attributes(&la); }
    else { reply(nil, perr(EIO)); return; }
    FSItemAttributes *da = [self attrsOf:dir];
    if (!da) { reply(nil, perr(EIO)); return; }
    reply([[FSRemoveItemResult alloc] initWithItemAttributes:ia
                                         directoryAttributes:da
                                                   freeSpace:noFreeSpaceUpdate()], nil);
}

- (void)renameItem:(FSItem *)item inDirectory:(FSItem *)sourceDirectory named:(FSFileName *)sourceName
         toNewName:(FSFileName *)destinationName inDirectory:(FSItem *)destinationDirectory
          overItem:(FSItem *_Nullable)overItem context:(FSContext *)context
      replyHandler:(void (^)(FSRenameItemResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("rename ino=%llu sdir=%llu %{public}@ -> ddir=%llu %{public}@ over=%llu", W(item).ino, W(sourceDirectory).ino, sourceName.string, W(destinationDirectory).ino, destinationName.string, overItem ? W(overItem).ino : 0);
    WorldItem *it = W(item), *src = W(sourceDirectory), *dst = W(destinationDirectory);
    if (!it || !src || !dst) { reply(nil, perr(EINVAL)); return; }
    // The overwritten item disappears with the rename, so snapshot it first (same rule as remove).
    FSItemAttributes *oa = nil;
    if (overItem) {
        wfs_attr o;
        if (wfs_getattr(_view, W(overItem).ino, &o) == 0) { if (o.nlink) o.nlink--; oa = wfs_attributes(&o); }
        else if (W(overItem).hasLastAttrs) { wfs_attr la = W(overItem).lastAttrs; if (la.nlink) la.nlink--; oa = wfs_attributes(&la); }
    }
    int rc = wfs_rename(_view, src.ino, (const char *)sourceName.data.bytes, sourceName.data.length,
                        dst.ino, (const char *)destinationName.data.bytes, destinationName.data.length, it.ino);
    if (rc) { os_log_error(wfs_log(), "rename %{public}@ -> %{public}@ failed: %{errno}d", sourceName.string, destinationName.string, -rc); reply(nil, perr(rc)); return; }
    FSItemAttributes *ra = [self attrsOf:it], *sa = [self attrsOf:src], *da = [self attrsOf:dst];
    if (!ra || !sa || !da) { reply(nil, perr(EIO)); return; }
    reply([[FSRenameItemResult alloc] initWithNewName:destinationName
                                renamedItemAttributes:ra
                            sourceDirectoryAttributes:sa
                       destinationDirectoryAttributes:da
                                   overItemAttributes:oa
                                            freeSpace:noFreeSpaceUpdate()], nil);
}

#pragma mark - FSVolumeHandler: attributes

// Kept from the old frontend: if the core has already dropped the node record (-ESTALE), serve the
// last attributes it reported for this inode rather than let FSKit log an error-level line per call.
- (void)getAttributes:(FSItemGetAttributesRequest *)desired ofItem:(FSItem *)item context:(FSContext *)context
         replyHandler:(void (^)(FSGetAttributesResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("getattr ino=%llu", W(item).ino);
    WorldItem *it = W(item);
    if (!it) { reply(nil, perr(EINVAL)); return; }
    wfs_attr a;
    int rc = wfs_getattr(_view, it.ino, &a);
    if (rc == -ESTALE && it.hasLastAttrs) { a = it.lastAttrs; rc = 0; }
    else if (rc == 0) [it rememberAttrs:&a];
    if (rc) { reply(nil, perr(rc)); return; }
    reply([[FSGetAttributesResult alloc] initWithAttributes:wfs_attributes(&a)], nil);
}

- (void)setAttributes:(FSItemSetAttributesRequest *)req onItem:(FSItem *)item context:(FSContext *)context
         replyHandler:(void (^)(FSSetAttributesResult *_Nullable, NSError *_Nullable))reply {
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
    reply([[FSSetAttributesResult alloc] initWithAttributes:wfs_attributes(&a)
                                                  freeSpace:noFreeSpaceUpdate()], nil);
}

#pragma mark - FSVolumeHandler: directories and symlinks

struct PackCtx {
    FSDirectoryEntryPacker *packer;
    BOOL wantAttrs;
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

- (void)enumerateDirectory:(FSItem *)directory startingAtCookie:(FSDirectoryCookie)cookie
                  verifier:(FSDirectoryVerifier)verifier
       providingAttributes:(FSItemGetAttributesRequest *_Nullable)attributes
               usingPacker:(FSDirectoryEntryPacker *)packer context:(FSContext *)context
              replyHandler:(void (^)(FSEnumerateDirectoryResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("readdir dir=%llu cookie=%llu attrs=%d", W(directory).ino, (unsigned long long)cookie, attributes != nil);
    WorldItem *dir = W(directory);
    if (!dir) { reply(nil, perr(EINVAL)); return; }
    PackCtx c{packer, attributes != nil};
    int rc = wfs_readdir(_view, dir.ino, cookie == FSDirectoryCookieInitial ? 0 : cookie, attributes != nil, pack_entry, &c);
    if (rc) { reply(nil, perr(rc)); return; }
    reply([[FSEnumerateDirectoryResult alloc] initWithVerifier:FSDirectoryVerifierInitial], nil);
}

- (void)readSymbolicLink:(FSItem *)item context:(FSContext *)context
            replyHandler:(void (^)(FSReadSymlinkResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("readlink ino=%llu", W(item).ino);
    WorldItem *it = W(item);
    if (!it) { reply(nil, perr(EINVAL)); return; }
    char buf[PATH_MAX]; size_t len = 0;
    int rc = wfs_readlink(_view, it.ino, buf, sizeof buf, &len);
    if (rc) { reply(nil, perr(rc)); return; }
    FSItemAttributes *a = [self attrsOf:it];
    if (!a) { reply(nil, perr(EIO)); return; }
    reply([[FSReadSymlinkResult alloc] initWithContents:[FSFileName nameWithBytes:buf length:len]
                                      symlinkAttributes:a], nil);
}

#pragma mark - FSVolumeDataCacheHandler

// M0 semantics unchanged: no per-item fd, no fd cache. What this protocol buys is telling the kernel
// it may keep the page cache for this item (and, for writers, write back lazily) -- the 27 replacement
// for switching open/close off wholesale with openCloseInhibited. `wfs_nodatacache` in the store dir
// turns the protocol off, which is the old behaviour (FSKit sends us neither open nor close and
// caches as it sees fit).
- (BOOL)isDataCacheInhibited { return _inhibitDataCache; }

static FSKernelCacheCoherencyType grantFor(FSDataCacheMode m) {
    switch (m) {
    case FSDataCacheModeReadWithCache:      return FSKernelCacheCoherencyTypeReadCache;
    case FSDataCacheModeReadWriteWithCache: return FSKernelCacheCoherencyTypeWriteBack;
    case FSDataCacheModeNone:
    default:                                return FSKernelCacheCoherencyTypeNoCache;
    }
}

- (void)openItem:(FSItem *)item modes:(FSVolumeOpenModes)modes cacheMode:(FSDataCacheMode)cacheMode
         context:(FSContext *)context
    replyHandler:(void (^)(FSOpenItemResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("open ino=%llu modes=%lu cache=%ld", W(item).ino, (unsigned long)modes, (long)cacheMode);
    reply([[FSOpenItemResult alloc] initWithGrantedCoherency:grantFor(cacheMode)], nil);
}

- (void)closeItem:(FSItem *)item context:(FSContext *)context replyHandler:(void (^)(void))reply {
    WFS_TRACE("close ino=%llu", W(item).ino);
    reply();
}

- (void)upgradeItem:(FSItem *)item cacheMode:(FSDataCacheMode)cacheMode context:(FSContext *)context
       replyHandler:(void (^)(FSUpgradeItemResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("upgrade ino=%llu cache=%ld", W(item).ino, (long)cacheMode);
    reply([[FSUpgradeItemResult alloc] initWithGrantedCoherency:grantFor(cacheMode)], nil);
}

#pragma mark - FSVolumeReadWriteHandler

// Per-request backing open, exactly as in M0: ~5us against a ~70us XPC round trip, and caching fds
// per item leaks them because the kernel reclaims items lazily. See WorldVolume.mm for why the mode
// is lifted and put straight back for the EACCES case (git's write, fchmod 0444, close).
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

- (void)readFromFile:(FSItem *)item offset:(off_t)offset length:(size_t)length
          intoBuffer:(FSMutableFileDataBuffer *)buffer
        replyHandler:(void (^)(FSReadFileResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("read ino=%llu off=%lld len=%zu", W(item).ino, (long long)offset, length);
    WorldItem *it = W(item);
    if (!it) { reply(nil, perr(EINVAL)); return; }
    int fd = [self openFDFor:it writable:NO];
    if (fd < 0) { os_log_error(wfs_log(), "read ino=%llu: cannot open backing: %{errno}d", it.ino, -fd); reply(nil, perr(fd)); return; }
    ssize_t n = pread(fd, buffer.mutableBytes, MIN(length, buffer.length), offset);
    int e = errno;
    close(fd);
    if (n < 0) { os_log_error(wfs_log(), "read ino=%llu off=%lld failed: %{errno}d", it.ino, (long long)offset, e); reply(nil, perr(e)); return; }
    FSItemAttributes *a = [self attrsOf:it];
    if (!a) { reply(nil, perr(EIO)); return; }
    reply([[FSReadFileResult alloc] initWithBytesRead:(size_t)n itemAttributes:a], nil);
}

- (void)writeContents:(NSData *)contents toFile:(FSItem *)item atOffset:(off_t)offset
         replyHandler:(void (^)(FSWriteFileResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("write ino=%llu off=%lld len=%lu", W(item).ino, (long long)offset, (unsigned long)contents.length);
    WorldItem *it = W(item);
    if (!it) { reply(nil, perr(EINVAL)); return; }
    int fd = [self openFDFor:it writable:YES];
    if (fd < 0) { os_log_error(wfs_log(), "write ino=%llu: cannot open backing: %{errno}d", it.ino, -fd); reply(nil, perr(fd)); return; }
    ssize_t n = pwrite(fd, contents.bytes, contents.length, offset);
    int e = errno;
    close(fd);
    if (n < 0) { os_log_error(wfs_log(), "write ino=%llu off=%lld failed: %{errno}d", it.ino, (long long)offset, e); reply(nil, perr(e)); return; }
    FSItemAttributes *a = [self attrsOf:it];
    if (!a) { reply(nil, perr(EIO)); return; }
    reply([[FSWriteFileResult alloc] initWithBytesWritten:(size_t)n
                                           itemAttributes:a
                                                freeSpace:noFreeSpaceUpdate()], nil);
}

#pragma mark - FSVolumeXattrHandler

// Do NOT implement supportedXattrNamesForItem: / xattrOperationsInhibited -- telling the kernel an
// item has no xattrs makes it fall back to AppleDouble "._" files for com.apple.provenance on every
// create, which doubles create/unlink (measured on the old API: 548us -> 1046us).
- (void)getXattrNamed:(FSFileName *)name ofItem:(FSItem *)item context:(FSContext *)context
         replyHandler:(void (^)(FSGetXattrResult *_Nullable, NSError *_Nullable))reply {
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
    reply([[FSGetXattrResult alloc] initWithXattrValue:d], nil);
}

- (void)setXattrNamed:(FSFileName *)name toData:(NSData *_Nullable)value onItem:(FSItem *)item
               policy:(FSSetXattrPolicy)policy context:(FSContext *)context
         replyHandler:(void (^)(FSSetXattrResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("setxattr ino=%llu %{public}@", W(item).ino, name.string);
    WorldItem *it = W(item);
    if (!it) { reply(nil, perr(EINVAL)); return; }
    int rc;
    if (policy == FSSetXattrPolicyDelete) rc = wfs_removexattr(_view, it.ino, name.string.UTF8String);
    else {
        int flags = policy == FSSetXattrPolicyMustCreate ? WFS_XATTR_CREATE : policy == FSSetXattrPolicyMustReplace ? WFS_XATTR_REPLACE : 0;
        rc = wfs_setxattr(_view, it.ino, name.string.UTF8String, value.bytes, value.length, flags);
    }
    if (rc) { reply(nil, perr(rc)); return; }
    reply([[FSSetXattrResult alloc] initWithFreeSpace:noFreeSpaceUpdate()], nil);
}

- (void)listXattrsOfItem:(FSItem *)item context:(FSContext *)context
            replyHandler:(void (^)(FSListXattrsResult *_Nullable, NSError *_Nullable))reply {
    WFS_TRACE("listxattr ino=%llu", W(item).ino);
    WorldItem *it = W(item);
    if (!it) { reply(nil, perr(EINVAL)); return; }
    size_t len = 0;
    int rc = wfs_listxattr(_view, it.ino, NULL, 0, &len);
    if (rc) { reply(nil, perr(rc)); return; }
    NSMutableArray<FSFileName *> *names = [NSMutableArray new];
    if (len == 0) { reply([[FSListXattrsResult alloc] initWithXattrNames:names], nil); return; }
    NSMutableData *buf = [NSMutableData dataWithLength:len];
    rc = wfs_listxattr(_view, it.ino, (char *)buf.mutableBytes, len, &len);
    if (rc) { reply(nil, perr(rc)); return; }
    const char *b = (const char *)buf.bytes;
    size_t start = 0;
    for (size_t i = 0; i < len; ++i) if (b[i] == 0) { [names addObject:[FSFileName nameWithBytes:b + start length:i - start]]; start = i + 1; }
    reply([[FSListXattrsResult alloc] initWithXattrNames:names], nil);
}

@end
