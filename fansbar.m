/*
 * fansbar.m - fansctl 的菜单栏界面 (fansctl 不带参数时进入, 见 bar_main)
 *
 * 菜单:
 *   [状态区] 最热传感器/温度/曲线百分比或恒温目标 + 每风扇转速/目标 (2 秒刷新)
 *   全速 / 自动 / 智能模式 / 恒温模式 -> 四态互斥打钩; 点任意控制项先接管
 *   (停掉运行中的智能/恒温)再应用
 *   智能阈值 / 恒温目标 -> 四档预设子菜单(存 NSUserDefaults), 运行中可热更新
 *   语言 -> 中/英双语, 默认跟系统语言, 手动切换即重建菜单 (CLI 与日志不经过此处)
 *   退出 (⌘Q): 无守护进程且风扇手动时先恢复自动
 * 特权动作走 run_root_argv: 优先 setuid 助手 fansctl-root(免密), 未安装时
 * 回退 __ask 子进程授权弹窗。动作完成后自动重开下拉, 菜单开着也持续刷新。
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
static NSMenuItem *g_maxItem;
static NSMenuItem *g_autoItem;
static NSMenuItem *g_smartItem;
static NSMenuItem *g_threshRoot;      /* "智能阈值" 父项, 子菜单为四档预设 */
static NSMenuItem *g_holdItem;
static NSMenuItem *g_holdTargetRoot;  /* "恒温目标" 父项, 子菜单为四档预设 */
static NSMenuItem *g_langRoot;        /* "语言" 父项, 子菜单 中文/English */
static int g_fan_n = 1;

/* ---- 界面双语 ---- */
static int g_lang_en; /* 0=中文 1=English */

/* L(中文, English): 按当前语言取一侧。字符串少不值得 .lproj 资源包, 就地内联 */
static const char *L(const char *zh, const char *en) { return g_lang_en ? en : zh; }

/* MenuLang: 0=跟系统(默认) 1=手动中文 2=手动英文, 存 NSUserDefaults */
static void lang_init(void) {
    NSInteger pref = [[NSUserDefaults standardUserDefaults] integerForKey:@"MenuLang"];
    if (pref == 1) { g_lang_en = 0; return; }
    if (pref == 2) { g_lang_en = 1; return; }
    NSString *first = [[NSLocale preferredLanguages] firstObject];
    g_lang_en = first && ![first hasPrefix:@"zh"] ? 1 : 0;
}

static const double kThresh[][2] = {{60, 95}, {55, 95}, {45, 85}, {40, 80}};
#define KTHRESH_N ((int)(sizeof kThresh / sizeof kThresh[0]))
static const double kHoldTarget[] = {65, 70, 75, 80};
#define KHOLD_N ((int)(sizeof kHoldTarget / sizeof kHoldTarget[0]))

static char g_self[PATH_MAX];

static NSString *U(const char *s) { return [NSString stringWithUTF8String:s ? s : "?"]; }

/* 电池充电功率 W: 复用 fansctl.h 的 power_read (PSTR 是适配器输入功率,
   含充电分量——那部分能量存进电池而非机器消耗, 显示时扣除) */
static double read_charge_watts(void) {
    struct power_info p;
    if (power_read(&p) != 0) return 0;
    return p.charge_w;
}

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
    /* argv[0] 是工具路径(给 __ask/AEWP 用); setuid 助手自身即工具, 参数从 argv+1 起 */
    int helper = access(FANSCTL_ROOT, X_OK) == 0;
    if (!helper && (!argv[0] || !g_self[0])) return -1;
    NSMutableArray *m = [NSMutableArray array];
    if (!helper) [m addObject:@"__ask"];
    for (char *const *a = argv + (helper ? 1 : 0); *a; a++) [m addObject:U(*a)];
    NSTask *t = [[NSTask alloc] init];
    t.launchPath = helper ? @FANSCTL_ROOT : @(g_self);
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

static int hold_idx(void) {
    NSInteger i = [[NSUserDefaults standardUserDefaults] integerForKey:@"HoldTargetIdx"];
    if (i < 0 || i >= KHOLD_N) i = 1; /* 默认 70°C */
    return (int)i;
}

