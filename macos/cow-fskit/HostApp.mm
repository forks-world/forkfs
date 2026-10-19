#import <Cocoa/Cocoa.h>
#import "RevisionFileSystem.h"
@interface ForkRevisionHost : NSObject <NSApplicationDelegate>
@property(nonatomic,strong) NSWindow *window;
@property(nonatomic,strong) NSTextField *status;
@property(nonatomic,strong) NSButton *mountButton;
@property(nonatomic,strong) NSURL *selectedURL;
@property(nonatomic) BOOL scoped;
@property(nonatomic) BOOL mounting;
@end
@implementation ForkRevisionHost
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,620,250)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    self.window.title=@"Forkfs 文件系统";
    auto title=[NSTextField labelWithString:@"Forkfs 挂载"];title.frame=NSMakeRect(24,200,570,24);
    auto description=[NSTextField wrappingLabelWithString:@"选择已有仓库，然后挂载其 World 或 revision。需要已启用的 Forkfs 文件系统扩展。"];description.frame=NSMakeRect(24,140,570,50);
    self.status=[NSTextField wrappingLabelWithString:@"尚未选择仓库或挂载。"];self.status.frame=NSMakeRect(24,65,570,65);
    auto button=[NSButton buttonWithTitle:@"选择仓库" target:self action:@selector(selectRepository:)];button.frame=NSMakeRect(24,20,120,30);
    self.mountButton=[NSButton buttonWithTitle:@"挂载" target:self action:@selector(mountRepository:)];self.mountButton.frame=NSMakeRect(160,20,100,30);self.mountButton.enabled=NO;
    auto settings=[NSButton buttonWithTitle:@"文件系统扩展设置" target:self action:@selector(openSettings:)];settings.frame=NSMakeRect(280,20,180,30);
    for(NSView *view in @[title,description,self.status,button,self.mountButton,settings])[self.window.contentView addSubview:view];
    [self.window center];[self.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
}
- (void)releaseSelection {
    if(self.scoped)[self.selectedURL stopAccessingSecurityScopedResource];self.scoped=NO;self.selectedURL=nil;
}
- (void)selectRepository:(id)sender {
    if(self.mounting)return;
    auto panel=[NSOpenPanel openPanel];panel.canChooseFiles=NO;panel.canChooseDirectories=YES;panel.allowsMultipleSelection=NO;
    if([panel runModal]!=NSModalResponseOK)return;
    [self releaseSelection];NSURL *url=panel.URL;BOOL scoped=[url startAccessingSecurityScopedResource];
    ForkFSResourceSelection *selection=[ForkFSResourceSelection selectionAtURL:url];
    if(!selection){if(scoped)[url stopAccessingSecurityScopedResource];self.mountButton.enabled=NO;self.status.stringValue=@"未找到唯一有效的 World 或 revision 资源描述。";return;}
    self.selectedURL=url;self.scoped=scoped;self.mountButton.enabled=YES;
    self.status.stringValue=[NSString stringWithFormat:@"已选择 %@ %@，尚未挂载。",selection.readOnly?@"revision":@"World",selection.name];
}
- (void)mountRepository:(id)sender {
    if(!self.selectedURL || self.mounting)return;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
    if(@available(macOS 27.0,*)){
        // The selected path grants backing access to FSKit, never to a workload.
        auto resource=[[FSPathURLResource alloc] initWithURL:self.selectedURL writable:YES];
        self.mounting=YES;self.mountButton.enabled=NO;self.status.stringValue=@"正在请求系统挂载……";
        [[FSClient sharedInstance] mountSingleVolumeForResource:resource bundleID:@"world.forks.cow.revision.extension" options:@[]
            completionHandler:^(NSURL *mountPath,NSError *error){dispatch_async(dispatch_get_main_queue(),^{
                self.mounting=NO;self.mountButton.enabled=YES;
                NSError *failure=error;
                if(!failure && !mountPath)failure=[NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadUnknownError userInfo:@{NSLocalizedDescriptionKey:@"系统没有返回挂载路径。"}];
                self.status.stringValue=failure?[NSString stringWithFormat:@"挂载失败：%@（%@ / %ld）",failure.localizedDescription,failure.domain,(long)failure.code]:[NSString stringWithFormat:@"已挂载：%@",mountPath.path];
            });}];
    }else
#endif
    self.status.stringValue=@"此挂载入口需要 macOS 27 SDK 构建及 macOS 27 或更高版本。";
}
- (void)openSettings:(id)sender {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
    if(@available(macOS 27.0,*)){
        if(![[FSClient sharedInstance] openFileSystemExtensionsSettings])self.status.stringValue=@"无法打开文件系统扩展设置。";
    }else
#endif
    self.status.stringValue=@"请在系统设置中手动检查文件系统扩展。";
}
- (void)applicationWillTerminate:(NSNotification *)notification {[self releaseSelection];}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {return YES;}
@end
int main(int argc,const char **argv){@autoreleasepool {
    auto app=[NSApplication sharedApplication];auto delegate=[ForkRevisionHost new];app.delegate=delegate;
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];[app run];return 0;
}}
