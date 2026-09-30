/*
 * fansbar.m - fansctl 的菜单栏界面 (fansctl 不带参数时进入, 见 bar_main)
 *
 * 菜单:
 *   [状态区] 最热传感器/温度/曲线百分比 + 每个风扇的转速/目标 (2 秒刷新, 只读免 root)
 *   全速 / 恢复自动        -> 授权弹窗后以 root 重新执行自身: fansctl __apply max|auto
 *   智能模式 开/关          -> root 后台运行 fansctl __smart 低 高 (pidfile 记录状态)
 *   智能阈值 切换           -> 预设循环, 存 NSUserDefaults
 *   退出 (⌘Q)
 *
 * 从终端直接运行 fansctl 时 fork 进后台并 setsid, 不占用 iTerm2。
 */
#import <AppKit/AppKit.h>
#import <mach-o/dyld.h>
#include <limits.h>
#include "fansctl.h"

static NSStatusItem *g_item;
static NSMutableArray *g_infoItems;   /* 状态区: 第 0 行温度, 之后每风扇一行 */
static NSMenuItem *g_smartItem;
static NSMenuItem *g_threshItem;
static int g_fan_n = 1;

static const double kThresh[][2] = {{60, 95}, {55, 95}, {45, 85}, {40, 80}};
#define KTHRESH_N 4

static char g_self[PATH_MAX];

static NSString *U(const char *s) { return [NSString stringWithUTF8String:s ? s : "?"]; }

static NSString *self_path(void) {
    UInt32 sz = (UInt32)sizeof(g_self);
    if (_NSGetExecutablePath(g_self, &sz) != 0) return nil;
    return U(g_self);
}

static NSString *sh_quote(NSString *s) {
    return [NSString stringWithFormat:@"'%@'",
            [s stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];
}

/* 弹管理员授权框以 root 执行 shell 命令; 失败返回 NO。
   经 NSTask 调 /usr/bin/osascript (命令行 osascript 从无 Info.plist 的
   后台进程弹授权窗已验证可用; 而进程内 NSAppleScript 会静默失败)。
   注意: AppleScript 字符串只认双引号, 不能直接用 sh_quote 的单引号结果。 */
static BOOL run_root(NSString *shell) {
    NSString *escaped = [[sh_quote(shell)
        stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"]
        stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
    NSString *script = [NSString stringWithFormat:
        @"do shell script \"%@\" with administrator privileges", escaped];
    NSTask *t = [[NSTask alloc] init];
    t.launchPath = @"/usr/bin/osascript";
    t.arguments = @[@"-e", script];
    [t launch];
    [t waitUntilExit];
    BOOL ok = t.terminationStatus == 0;
    [t release];
    return ok;
}

static int thresh_idx(void) {
    NSInteger i = [[NSUserDefaults standardUserDefaults] integerForKey:@"SmartThreshIdx"];
    if (i < 0 || i >= KTHRESH_N) i = 0;
    return (int)i;
}

@interface BarDelegate : NSObject<NSApplicationDelegate>
@property (retain) NSTimer *timer;
- (void)tick;
- (void)doMax:(id)sender;
- (void)doAuto:(id)sender;
- (void)doSmart:(id)sender;
- (void)doThresh:(id)sender;
@end

@implementation BarDelegate

- (void)tick {
    int pid = 0; double slo = 0, shi = 0;
    int smart = smart_pid_read(&pid, &slo, &shi);

    /* 标题栏: 温度|最大转速 */
    double mx_ac = -1;
    for (int f = 0; f < g_fan_n; f++) {
        double ac;
        if (read_rpm_key(f, "Ac", &ac) == 0 && ac > mx_ac) mx_ac = ac;
    }
    const char *tkey = NULL;
    double T = hottest_temp(&tkey);
    char buf[48];
    if (T < -999 && mx_ac < 0) snprintf(buf, sizeof buf, "--|--");
    else if (T < -999)         snprintf(buf, sizeof buf, "--|%.0f", mx_ac / 1000.0);
    else if (mx_ac < 0)        snprintf(buf, sizeof buf, "%.0f|--", T);
    else                       snprintf(buf, sizeof buf, "%.0f|%.0f", T, mx_ac / 1000.0);
    g_item.button.title = U(buf);

    /* 状态区第 0 行: 最热传感器 + 曲线百分比 */
    if (T > -999) {
        if (smart) {
            double frac = (T - slo) / (shi - slo);
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            snprintf(buf, sizeof buf, "%s %.1f°C  曲线%.0f%%", tkey, T, frac * 100);
        } else {
            snprintf(buf, sizeof buf, "%s %.1f°C", tkey, T);
        }
    } else {
        snprintf(buf, sizeof buf, "温度读取失败");
    }
    if ([g_infoItems count] > 0)
        [(NSMenuItem *)g_infoItems[0] setTitle:U(buf)];

    /* 每风扇一行: 当前 (目标) */
    for (int f = 0; f < g_fan_n && (NSInteger)(f + 1) < [g_infoItems count]; f++) {
        double ac = -1, tg = -1;
        read_rpm_key(f, "Ac", &ac);
        read_rpm_key(f, "Tg", &tg);
        if (tg < 0) snprintf(buf, sizeof buf, "风扇%d  %6.0f rpm", f, ac);
        else        snprintf(buf, sizeof buf, "风扇%d  %6.0f rpm (目标 %.0f)", f, ac, tg);
        [(NSMenuItem *)g_infoItems[f + 1] setTitle:U(buf)];
    }

    /* 智能模式菜单项 */
    if (smart) {
        g_smartItem.title = [NSString stringWithFormat:@"智能模式 (%.0f~%.0f°C) 运行中 — 点击关闭", slo, shi];
        g_smartItem.state = NSControlStateValueOn;
        g_threshItem.enabled = NO;
    } else {
        g_smartItem.title = @"智能模式 — 点击开启";
        g_smartItem.state = NSControlStateValueOff;
        g_threshItem.enabled = YES;
    }
    int idx = thresh_idx();
    g_threshItem.title = [NSString stringWithFormat:
        @"智能阈值: %.0f~%.0f°C (点击切换)", kThresh[idx][0], kThresh[idx][1]];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    /* statusItemWithLength 返回不保留对象, 非 ARC 下必须手动 retain,
       否则自动释放池清空后悬空 -> 定时器触发时 SIGSEGV */
    g_item = [[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] retain];
    g_item.button.font = [NSFont monospacedDigitSystemFontOfSize:0 weight:NSFontWeightRegular];
    g_item.button.title = @"…";
    g_item.button.toolTip = @"fansctl: 最热温度°C | 最大风扇转速(krpm)";

    g_fan_n = fan_count();
    if (g_fan_n < 1) g_fan_n = 1;
    if (g_fan_n > 4) g_fan_n = 4;

    NSMenu *menu = [[NSMenu alloc] init];
    g_infoItems = [[NSMutableArray alloc] init];

    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:@"读取中…" action:nil keyEquivalent:@""];
    [it setEnabled:NO];
    [menu addItem:it];
    [g_infoItems addObject:it];
    [it release];
    for (int f = 0; f < g_fan_n; f++) {
        it = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"风扇%d", f]
                                        action:nil keyEquivalent:@""];
        [it setEnabled:NO];
        [menu addItem:it];
        [g_infoItems addObject:it];
        [it release];
    }
    [menu addItem:[NSMenuItem separatorItem]];

    it = [[NSMenuItem alloc] initWithTitle:@"全速" action:@selector(doMax:) keyEquivalent:@""];
    [it setTarget:self];
    [menu addItem:it];
    [it release];
    it = [[NSMenuItem alloc] initWithTitle:@"恢复自动" action:@selector(doAuto:) keyEquivalent:@""];
    [it setTarget:self];
    [menu addItem:it];
    [it release];
    [menu addItem:[NSMenuItem separatorItem]];

    g_smartItem = [[NSMenuItem alloc] initWithTitle:@"智能模式 — 点击开启"
                                             action:@selector(doSmart:) keyEquivalent:@""];
    [g_smartItem setTarget:self];
    [menu addItem:g_smartItem];
    g_threshItem = [[NSMenuItem alloc] initWithTitle:@"智能阈值" action:@selector(doThresh:) keyEquivalent:@""];
    [g_threshItem setTarget:self];
    [menu addItem:g_threshItem];
    [menu addItem:[NSMenuItem separatorItem]];

    it = [[NSMenuItem alloc] initWithTitle:@"退出 fansctl" action:@selector(terminate:) keyEquivalent:@"q"];
    [menu addItem:it];
    [it release];
    g_item.menu = menu;

    self.timer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                                  target:self
                                                selector:@selector(tick)
                                                userInfo:nil
                                                 repeats:YES];
    [self tick];
}