@interface BarDelegate : NSObject<NSApplicationDelegate>
@property (retain) NSTimer *timer;
- (void)tick;
- (void)doMax:(id)sender;
- (void)doAuto:(id)sender;
- (void)doSmart:(id)sender;
- (void)doThresh:(id)sender;
- (void)doHold:(id)sender;
- (void)doHoldTarget:(id)sender;
- (void)doLang:(id)sender;
- (void)doQuit:(id)sender;
- (NSMenu *)buildMenu;
- (void)rebuildMenu;
@end

@implementation BarDelegate

/* 菜单栏应用事实上活到老进程被杀, dealloc 不会真正跑, 但非 ARC 下语义要正确
   (clang --analyze 会查), 也防将来被别处的代码 retain/release 玩坏 */
- (void)dealloc {
    [_timer release];
    [super dealloc];
}

- (void)tick {
    int pid = 0; double slo = 0, shi = 0;
    int smart = smart_pid_read(&pid, &slo, &shi);
    double ht = 0;
    int hold = hold_pid_read(&pid, &ht);

    /* 标题栏: 温度|最大转速 */
    double mx_ac = -1;
    for (int f = 0; f < g_fan_n; f++) {
        double ac;
        if (read_rpm_key(f, "Ac", &ac) == 0 && ac > mx_ac) mx_ac = ac;
    }
    const char *tkey = NULL;
    double T = hottest_temp(&tkey);
    char tb[8] = "--", fb[8] = "--", buf[64];
    if (T > -999) snprintf(tb, sizeof tb, "%.0f", T);
    if (mx_ac >= 0) snprintf(fb, sizeof fb, "%.0f", mx_ac / 1000.0);
    snprintf(buf, sizeof buf, "%s|%s", tb, fb);
    g_item.button.title = U(buf);

    /* 状态区第 0 行: 最热传感器/曲线或恒温目标 + 整机功率。
       PSTR = 系统输入功率 W (与 ioreg SystemPowerIn 同源), 无此键的机器不显示 */
    double pw = -1;
    read_key_value("PSTR", NULL, &pw, NULL);
    int off = 0;
    if (T > -999) {
        if (smart) {
            double frac = (T - slo) / (shi - slo);
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            off = snprintf(buf, sizeof buf, L("%s %.1f°C  曲线%.0f%%", "%s %.1f°C  curve %.0f%%"),
                           tkey, T, frac * 100);
        } else if (hold) {
            off = snprintf(buf, sizeof buf, L("%s %.1f°C  目标%.0f°C", "%s %.1f°C  target %.0f°C"),
                           tkey, T, ht);
        } else {
            off = snprintf(buf, sizeof buf, "%s %.1f°C", tkey, T);
        }
    } else {
        off = snprintf(buf, sizeof buf, "%s", L("温度读取失败", "temp read failed"));
    }
    if (pw > 0 && pw < 1000 && off > 0 && (size_t)off < sizeof buf - 20) {
        double cw = read_charge_watts(); /* 充电分量: 入电池, 非机器消耗 */
        if (cw > 0 && pw - cw > 0)
            snprintf(buf + off, sizeof buf - (size_t)off,
                     L("  %.0fW+%.0fW充", "  %.0fW+%.0fW chg"), pw - cw, cw);
        else if (pw - cw > 0)
            snprintf(buf + off, sizeof buf - (size_t)off, "  %.0fW", pw - cw);
    }
    if ([g_infoItems count] > 0)
        [(NSMenuItem *)g_infoItems[0] setTitle:U(buf)];

    /* 每风扇一行: 当前 (目标) 模式; 同时统计控制状态 */
    int n_manual = 0, n_at_max = 0;
    for (int f = 0; f < g_fan_n && (NSInteger)(f + 1) < [g_infoItems count]; f++) {
        double ac = -1, tg = -1, mx = -1;
        read_rpm_key(f, "Ac", &ac);
        read_rpm_key(f, "Tg", &tg);
        read_rpm_key(f, "Mx", &mx);
        int mode = fan_mode(f);
        if (mode == 1) {
            n_manual++;
            if (mx > 0 && tg >= mx - 50) n_at_max++;
        }
        const char *ms = mode == 1 ? L("手动", "Manual") : mode == 0 ? L("自动", "Auto") : L("未知", "?");
        if (tg < 0) snprintf(buf, sizeof buf, L("风扇%d  %6.0f rpm  %s", "Fan %d  %6.0f rpm  %s"), f, ac, ms);
        else        snprintf(buf, sizeof buf, L("风扇%d  %6.0f rpm (目标 %.0f)  %s",
                                                "Fan %d  %6.0f rpm (target %.0f)  %s"), f, ac, tg, ms);
        [(NSMenuItem *)g_infoItems[f + 1] setTitle:U(buf)];
    }

    /* 四种模式互斥打钩: 当前生效的状态 */
    g_smartItem.state = smart ? NSControlStateValueOn : NSControlStateValueOff;
    g_holdItem.state  = hold ? NSControlStateValueOn : NSControlStateValueOff;
    g_maxItem.state  = (!smart && !hold && n_manual == g_fan_n && n_at_max == g_fan_n)
                       ? NSControlStateValueOn : NSControlStateValueOff;
    g_autoItem.state = (!smart && !hold && n_manual == 0)
                       ? NSControlStateValueOn : NSControlStateValueOff;

    /* 智能模式: 运行中显示当前阈值 (阈值可运行中热更新) */
    if (smart) {
        g_smartItem.title = [NSString stringWithFormat:
            U(L("智能模式 (%.0f~%.0f°C)", "Smart (%.0f~%.0f°C)")), slo, shi];
    } else {
        g_smartItem.title = U(L("智能模式", "Smart"));
    }
    g_threshRoot.enabled = YES;
    /* 阈值子菜单: 当前档打钩 */
    int idx = thresh_idx();
    NSMenu *sub = g_threshRoot.submenu;
    for (NSInteger i = 0; i < [sub numberOfItems]; i++)
        [sub itemAtIndex:i].state = (i == idx) ? NSControlStateValueOn : NSControlStateValueOff;

    /* 恒温模式: 运行中显示当前目标 (可运行中热更新) */
    if (hold) {
        g_holdItem.title = [NSString stringWithFormat:
            U(L("恒温模式 (→%.0f°C)", "Thermostat (→%.0f°C)")), ht];
    } else {
        g_holdItem.title = U(L("恒温模式", "Thermostat"));
    }
    g_holdTargetRoot.enabled = YES;
    idx = hold_idx();
    sub = g_holdTargetRoot.submenu;
    for (NSInteger i = 0; i < [sub numberOfItems]; i++)
        [sub itemAtIndex:i].state = (i == idx) ? NSControlStateValueOn : NSControlStateValueOff;

    /* 语言子菜单: 当前语言打钩 (0=中文 1=English) */
    sub = g_langRoot.submenu;
    for (NSInteger i = 0; i < [sub numberOfItems]; i++)
        [sub itemAtIndex:i].state = (i == g_lang_en) ? NSControlStateValueOn : NSControlStateValueOff;
}

