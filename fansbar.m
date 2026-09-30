/*
 * fansbar - macOS 菜单栏显示最热温度与最大风扇转速
 * 显示格式: 温度°C|最大转速÷1000, 例: 67|5
 * 编译: 见 Makefile; 退出: 点菜单栏图标 -> 退出
 */
#import <AppKit/AppKit.h>
#include "fansctl.h"

static NSStatusItem *g_item;

static void bar_format(char *out, size_t sz) {
    int n = fan_count();
    if (n < 1) n = 1;
    double mx_ac = -1;
    for (int f = 0; f < n; f++) {
        double ac;
        if (read_rpm_key(f, "Ac", &ac) == 0 && ac > mx_ac) mx_ac = ac;
    }
    double T = hottest_temp(NULL);
    if (T < -999 && mx_ac < 0) { snprintf(out, sz, "--|--"); return; }
    if (T < -999) { snprintf(out, sz, "--|%.0f", mx_ac / 1000.0); return; }
    if (mx_ac < 0) { snprintf(out, sz, "%.0f|--", T); return; }
    snprintf(out, sz, "%.0f|%.0f", T, mx_ac / 1000.0);
}

@interface AppDelegateDelegate : NSObject<NSApplicationDelegate>
@property (retain) NSTimer *timer;
- (void)tick;
@end

@implementation AppDelegateDelegate
- (void)tick {
    char buf[32];
    bar_format(buf, sizeof(buf));
    g_item.button.title = [NSString stringWithUTF8String:buf];
}
- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    /* statusItemWithLength 返回不保留对象, 非 ARC 下必须手动 retain,
       否则自动释放池清空后悬空 -> 定时器触发时 SIGSEGV */
    g_item = [[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] retain];
    g_item.button.font = [NSFont monospacedDigitSystemFontOfSize:0 weight:NSFontWeightRegular];
    g_item.button.title = @"…";
    g_item.button.toolTip = @"fansctl: 最热温度°C | 最大风扇转速(krpm)";

    NSMenu *menu = [[NSMenu alloc] init];
    NSMenuItem *detail = [[NSMenuItem alloc] initWithTitle:@"fansctl 状态栏"
                                                    action:nil keyEquivalent:@""];
    [detail setEnabled:NO];
    [menu addItem:detail];
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"退出 fansbar"
                                                  action:@selector(terminate:)
                                           keyEquivalent:@"q"];
    [menu addItem:quit];
    g_item.menu = menu;

    self.timer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                                  target:self
                                                selector:@selector(tick)
                                                userInfo:nil
                                                 repeats:YES];
    [self tick];
}
@end

static AppDelegateDelegate *g_delegate; /* 无 ARC, 用全局变量持有 */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (smc_open() != 0) return 1;
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyAccessory]; /* 只在菜单栏, 不进 Dock */
    g_delegate = [AppDelegateDelegate new];
    app.delegate = g_delegate;
    [app run];
    smc_close();
    return 0;
}
