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
#include "fansctl.h"

static NSStatusItem *g_item;
static NSMutableArray *g_infoItems;   /* 状态区: 第 0 行温度, 之后每风扇一行 */
static NSMenuItem *g_smartItem;
static NSMenuItem *g_threshRoot;      /* "智能阈值" 父项, 子菜单为四档预设 */
static int g_fan_n = 1;

static const double kThresh[][2] = {{60, 95}, {55, 95}, {45, 85}, {40, 80}};
#define KTHRESH_N 4

static char g_self[PATH_MAX];

static NSString *U(const char *s) { return [NSString stringWithUTF8String:s ? s : "?"]; }

#pragma clang diagnostic ignored "-Wdeprecated-declarations"
/* 授权+AEWP 核心。必须在普通子进程里跑 (fansctl __ask): 直接在 fork+setsid 的
   会话首进程里调用 AuthorizationCopyRights 会永远阻塞且不弹密码框。成功返回 0,
   子工具有报错输出时转写到自身 stderr 并返回 -1。 */
static int aewp_exec(const char *tool, char *const args[]) {
    AuthorizationRef auth = NULL;
    if (AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
                            kAuthorizationFlagDefaults, &auth) != errAuthorizationSuccess)
        return -1;
    AuthorizationItem item = {kAuthorizationRightExecute, 0, NULL, 0};
    AuthorizationRights rights = {1, &item};
    OSStatus st = AuthorizationCopyRights(auth, &rights, NULL,
            kAuthorizationFlagInteractionAllowed | kAuthorizationFlagExtendRights, NULL);
    if (st != errAuthorizationSuccess) {
        fprintf(stderr, "CopyRights 失败: %d\n", (int)st);
        AuthorizationFree(auth, kAuthorizationFlagDefaults);
        return -1;
    }
    FILE *pipe = NULL;
    st = AuthorizationExecuteWithPrivileges(auth, tool, kAuthorizationFlagDefaults,
                                            (char *const *)args, &pipe);
    AuthorizationFree(auth, kAuthorizationFlagDefaults);
    if (st != errAuthorizationSuccess) {
        fprintf(stderr, "ExecuteWithPrivileges 失败: %d\n", (int)st);
        return -1;
    }
    int rc = 0;
    if (pipe) {
        char buf[1024]; size_t got = 0, n;
        while (got < sizeof buf - 1 && (n = fread(buf + got, 1, sizeof buf - 1 - got, pipe)) > 0)
            got += n;
        buf[got] = 0;
        fclose(pipe);
        if (got > 0) { fputs(buf, stderr); rc = -1; }
    }
    return rc;
}

/* 特权动作统一入口: 优先 setuid 助手 fansctl-root (免密); 未安装时回退
   __ask 子进程授权 (每次弹密码框) */
#define FANSCTL_ROOT "/usr/local/bin/fansctl-root"