/* 构建整个下拉菜单 (重建安全: 所有句柄全局变量在此全部重新赋值) */
- (NSMenu *)buildMenu {
    g_item.button.toolTip = U(L("fansctl: 最热温度°C | 最大风扇转速(krpm)",
                                "fansctl: hottest temp °C | max fan rpm (krpm)"));
    NSMenu *menu = [[NSMenu alloc] init];
    [g_infoItems removeAllObjects];

    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:U(L("读取中…", "Loading…"))
                                                action:nil keyEquivalent:@""];
    [it setEnabled:NO];
    [menu addItem:it];
    [g_infoItems addObject:it];
    [it release];
    for (int f = 0; f < g_fan_n; f++) {
        it = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:U(L("风扇%d", "Fan %d")), f]
                                        action:nil keyEquivalent:@""];
        [it setEnabled:NO];
        [menu addItem:it];
        [g_infoItems addObject:it];
        [it release];
    }
    [menu addItem:[NSMenuItem separatorItem]];

    g_maxItem = [[NSMenuItem alloc] initWithTitle:U(L("全速", "Max"))
                                           action:@selector(doMax:) keyEquivalent:@""];
    [g_maxItem setTarget:self];
    [menu addItem:g_maxItem];
    g_autoItem = [[NSMenuItem alloc] initWithTitle:U(L("自动", "Auto"))
                                            action:@selector(doAuto:) keyEquivalent:@""];
    [g_autoItem setTarget:self];
    [menu addItem:g_autoItem];
    [menu addItem:[NSMenuItem separatorItem]];

    g_smartItem = [[NSMenuItem alloc] initWithTitle:U(L("智能模式", "Smart"))
                                             action:@selector(doSmart:) keyEquivalent:@""];
    [g_smartItem setTarget:self];
    [menu addItem:g_smartItem];
    g_threshRoot = [[NSMenuItem alloc] initWithTitle:U(L("智能阈值", "Smart thresholds"))
                                              action:nil keyEquivalent:@""];
    NSMenu *sub = [[NSMenu alloc] init];
    for (int i = 0; i < KTHRESH_N; i++) {
        NSMenuItem *ti = [[NSMenuItem alloc]
            initWithTitle:[NSString stringWithFormat:@"%.0f~%.0f°C", kThresh[i][0], kThresh[i][1]]
                    action:@selector(doThresh:) keyEquivalent:@""];
        [ti setTarget:self];
        [ti setTag:i];
        [sub addItem:ti];
        [ti release];
    }
    [g_threshRoot setSubmenu:sub];
    [sub release];
    [menu addItem:g_threshRoot];

    g_holdItem = [[NSMenuItem alloc] initWithTitle:U(L("恒温模式", "Thermostat"))
                                            action:@selector(doHold:) keyEquivalent:@""];
    [g_holdItem setTarget:self];
    [menu addItem:g_holdItem];
    g_holdTargetRoot = [[NSMenuItem alloc] initWithTitle:U(L("恒温目标", "Thermostat target"))
                                                  action:nil keyEquivalent:@""];
    sub = [[NSMenu alloc] init];
    for (int i = 0; i < KHOLD_N; i++) {
        NSMenuItem *hit = [[NSMenuItem alloc]
            initWithTitle:[NSString stringWithFormat:@"%.0f°C", kHoldTarget[i]]
                    action:@selector(doHoldTarget:) keyEquivalent:@""];
        [hit setTarget:self];
        [hit setTag:i];
        [sub addItem:hit];
        [hit release];
    }
    [g_holdTargetRoot setSubmenu:sub];
    [sub release];
    [menu addItem:g_holdTargetRoot];
    [menu addItem:[NSMenuItem separatorItem]];

    /* 语言子菜单: 两个选项各用自家文字, 不翻译 */
    g_langRoot = [[NSMenuItem alloc] initWithTitle:U(L("语言", "Language"))
                                            action:nil keyEquivalent:@""];
    sub = [[NSMenu alloc] init];
    NSMenuItem *zh = [[NSMenuItem alloc] initWithTitle:@"中文"
                                                action:@selector(doLang:) keyEquivalent:@""];
    [zh setTarget:self];
    [zh setTag:0];
    [sub addItem:zh];
    [zh release];
    NSMenuItem *en = [[NSMenuItem alloc] initWithTitle:@"English"
                                                action:@selector(doLang:) keyEquivalent:@""];
    [en setTarget:self];
    [en setTag:1];
    [sub addItem:en];
    [en release];
    [g_langRoot setSubmenu:sub];
    [sub release];
    [menu addItem:g_langRoot];

    it = [[NSMenuItem alloc] initWithTitle:U(L("退出 fansctl", "Quit fansctl"))
                                    action:@selector(doQuit:) keyEquivalent:@"q"];
    [menu addItem:it];
    [it release];
    return [menu autorelease]; /* 按命名约定返回 +0, 所有权在接收方 */
}

