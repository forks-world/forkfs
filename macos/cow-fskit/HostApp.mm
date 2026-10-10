#import <Cocoa/Cocoa.h>
extern NSString *ForkRevisionResourceName(NSURL *url);
@interface ForkRevisionHost : NSObject <NSApplicationDelegate>
@property(nonatomic,strong) NSWindow *window;
@property(nonatomic,strong) NSTextField *status;
@end
@implementation ForkRevisionHost
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,560,220)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    self.window.title=@"Forkfs Revision";
    auto title=[NSTextField labelWithString:@"只读历史文件系统预览"];title.frame=NSMakeRect(24,165,510,24);
    auto description=[NSTextField wrappingLabelWithString:@"扩展已随应用打包。签名后需在系统设置中启用。此预览暂不提供挂载，也不改变仓库内容。"];description.frame=NSMakeRect(24,105,510,50);
    self.status=[NSTextField wrappingLabelWithString:@"选择仓库以检查其 revision 资源描述。"];self.status.frame=NSMakeRect(24,55,510,40);
    auto button=[NSButton buttonWithTitle:@"选择仓库" target:self action:@selector(selectRepository:)];button.frame=NSMakeRect(24,15,120,30);
    for(NSView *view in @[title,description,self.status,button])[self.window.contentView addSubview:view];
    [self.window center];[self.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
}
- (void)selectRepository:(id)sender {
    auto panel=[NSOpenPanel openPanel];panel.canChooseFiles=NO;panel.canChooseDirectories=YES;panel.allowsMultipleSelection=NO;
    if([panel runModal]!=NSModalResponseOK)return;
    NSURL *url=panel.URL;BOOL scoped=[url startAccessingSecurityScopedResource];
    NSString *revision=ForkRevisionResourceName(url);
    if(scoped)[url stopAccessingSecurityScopedResource];
    self.status.stringValue=revision?[NSString stringWithFormat:@"资源描述有效：%@（尚未挂载）",revision]:@"未找到有效的 revision 资源描述。";
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {return YES;}
@end
int main(int argc,const char **argv){@autoreleasepool {
    auto app=[NSApplication sharedApplication];auto delegate=[ForkRevisionHost new];app.delegate=delegate;
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];[app run];return 0;
}}