static int run_root_argv(char *const argv[]) {
    if (access(FANSCTL_ROOT, X_OK) == 0) {
        /* argv[0] 是工具路径(给 __ask/AEWP 用), helper 自身即工具, 参数从 argv[1] 起 */
        NSMutableArray *m = [NSMutableArray array];
        for (char *const *a = argv + 1; *a; a++) [m addObject:U(*a)];
        NSTask *t = [[NSTask alloc] init];
        t.launchPath = @FANSCTL_ROOT;
        t.arguments = m;
        [t launch];
        [t waitUntilExit];
        int rc = (int)t.terminationStatus;
        [t release];
        return rc == 0 ? 0 : -1;
    }
    if (!argv[0] || !g_self[0]) return -1;
    NSMutableArray *m = [NSMutableArray arrayWithObject:@"__ask"];
    for (char *const *a = argv; *a; a++) [m addObject:U(*a)];
    NSTask *t = [[NSTask alloc] init];
    t.launchPath = @(g_self);
    t.arguments = m;
    [t launch];
    [t waitUntilExit];
    int rc = (int)t.terminationStatus;
    [t release];
    return rc == 0 ? 0 : -1;
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

    /* 每风扇一行: 当前 (目标) 模式 */
    for (int f = 0; f < g_fan_n && (NSInteger)(f + 1) < [g_infoItems count]; f++) {
        double ac = -1, tg = -1;
        read_rpm_key(f, "Ac", &ac);
        read_rpm_key(f, "Tg", &tg);
        int mode = fan_mode(f);
        const char *ms = mode == 1 ? "手动" : mode == 0 ? "自动" : "未知";
        if (tg < 0) snprintf(buf, sizeof buf, "风扇%d  %6.0f rpm  %s", f, ac, ms);
        else        snprintf(buf, sizeof buf, "风扇%d  %6.0f rpm (目标 %.0f)  %s", f, ac, tg, ms);
        [(NSMenuItem *)g_infoItems[f + 1] setTitle:U(buf)];
    }

    /* 智能模式: 运行中打钩并显示当前阈值 */
    if (smart) {
        g_smartItem.title = [NSString stringWithFormat:@"智能模式 (%.0f~%.0f°C)", slo, shi];
        g_smartItem.state = NSControlStateValueOn;
        g_threshRoot.enabled = NO;
    } else {
        g_smartItem.title = @"智能模式";
        g_smartItem.state = NSControlStateValueOff;
        g_threshRoot.enabled = YES;
    }
    /* 阈值子菜单: 当前档打钩 */
    int idx = thresh_idx();
    NSMenu *sub = g_threshRoot.submenu;
    for (NSInteger i = 0; i < [sub numberOfItems]; i++)
        [sub itemAtIndex:i].state = (i == idx) ? NSControlStateValueOn : NSControlStateValueOff;
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

    g_smartItem = [[NSMenuItem alloc] initWithTitle:@"智能模式"
                                             action:@selector(doSmart:) keyEquivalent:@""];
    [g_smartItem setTarget:self];
    [menu addItem:g_smartItem];
    g_threshRoot = [[NSMenuItem alloc] initWithTitle:@"智能阈值" action:nil keyEquivalent:@""];
    NSMenu *sub = [[NSMenu alloc] init];
    for (int i = 0; i < KTHRESH_N; i++) {
        NSMenuItem *it = [[NSMenuItem alloc]
            initWithTitle:[NSString stringWithFormat:@"%.0f~%.0f°C", kThresh[i][0], kThresh[i][1]]
                    action:@selector(doThresh:) keyEquivalent:@""];
        [it setTarget:self];
        [it setTag:i];
        [sub addItem:it];
        [it release];
    }
    [g_threshRoot setSubmenu:sub];
    [menu addItem:g_threshRoot];
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
    [self flash:run_root_argv(argv) == 0 ? @"全速 ✓" : @"⚠ 授权失败"];
}
- (void)doAuto:(id)sender {
    (void)sender;
    char *argv[] = {g_self, "__apply", "auto", NULL};
    [self flash:run_root_argv(argv) == 0 ? @"恢复自动 ✓" : @"⚠ 授权失败"];
}

- (void)doSmart:(id)sender {
    (void)sender;
    int pid; double slo, shi;
    if (smart_pid_read(&pid, &slo, &shi)) {
        /* smart 的 SIGTERM 处理器会恢复自动并清 pidfile */
        char *argv[] = {g_self, "__smart", "stop", NULL};
        [self flash:run_root_argv(argv) == 0 ? @"智能已关闭 ✓" : @"⚠ 执行失败"];
    } else {
        int idx = thresh_idx();
        char lo[8], hi[8];
        snprintf(lo, sizeof lo, "%.0f", kThresh[idx][0]);
        snprintf(hi, sizeof hi, "%.0f", kThresh[idx][1]);
        char *argv[] = {g_self, "__smart", lo, hi, NULL};
        [self flash:run_root_argv(argv) == 0 ? @"智能启动中…" : @"⚠ 授权失败"];
    }
}

- (void)doThresh:(id)sender {
    [[NSUserDefaults standardUserDefaults] setInteger:[sender tag] forKey:@"SmartThreshIdx"];
    [self tick];
}

@end

static BarDelegate *g_bar_delegate; /* 无 ARC, 用全局变量持有 */

/* fansctl __ask <tool> [args...]: 在普通子进程上下文里完成授权并以 root 执行 tool */
int ask_main(int argc, char **argv) {
    if (argc < 3) return 1;
    return aewp_exec(argv[2], &argv[3]);
}

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
    {   /* 单实例: 已有活实例则静默退出, 防止重复图标 */
        FILE *f = fopen("/tmp/fansctl.bar.pid", "r");
        if (f) {
            int old = 0;
            int ok = fscanf(f, "%d", &old) == 1;
            fclose(f);
            if (ok && old != (int)getpid() && !(kill(old, 0) != 0 && errno == ESRCH)) {
                fprintf(stderr, "菜单栏已在运行 (pid %d), 本次退出\n", old);
                return 0;
            }
        }
        f = fopen("/tmp/fansctl.bar.pid", "w");
        if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
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