/* 换菜单: setter 先保留新菜单再释放旧菜单; buildMenu 返回 +0, 此处不再 release */
- (void)rebuildMenu {
    g_item.menu = [self buildMenu];
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

    g_fan_n = fan_count();
    if (g_fan_n < 1) g_fan_n = 1;
    if (g_fan_n > 4) g_fan_n = 4;

    g_infoItems = [[NSMutableArray alloc] init];
    [self rebuildMenu];

    /* 菜单展开时主线程处于 NSEventTrackingRunLoopMode, 默认模式的 NSTimer 不触发,
       打钩会"冻结"。挂到公共模式, 菜单开着也持续 2 秒刷新 */
    NSTimer *t = [NSTimer timerWithTimeInterval:2.0 target:self selector:@selector(tick)
                                      userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
    self.timer = t;
    [self tick];
}

- (void)flash:(NSString *)s { g_item.button.title = s; /* ≤2s 后 tick 自动刷新 */ }

/* NSMenu 点任何项都会收起下拉; 动作完成后短暂延迟重新打开,
   效果上"点了不消失", 可连续切换并看到钩子变化 */
- (void)reopenMenu {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.15 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [g_item.button performClick:nil]; });
}

/* 点全速/自动/智能/恒温 = 接管控制: 先停掉运行中的其他控制器(智能/恒温),
   否则它们每秒改回目标转速 */
