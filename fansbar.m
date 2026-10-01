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
#import <Security/Security.h>
#include <limits.h>
#include <sys/wait.h>
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

#pragma clang diagnostic ignored "-Wdeprecated-declarations"
/* 以 root 直接 exec argv[0] (argv 以 NULL 结尾) — 无 shell 无引号无 osascript。
   密码框由 SecurityAgent 弹出。返回 0=成功(含用户授权且已启动), -1=取消/失败。
   注: 不能走 do shell script with administrator privileges — 该路径在无 TTY 的
   后台进程里会把整条命令串当成单个文件名执行 (ENOENT 127)。 */
static int run_root_argv(char *const argv[], BOOL wait_finish) {
    AuthorizationRef auth = NULL;
    if (AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
                            kAuthorizationFlagDefaults, &auth) != errAuthorizationSuccess)
        return -1;
    AuthorizationItem item = {kAuthorizationRightExecute, 0, NULL, 0};
    AuthorizationRights rights = {1, &item};
    OSStatus st = AuthorizationCopyRights(auth, &rights, NULL,
            kAuthorizationFlagInteractionAllowed | kAuthorizationFlagExtendRights, NULL);
    if (st != errAuthorizationSuccess) {
        AuthorizationFree(auth, kAuthorizationFlagDefaults);
        return -1;
    }
    FILE *pipe = NULL;
    st = AuthorizationExecuteWithPrivileges(auth, argv[0], kAuthorizationFlagDefaults,
                                            (char *const *)&argv[1], &pipe);
    AuthorizationFree(auth, kAuthorizationFlagDefaults);
    if (st != errAuthorizationSuccess) return -1;
    if (pipe) {
        if (wait_finish) {
            /* 子进程成功时静默; 有报错输出则记日志并视为失败 (按钮显示 ⚠) */
            char buf[1024]; size_t got = 0, n;
            while (got < sizeof buf - 1 && (n = fread(buf + got, 1, sizeof buf - 1 - got, pipe)) > 0)
                got += n;
            buf[got] = 0;
            fclose(pipe);
            if (got > 0) {
                FILE *lg = fopen("/tmp/fansctl.bar.log", "a");
                if (lg) { fprintf(lg, "%s", buf); fclose(lg); }
                return -1;
            }
        } else {
            fclose(pipe);
        }
    }
    /* AEWP 子进程是我们直接子进程, tick 里收割僵尸 */
    return 0;
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
    while (waitpid(-1, NULL, WNOHANG) > 0) {} /* 收割 run_root_argv 的僵尸 */
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
    UInt32 sz = (UInt32)sizeof(g_self);
    if (_NSGetExecutablePath(g_self, &sz) != 0) g_self[0] = 0;
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
    char *argv[] = {g_self, "__apply", "max", NULL};
    [self flash:run_root_argv(argv, YES) == 0 ? @"全速 ✓" : @"⚠ 授权失败"];
}
- (void)doAuto:(id)sender {
    (void)sender;
    char *argv[] = {g_self, "__apply", "auto", NULL};
    [self flash:run_root_argv(argv, YES) == 0 ? @"恢复自动 ✓" : @"⚠ 授权失败"];
}

- (void)doSmart:(id)sender {
    (void)sender;
    int pid; double slo, shi;
    char pidstr[16];
    if (smart_pid_read(&pid, &slo, &shi)) {
        /* smart 的 SIGTERM 处理器会恢复自动并清 pidfile */
        snprintf(pidstr, sizeof pidstr, "%d", pid);
        char *argv[] = {(char *)"/bin/kill", "-TERM", pidstr, NULL};
        [self flash:run_root_argv(argv, YES) == 0 ? @"智能已关闭 ✓" : @"⚠ 授权失败"];
    } else {
        int idx = thresh_idx();
        char lo[8], hi[8];
        snprintf(lo, sizeof lo, "%.0f", kThresh[idx][0]);
        snprintf(hi, sizeof hi, "%.0f", kThresh[idx][1]);
        char *argv[] = {g_self, "__smart", lo, hi, NULL};
        [self flash:run_root_argv(argv, NO) == 0 ? @"智能启动中…" : @"⚠ 授权失败"];
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
        /* fork 后父进程立刻退出, script/终端随即关闭 pty 并向会话发 SIGHUP,
           子进程若尚未 setsid 会被误杀 — 先忽略 SIGHUP 消除竞态 */
        signal(SIGHUP, SIG_IGN);
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