- (void)flash:(NSString *)s { g_item.button.title = s; /* ≤2s 后 tick 自动刷新 */ }

- (void)doMax:(id)sender {
    (void)sender;
    [self flash:run_root([NSString stringWithFormat:@"%@ __apply max", sh_quote(self_path())])
           ? @"全速 ✓" : @"⚠ 授权失败"];
}
- (void)doAuto:(id)sender {
    (void)sender;
    [self flash:run_root([NSString stringWithFormat:@"%@ __apply auto", sh_quote(self_path())])
           ? @"恢复自动 ✓" : @"⚠ 授权失败"];
}

- (void)doSmart:(id)sender {
    (void)sender;
    int pid; double slo, shi;
    if (smart_pid_read(&pid, &slo, &shi)) {
        /* smart 的 SIGTERM 处理器会恢复自动并清 pidfile */
        [self flash:run_root([NSString stringWithFormat:@"kill -TERM %d", pid])
               ? @"智能已关闭 ✓" : @"⚠ 授权失败"];
    } else {
        int idx = thresh_idx();
        NSString *sh = [NSString stringWithFormat:
            @"nohup %@ __smart %.0f %.0f >/dev/null 2>&1 &",
            sh_quote(self_path()), kThresh[idx][0], kThresh[idx][1]];
        [self flash:run_root(sh) ? @"智能启动中…" : @"⚠ 授权失败"];
    }
}

- (void)doThresh:(id)sender {
    (void)sender;
    int idx = (thresh_idx() + 1) % KTHRESH_N;
    [[NSUserDefaults standardUserDefaults] setInteger:idx forKey:@"SmartThreshIdx"];
    [self tick];
}

@end

static BarDelegate *g_bar_delegate; /* 无 ARC, 用全局变量持有 */

/* fansctl 无参数入口: fork 进后台, 不占用终端 */
int bar_main(void) {
    if (isatty(STDIN_FILENO)) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) return 0; /* 父进程立即返回, 终端释放 */
        setsid();
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/tmp/fansctl.bar.log", "a", stderr); /* 排查日志 */
    }
    if (smc_open() != 0) return 1;
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyAccessory]; /* 只在菜单栏, 不进 Dock */
    g_bar_delegate = [BarDelegate new];
    app.delegate = g_bar_delegate;
    [app run];
    smc_close();
    return 0;
}