- (void)takeover:(void (^)(void))apply {
    int pid; double slo, shi;
    if (smart_pid_read(&pid, &slo, &shi)) {
        char *stop[] = {g_self, "__smart", "stop", NULL};
        if (run_root_argv(stop) != 0) { [self flash:U(L("⚠ 停止智能失败", "⚠ failed to stop Smart"))]; return; }
    }
    double t;
    if (hold_pid_read(&pid, &t)) {
        char *stop[] = {g_self, "__hold", "stop", NULL};
        if (run_root_argv(stop) != 0) { [self flash:U(L("⚠ 停止恒温失败", "⚠ failed to stop Thermostat"))]; return; }
    }
    apply();
}

- (void)doMax:(id)sender {
    (void)sender;
    [self takeover:^{
        char *argv[] = {g_self, "__apply", "max", NULL};
        int ok = run_root_argv(argv) == 0;
        [self tick]; /* 立即刷新打钩, 重开的菜单直接显示新状态 */
        [self flash:ok ? U(L("全速 ✓", "Max ✓")) : U(L("⚠ 执行失败", "⚠ failed"))];
    }];
    [self reopenMenu];
}
- (void)doAuto:(id)sender {
    (void)sender;
    [self takeover:^{
        char *argv[] = {g_self, "__apply", "auto", NULL};
        int ok = run_root_argv(argv) == 0;
        [self tick];
        [self flash:ok ? U(L("已恢复自动 ✓", "Auto ✓")) : U(L("⚠ 执行失败", "⚠ failed"))];
    }];
    [self reopenMenu];
}

- (void)doSmart:(id)sender {
    (void)sender;
    int pid; double slo, shi;
    if (smart_pid_read(&pid, &slo, &shi)) {
        /* smart 的 SIGTERM 处理器会恢复自动并清 pidfile */
        char *argv[] = {g_self, "__smart", "stop", NULL};
        int ok = run_root_argv(argv) == 0;
        [self tick];
        [self flash:ok ? U(L("智能已关闭 ✓", "Smart off ✓")) : U(L("⚠ 执行失败", "⚠ failed"))];
    } else {
        int idx = thresh_idx();
        char lo[8], hi[8];
        snprintf(lo, sizeof lo, "%.0f", kThresh[idx][0]);
        snprintf(hi, sizeof hi, "%.0f", kThresh[idx][1]);
        char *argv[] = {g_self, "__smart", lo, hi, NULL};
        int ok = run_root_argv(argv) == 0;
        [self tick];
        [self flash:ok ? U(L("智能启动中…", "Smart starting…")) : U(L("⚠ 执行失败", "⚠ failed"))];
    }
    [self reopenMenu];
}

