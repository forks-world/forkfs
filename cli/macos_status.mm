// macOS-only: list FSKit modules via FSClient so `world fs status` shows enable state.
#import <FSKit/FSKit.h>
#import <Foundation/Foundation.h>
#include "worldfs/worldfs.h"
#include <stdio.h>

extern "C" int world_fs_status_platform(void) {
    __block int found = 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [[FSClient sharedInstance] fetchInstalledExtensionsWithCompletionHandler:^(NSArray<FSModuleIdentity *> *list, NSError *err) {
        if (err) printf("error: %s\n", err.description.UTF8String);
        for (FSModuleIdentity *m in list) {
            printf("%s  enabled=%s  %s\n", m.bundleIdentifier.UTF8String, m.enabled ? "true" : "false", m.url.path.UTF8String);
            if ([m.bundleIdentifier isEqualToString:@WFS_EXTENSION_BUNDLE_ID]) found = 1;
        }
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (!found) {
        // FSClient's public list omits third-party modules even when enabled; fall back to pluginkit.
        FILE *pk = popen("/usr/bin/pluginkit -m -p com.apple.fskit.fsmodule 2>/dev/null", "r");
        char line[512];
        while (pk && fgets(line, sizeof line, pk)) if (strstr(line, WFS_EXTENSION_BUNDLE_ID)) { found = 2; }
        if (pk) pclose(pk);
    }
    if (found == 2) { printf("%s  registered (pluginkit); enable state is only visible in System Settings\n", WFS_EXTENSION_BUNDLE_ID); return 0; }
    if (!found) {
        printf("\n%s is not registered. Run scripts/bundle.sh, then enable WorldFS in\n"
               "System Settings > General > Login Items & Extensions > File System Extensions.\n",
               WFS_EXTENSION_BUNDLE_ID);
        return 1;
    }
    return 0;
}