- (void)doHold:(id)sender {
    (void)sender;
    int pid; double t;
    if (hold_pid_read(&pid, &t)) {
        /* 运行中点击 = 关闭 (恒温的 SIGTERM 处理器会恢复自动并清 pidfile) */
        char *argv[] = {g_self, "__hold", "stop", NULL};
        int ok = run_root_argv(argv) == 0;
        [self tick];
        [self flash:ok ? U(L("恒温已关闭 ✓", "Thermostat off ✓")) : U(L("⚠ 执行失败", "⚠ failed"))];
    } else {
        /* 与智能互斥: 先收智能的控制权再启动恒温 */
        int spid; double slo, shi;
        if (smart_pid_read(&spid, &slo, &shi)) {
            char *stop[] = {g_self, "__smart", "stop", NULL};
            if (run_root_argv(stop) != 0) {
                [self flash:U(L("⚠ 停止智能失败", "⚠ failed to stop Smart"))];
                [self reopenMenu];
                return;
            }
        }
        char tb[8];
        snprintf(tb, sizeof tb, "%.0f", kHoldTarget[hold_idx()]);
        char *argv[] = {g_self, "__hold", tb, NULL};
        int ok = run_root_argv(argv) == 0;
        [self tick];
        [self flash:ok ? U(L("恒温启动中…", "Thermostat starting…")) : U(L("⚠ 执行失败", "⚠ failed"))];
    }
    [self reopenMenu];
}

- (void)doHoldTarget:(id)sender {
    int idx = (int)[sender tag];
    [[NSUserDefaults standardUserDefaults] setInteger:idx forKey:@"HoldTargetIdx"];
    int pid; double t;
    NSString *msg = nil;
    if (hold_pid_read(&pid, &t)) {
        /* 运行中: 热更新守护进程目标, 不打断风扇控制 */
        char tb[8];
        snprintf(tb, sizeof tb, "%.0f", kHoldTarget[idx]);
        char *argv[] = {g_self, "__hold", "target", tb, NULL};
        msg = run_root_argv(argv) == 0 ? U(L("目标已更新 ✓", "Target updated ✓"))
                                       : U(L("⚠ 更新失败", "⚠ update failed"));
    }
    [self tick];
    if (msg) [self flash:msg];
    [self reopenMenu];
}

/* 切语言: 存偏好 + 重建菜单, 下次启动仍生效 (CLI 与日志不受影响) */
- (void)doLang:(id)sender {
    int idx = (int)[sender tag]; /* 0=中文 1=English */
    [[NSUserDefaults standardUserDefaults] setInteger:idx + 1 forKey:@"MenuLang"];
    g_lang_en = idx;
    [self rebuildMenu];
    [self tick];
    [self reopenMenu];
}

- (void)doQuit:(id)sender {
    /* 退出 = 交还控制: 智能/恒温都未运行而风扇处于手动(全速/定速孤儿)时,
       经 setuid 助手静默恢复自动再退; 助手未装不弹密码直接退。
       守护进程(智能/恒温)独立于 UI, 退出菜单栏不影响它们 */
    int pid; double slo, shi, t;
    int orphan = 0;
    if (!smart_pid_read(&pid, &slo, &shi) && !hold_pid_read(&pid, &t))
        for (int f = 0; f < g_fan_n; f++)
            if (fan_mode(f) == 1) { orphan = 1; break; }
    if (orphan && access(FANSCTL_ROOT, X_OK) == 0) {
        char *argv[] = {g_self, "__apply", "auto", NULL};
        run_root_argv(argv);
    }
    [NSApp terminate:sender];
}

- (void)doThresh:(id)sender {
    int idx = (int)[sender tag];
    [[NSUserDefaults standardUserDefaults] setInteger:idx forKey:@"SmartThreshIdx"];
    int pid; double slo, shi;
    NSString *msg = nil;
    if (smart_pid_read(&pid, &slo, &shi)) {
        /* 运行中: 热更新守护进程阈值, 不打断风扇控制 */
        char lo[8], hi[8];
        snprintf(lo, sizeof lo, "%.0f", kThresh[idx][0]);
        snprintf(hi, sizeof hi, "%.0f", kThresh[idx][1]);
        char *argv[] = {g_self, "__smart", "thresh", lo, hi, NULL};
        msg = run_root_argv(argv) == 0 ? U(L("阈值已更新 ✓", "Thresholds updated ✓"))
                                       : U(L("⚠ 更新失败", "⚠ update failed"));
    }
    [self tick];
    if (msg) [self flash:msg];
    [self reopenMenu];
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
    lang_init();
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyAccessory]; /* 只在菜单栏, 不进 Dock */
    g_bar_delegate = [BarDelegate new];
    app.delegate = g_bar_delegate;
    [app run];
    smc_close();
    return 0;
}
