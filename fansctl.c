/*
 * fansctl - macOS SMC 风扇/温度工具 (Apple Silicon 兼容)
 * 一个程序两种形态: 无参数 = 菜单栏应用(fork 后台); 带参数 = CLI
 * SMC 读取核心在 fansctl.h, 菜单栏界面在 fansbar.m
 * 完整子命令列表见 main() 的用法输出 (fans/status/temps/dump/watch 只读;
 * set/max/auto/smart 需 root)
 * 许可证: AGPL-3.0-or-later (见 LICENSE), 商用需开源衍生代码
 */
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <sys/sysctl.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include "fansctl.h"

/* ==================== CLI 双语 (跟随系统语言) ====================
   FANSCTL_LANG > LC_ALL > LC_MESSAGES > LANG, zh* 前缀 = 中文, 其余 = 英文;
   全部未设置 = 中文 —— GUI 上下文 (LaunchAgent/菜单栏) 无 LANG, 由它拉起的
   守护进程日志 (/tmp/fansctl.smart.log 等) 由此恒为中文, 便于 grep。
   菜单栏界面另有独立可运行时切换的 L()/lang_init() (fansbar.m), 互不影响。 */
static int g_lang_en;
static const char *L(const char *zh, const char *en) { return g_lang_en ? en : zh; }

static void cli_lang_init(void) {
    const char *v = getenv("FANSCTL_LANG");
    if (!v || !*v) v = getenv("LC_ALL");
    if (!v || !*v) v = getenv("LC_MESSAGES");
    if (!v || !*v) v = getenv("LANG");
    g_lang_en = (v && *v && strncmp(v, "zh", 2) != 0);
}

/* --json: fans/status/temps/power/watch 的机器可读输出, 键名固定英文与界面语言无关 */
static int g_json;

/* ==================== 写入/控制 (需要 root) ==================== */

static void print_fans(void);
static int power_cmd(void);
static void json_power(void);

#define KR_NOT_PRIVILEGED 0xe00002c1

static kern_return_t smc_write_key(const char *keyname, const UInt8 *buf, size_t len) {
    SMCKeyData in = {0}, out = {0};
    in.key = str_to_key(keyname);
    in.data8 = SMC_CMD_READ_KEYINFO;
    kern_return_t r = smc_call(&in, &out);
    if (r != KERN_SUCCESS) return r;
    if (out.result != 0) return kIOReturnNotFound;
    if (out.keyInfo.dataSize != len) {
        if (g_debug) fprintf(stderr, "[dbg] write %s: 长度不匹配 键=%u 传入=%zu\n",
                             keyname, out.keyInfo.dataSize, len);
        return kIOReturnBadArgument;
    }
    memset(&in, 0, sizeof(in)); memset(&out, 0, sizeof(out));
    in.key = str_to_key(keyname);
    in.keyInfo.dataSize = (UInt32)len;
    in.data8 = SMC_CMD_WRITE_BYTES;
    memcpy(in.bytes, buf, len);
    r = smc_call(&in, &out);
    if (g_debug) fprintf(stderr, "[dbg] write %s: kr=0x%x result=%u\n", keyname, r, out.result);
    if (r != KERN_SUCCESS) return r;
    if (out.result != 0) return kIOReturnUnsupported; /* 0x82=温控器拒绝 0x86=键不可写 */
    return KERN_SUCCESS;
}

/* F?md / F?Md 探测与模式读取: fan_md_key / fan_mode / key_exists (fansctl.h) */

/* 返回 kern_return_t: KERN_SUCCESS=成功 */
static kern_return_t write_mode_bit(int idx, int manual) {
    char md[5];
    if (fan_md_key(idx, md)) {
        UInt8 v = manual ? 1 : 0;
        return smc_write_key(md, &v, 1);
    }
    UInt8 buf[32]; size_t len = sizeof(buf);
    kern_return_t r = smc_read_key("FS! ", buf, &len, NULL);
    if (r != KERN_SUCCESS) return r;
    if (len < 2) return kIOReturnBadArgument;
    UInt16 bits = (UInt16)((buf[0] << 8) | buf[1]);
    if (manual) bits |= (UInt16)(1 << idx);
    else        bits &= (UInt16)~(1 << idx);
    buf[0] = (UInt8)(bits >> 8); buf[1] = (UInt8)(bits & 0xff);
    return smc_write_key("FS! ", buf, 2);
}

static int ftst_held = 0;

static int engage_manual(int idx) {
    if (fan_mode(idx) == 1) return 0;
    kern_return_t r = write_mode_bit(idx, 1);
    if (r == KERN_SUCCESS) return 0;
    if (r == KR_NOT_PRIVILEGED) {
        fprintf(stderr, "%s", L("开启手动模式失败: 权限不足, 请用 sudo 运行\n",
                                "failed to enable manual mode: permission denied, run with sudo\n"));
        return -1;
    }
    if (!key_exists("Ftst")) {
        fprintf(stderr, L("开启手动模式失败: kr=0x%x (无 Ftst 解锁键可用)\n",
                          "failed to enable manual mode: kr=0x%x (no Ftst unlock key)\n"), r);
        return -1;
    }
    UInt8 one = 1;
    if (smc_write_key("Ftst", &one, 1) != KERN_SUCCESS) {
        fprintf(stderr, "%s", L("写入 Ftst 解锁失败\n", "failed to write Ftst unlock\n"));
        return -1;
    }
    ftst_held = 1;
    fprintf(stderr, "%s", L("已向温控管理器申请解锁(Ftst=1)，等待 3 秒...\n",
                            "unlock requested from thermal controller (Ftst=1), waiting 3 s...\n"));
    sleep(3);
    for (int attempt = 0; attempt < 300; attempt++) {
        if (write_mode_bit(idx, 1) == KERN_SUCCESS) return 0;
        usleep(100 * 1000);
    }
    fprintf(stderr, "%s", L("解锁后仍无法开启手动模式\n",
                            "manual mode still unavailable after unlock\n"));
    return -1;
}

static int write_tg(int idx, double rpm) {
    char k[5];
    snprintf(k, sizeof(k), "F%dTg", idx);
    /* 按键类型编码 */
    SMCKeyData in = {0}, out = {0};
    in.key = str_to_key(k);
    in.data8 = SMC_CMD_READ_KEYINFO;
    if (smc_call(&in, &out) != KERN_SUCCESS || out.result != 0) {
        fprintf(stderr, L("未找到 %s 键\n", "key %s not found\n"), k);
        return -1;
    }
    UInt8 data[8]; size_t dlen;
    char ts[5]; type_str(out.keyInfo.dataType, ts);
    if (strcmp(ts, "flt ") == 0) {
        float f = (float)rpm;
        memcpy(data, &f, 4);
        dlen = 4;
    } else if (strcmp(ts, "fpe2") == 0) {
        UInt16 v = (UInt16)(rpm * 4.0);
        data[0] = (UInt8)(v >> 8); data[1] = (UInt8)(v & 0xff);
        dlen = 2;
    } else {
        fprintf(stderr, L("不支持的目标键类型: %s\n", "unsupported target key type: %s\n"), ts);
        return -1;
    }
    /* 写入退避: SMC 忙(0x82 温控器占用)时指数退避重试, 总计约 1.5 秒, 不硬敲 */
    for (int attempt = 0, ms = 50; attempt < 6; attempt++, ms *= 2) {
        if (smc_write_key(k, data, dlen) == KERN_SUCCESS) return 0;
        if (attempt < 5) usleep(ms * 1000);
    }
    fprintf(stderr, L("写入 %s 失败 (SMC 可能拒绝: 0x82=温控器占用, 0x86=键只读)\n",
                      "failed to write %s (SMC may refuse: 0x82=controller busy, 0x86=readonly)\n"), k);
    return -1;
}

static int fan_apply(int idx, double rpm) {
    if (engage_manual(idx) != 0) return -1;
    return write_tg(idx, rpm);
}

static int fan_set(int idx, double rpm) {
    double mn, mx;
    if (read_rpm_key(idx, "Mn", &mn) != 0) mn = 0;
    if (read_rpm_key(idx, "Mx", &mx) != 0) mx = 6000;
    if (rpm < mn || rpm > mx) {
        double clamped = rpm < mn ? mn : mx;
        fprintf(stderr, L("转速限制到 %.0f rpm (安全范围 %.0f~%.0f)\n",
                          "RPM clamped to %.0f (safe range %.0f~%.0f)\n"), clamped, mn, mx);
        rpm = clamped;
    }
    return fan_apply(idx, rpm);
}

static int fan_max(int idx) {
    double mx;
    if (read_rpm_key(idx, "Mx", &mx) != 0) mx = 6000;
    return fan_apply(idx, mx);
}

static int fan_auto(int idx) {
    kern_return_t r = write_mode_bit(idx, 0);
    if (r != KERN_SUCCESS) {
        fprintf(stderr, L("恢复自动失败: kr=0x%x%s\n", "failed to restore auto: kr=0x%x%s\n"), r,
                r == KR_NOT_PRIVILEGED ? L(" (需要 sudo)", " (sudo required)") : "");
        return -1;
    }
    write_tg(idx, 0); /* 目标清零, 尽力而为 */
    /* 释放温控器解锁(可能是此前 set 留下的) */
    if (key_exists("Ftst")) {
        UInt8 buf[32]; size_t len = sizeof(buf);
        if (smc_read_key("Ftst", buf, &len, NULL) == KERN_SUCCESS && len >= 1 && buf[0] == 1) {
            UInt8 zero = 0;
            smc_write_key("Ftst", &zero, 1);
        }
    }
    ftst_held = 0;
    return 0;
}

static void print_status(void) {
    int n = fan_count();
    if (!n) { printf("%s", L("FNum 未找到, 尝试枚举...\n", "FNum not found, enumerating...\n")); print_fans(); return; }
    for (int i = 0; i < n; i++) {
        double ac = -1, tg = -1, mn = -1, mx = -1;
        read_rpm_key(i, "Ac", &ac);
        read_rpm_key(i, "Tg", &tg);
        read_rpm_key(i, "Mn", &mn);
        read_rpm_key(i, "Mx", &mx);
        int mode = fan_mode(i);
        printf(L("风扇%d  当前 %7.0f  目标 %7.0f  [%.0f~%.0f]  模式 %s\n",
                 "Fan %d  current %7.0f  target %7.0f  [%.0f~%.0f]  mode %s\n"),
               i, ac, tg, mn, mx,
               mode == 1 ? L("手动", "manual") : mode == 0 ? L("自动", "auto") : L("未知", "?"));
    }
}

static void print_fans(void) {
    int n = 0;
    for (char c = '0'; c <= '9'; c++) {
        char k[5];
        snprintf(k, sizeof(k), "F%cAc", c);
        double v;
        if (read_key_value(k, NULL, &v, NULL) != 0) continue;
        char km[5], kx[5], kid[5];
        snprintf(km, sizeof(km), "F%cMn", c);
        snprintf(kx, sizeof(kx), "F%cMx", c);
        snprintf(kid, sizeof(kid), "F%cID", c);
        double mn = -1, mx = -1;
        read_key_value(km, NULL, &mn, NULL);
        read_key_value(kx, NULL, &mx, NULL);
        char id[32] = ""; UInt8 idbuf[32]; size_t idlen = sizeof(idbuf);
        if (smc_read_key(kid, idbuf, &idlen, NULL) == KERN_SUCCESS && idlen > 0) {
            size_t keep = idlen < sizeof(id) - 1 ? idlen : sizeof(id) - 1;
            memcpy(id, idbuf, keep); id[keep] = 0;
        }
        if (id[0])
            printf(L("风扇%c  %-16s 当前 %7.0f rpm   [下限 %7.0f / 上限 %7.0f]\n",
                     "Fan %c  %-16s current %7.0f rpm   [min %7.0f / max %7.0f]\n"), c, id, v, mn, mx);
        else
            printf(L("风扇%c  当前 %7.0f rpm   [下限 %7.0f / 上限 %7.0f]\n",
                     "Fan %c  current %7.0f rpm   [min %7.0f / max %7.0f]\n"), c, v, mn, mx);
        n++;
    }
    if (!n) printf("%s", L("未发现风扇键(F*Ac)\n", "no fan keys found (F*Ac)\n"));
}

/* above = 温度下限过滤 (-100 表示不过滤); sort_desc = 按温度降序 */
static void print_temps(int sort_desc, double above) {
    UInt32 n = total_keys();
    if (!n) { fprintf(stderr, "%s", L("无法获取键总数\n", "cannot get key count\n")); return; }
    enum { CAP = 1024 };
    static char ks[CAP][5];
    static double vs[CAP];
    int found = 0;
    for (UInt32 i = 0; i < n && found < CAP; i++) {
        char k[5];
        if (get_key_at(i, k) != 0) continue;
        if (k[0] != 'T') continue;
        double v;
        if (read_key_value(k, NULL, &v, NULL) == 0 && plausible_temp(v) && v >= above) {
            memcpy(ks[found], k, 5);
            vs[found] = v;
            found++;
        }
    }
    if (sort_desc) {
        /* 简单选择排序配对挪动 (found<=1024, 无需 qsort 的间接层) */
        for (int i = 0; i < found; i++)
            for (int j = i + 1; j < found; j++)
                if (vs[j] > vs[i]) {
                    double tv = vs[i]; vs[i] = vs[j]; vs[j] = tv;
                    char tk[5]; memcpy(tk, ks[i], 5); memcpy(ks[i], ks[j], 5); memcpy(ks[j], tk, 5);
                }
    }
    for (int i = 0; i < found; i++) {
        const char *nm = temp_key_name(ks[i], g_lang_en);
        if (nm) printf("%-6s %-16s %9.2f °C\n", ks[i], nm, vs[i]);
        else    printf("%-6s %18.2f °C\n", ks[i], vs[i]);
    }
    if (!found) printf("%s", L("未发现温度传感器\n", "no temperature sensors found\n"));
}

static void print_dump(void) {
    UInt32 n = total_keys();
    if (!n) { fprintf(stderr, "%s", L("无法获取键总数\n", "cannot get key count\n")); return; }
    printf(L("SMC 键总数: %u\n\n", "SMC key count: %u\n\n"), n);
    for (UInt32 i = 0; i < n; i++) {
        char k[5];
        if (get_key_at(i, k) != 0) continue;
        double v; UInt32 t; char hex[80];
        int rc = read_key_value(k, &t, &v, hex);
        char ts[5]; type_str(t, ts);
        if (rc == 0)      printf("%-6s %-4s %12.3f\n", k, ts, v);
        else if (rc == 1) printf("%-6s %-4s   raw: %s\n", k, ts, hex);
    }
}

static void print_status_line(void) {
    for (char c = '0'; c <= '9'; c++) {
        char k[5];
        snprintf(k, sizeof(k), "F%cAc", c);
        double v;
        if (read_key_value(k, NULL, &v, NULL) != 0) continue;
        printf("风扇%c %6.0f rpm  ", c, v);
    }
    const char *mxk = NULL;
    double mx = hottest_temp(&mxk);
    if (mx > -999) printf(L(" 最热 %s %.1f°C", " hottest %s %.1f°C"), mxk ? mxk : "?", mx);
    printf("\n");
}

/* ==================== JSON 输出 (--json) ==================== */

/* 字符串转义进缓冲区 (UTF-8 原样透传, 只转义 JSON 语法字符与控制字符)。
   独立成 buffer 版以便 __selftest 直接断言 */
static void json_escape(char *out, size_t n, const char *s) {
    size_t w = 0;
    if (!n) return;
    for (const unsigned char *p = (const unsigned char *)s; *p && w < n - 1; p++) {
        const char *esc = NULL; char tmp[8];
        switch (*p) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (*p < 0x20) { snprintf(tmp, sizeof tmp, "\\u%04x", *p); esc = tmp; }
        }
        if (esc) {
            size_t el = strlen(esc);
            if (w + el >= n) break;
            memcpy(out + w, esc, el); w += el;
        } else out[w++] = (char)*p;
    }
    out[w] = 0;
}

static void json_str(const char *s) {
    char buf[1024];
    json_escape(buf, sizeof buf, s);
    printf("\"%s\"", buf);
}

/* 数值格式化进缓冲区: 未知约定为 <=-0.5, 输出 null, 其余按精度 */
static void jnum_buf(char *out, size_t n, double v, int prec) {
    if (v <= -0.5) snprintf(out, n, "null");
    else {
        if (v < 0) v = 0; /* -0.x 归一, 防 JSON 里出现 "-0" */
        snprintf(out, n, "%.*f", prec, v);
    }
}

/* 数值打 null 或数字: 未知约定为 -1, 其余按精度输出 (电流这类合法负数字段不用此函数) */
static void jnum(double v, int prec) {
    char buf[32];
    jnum_buf(buf, sizeof buf, v, prec);
    fputs(buf, stdout);
}

static void json_fans(void) {
    printf("{\"fans\":[");
    int first = 1;
    for (char c = '0'; c <= '9'; c++) {
        char k[5];
        snprintf(k, sizeof(k), "F%cAc", c);
        double v;
        if (read_key_value(k, NULL, &v, NULL) != 0) continue;
        char km[5], kx[5], kid[5];
        snprintf(km, sizeof(km), "F%cMn", c);
        snprintf(kx, sizeof(kx), "F%cMx", c);
        snprintf(kid, sizeof(kid), "F%cID", c);
        double mn = -1, mx = -1;
        read_key_value(km, NULL, &mn, NULL);
        read_key_value(kx, NULL, &mx, NULL);
        char id[32] = ""; UInt8 idbuf[32]; size_t idlen = sizeof(idbuf);
        if (smc_read_key(kid, idbuf, &idlen, NULL) == KERN_SUCCESS && idlen > 0) {
            size_t keep = idlen < sizeof(id) - 1 ? idlen : sizeof(id) - 1;
            memcpy(id, idbuf, keep); id[keep] = 0;
        }
        printf("%s{\"id\":%c", first ? "" : ",", c);
        if (id[0]) { printf(",\"name\":"); json_str(id); }
        printf(",\"rpm\":");       jnum(v, 0);
        printf(",\"min_rpm\":");   jnum(mn, 0);
        printf(",\"max_rpm\":");   jnum(mx, 0);
        printf("}");
        first = 0;
    }
    printf("]}\n");
}

static void json_status(void) {
    int n = fan_count();
    printf("{\"fans\":[");
    int first = 1;
    for (int i = 0; i < (n > 0 ? n : 10); i++) {
        if (n <= 0) { /* FNum 不可读: 退回逐个探测 F?Ac, 与人类输出一致 */
            char k[5];
            snprintf(k, sizeof(k), "F%dAc", i);
            if (!key_exists(k)) continue;
        }
        double ac = -1, tg = -1, mn = -1, mx = -1;
        read_rpm_key(i, "Ac", &ac);
        read_rpm_key(i, "Tg", &tg);
        read_rpm_key(i, "Mn", &mn);
        read_rpm_key(i, "Mx", &mx);
        int mode = fan_mode(i);
        printf("%s{\"id\":%d,\"actual_rpm\":", first ? "" : ",", i);
        jnum(ac, 0);
        printf(",\"target_rpm\":");
        jnum(tg, 0);
        printf(",\"min_rpm\":");
        jnum(mn, 0);
        printf(",\"max_rpm\":");
        jnum(mx, 0);
        printf(",\"mode\":\"%s\"}",
               mode == 1 ? "manual" : mode == 0 ? "auto" : "unknown");
        first = 0;
    }
    printf("]}\n");
}

static void json_temps(void) {
    UInt32 n = total_keys();
    printf("{\"temps\":[");
    int first = 1;
    if (n)
        for (UInt32 i = 0; i < n; i++) {
            char k[5];
            if (get_key_at(i, k) != 0) continue;
            if (k[0] != 'T') continue;
            double v;
            if (read_key_value(k, NULL, &v, NULL) == 0 && plausible_temp(v)) {
                printf("%s{\"key\":\"%s\",\"name\":", first ? "" : ",", k);
                const char *nm = temp_key_name(k, 1);
                if (nm) json_str(nm); else fputs("null", stdout);
                printf(",\"celsius\":%.2f}", v);
                first = 0;
            }
        }
    printf("]}\n");
}

/* ==================== 智能模式 (需 root, Ctrl+C 退出并恢复自动) ==================== */

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;
static volatile sig_atomic_t g_stop_pid = 0; /* 发停止信号的进程, 排查莫名退出 */
static void on_stop_signal(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)ctx;
    if (si && si->si_pid > 0) g_stop_pid = si->si_pid;
    g_stop = 1;
}
static void on_reload_signal(int sig) { (void)sig; g_reload = 1; }

/* 连续时钟: 系统睡眠期间也走表 (mach_absolute_time 会停), 用于探测唤醒 */
static mach_timebase_info_data_t g_timebase;
static uint64_t cont_ns(void) {
    return mach_continuous_time() * g_timebase.numer / g_timebase.denom;
}

/* 通知中心横幅: 中间子进程再 fork 出执行者后立即退出, 控制循环只等几毫秒的
   waitpid, 不会被 osascript 启动耗时拖住。守护进程上下文无 LANG, 文案由调用方
   用 L() 备好 (默认中文); msg 内不得含双引号 (我们的消息只有数字与°C) */
static void notify_send(const char *msg) {
    pid_t pid = fork();
    if (pid > 0) { int st; waitpid(pid, &st, 0); return; }
    if (pid < 0) return;
    if (fork() == 0) {
        char script[512];
        snprintf(script, sizeof script,
                 "display notification \"%s\" with title \"fansctl\"", msg);
        execl("/usr/bin/osascript", "osascript", "-e", script, (char *)NULL);
        _exit(0);
    }
    _exit(0);
}

/* ==================== 智能模式功耗前馈 ====================
   热量还没到, 风扇先动: 整机功耗 ≈ 发热量, 功耗一跳 (编译/加载大模型) 转速
   立刻跟上, 而不是等芯片热了才反应, 实测可提前约 6 秒 (参考 TomEageer/fanctl)。
   数据源用 SMC 键 PSTR (秒级实时; ioreg 的 SystemPowerIn 分钟级刷新, 做前馈太钝)。
   检测用"快慢 EMA 差"(超前-滞后滤波): 负载上坡时快线先抬, 稳态后两线追平,
   前馈自然归零 —— 温度曲线接管稳态, 前馈只管瞬态, 不会长期多转。
   只升不降 (升了温度反馈会立刻修正, 最坏是白先转几秒; 降向交给温度曲线)。
   充电时扣除充电分量 (那部分能量入电池不发热); 电池供电/无 PSTR 键时前馈
   自动退场。纯函数化, __selftest 里有闭环仿真断言 */
#define FF_A_FAST  0.30    /* 快 EMA 系数 (1 秒拍) */
#define FF_A_SLOW  0.10    /* 慢 EMA 系数 */
#define FF_W_THRESH 3.0    /* 快慢差超过此值才算负载上坡 (W), 滤遥测抖动 */
#define FF_GAIN    60.0    /* 前馈增益 rpm/W */
#define FF_MAX     1500.0  /* 前馈增量上限 rpm */
#define FF_STEP    250.0   /* 前馈增量每拍限速 rpm (升/降同速, 平滑) */

struct ff_state { double w_fast, w_slow, boost; int have; };
static void ff_init(struct ff_state *st) {
    st->w_fast = st->w_slow = st->boost = 0;
    st->have = 0;
}

/* 一拍: w = 本拍整机功耗 W (已扣充电; <=0 表示无数据)。返回当前前馈增量 rpm */
static double ff_tick(struct ff_state *st, double w) {
    if (w <= 0) { /* 电池/无 PSTR: 增量平滑退场; 数据回来时重建 EMA 基线 */
        if (st->boost > 0) {
            st->boost -= FF_STEP;
            if (st->boost < 0) st->boost = 0;
        }
        st->have = 0;
        return st->boost;
    }
    if (!st->have) { st->have = 1; st->w_fast = st->w_slow = w; return st->boost; }
    st->w_fast += FF_A_FAST * (w - st->w_fast);
    st->w_slow += FF_A_SLOW * (w - st->w_slow);
    double lead = st->w_fast - st->w_slow; /* 上坡强度: 稳态时为 0 */
    double target = 0;
    if (lead > FF_W_THRESH) target = (lead - FF_W_THRESH) * FF_GAIN;
    if (target > FF_MAX) target = FF_MAX;
    if (st->boost < target) {
        st->boost += FF_STEP;
        if (st->boost > target) st->boost = target;
    } else {
        st->boost -= FF_STEP;
        if (st->boost < target) st->boost = target;
    }
    return st->boost;
}

/* SMC 写楔住时的退避时长: 5 秒起步每失败一次翻倍, 60 秒封顶 */
static uint64_t wedge_delay_ns(int wfail) {
    int s = 5 << (wfail - 1);
    if (s > 60) s = 60;
    return (uint64_t)s * 1000000000ULL;
}

static int smart_loop(double t_lo, double t_hi) {
    int n = fan_count();
    if (n < 1) n = 1;
    double base[10] = {0};   /* 离开自动那一刻系统想要的目标转速 */
    double last[10], want[10] = {0};
    int manual[10] = {0};
    for (int i = 0; i < 10; i++) last[i] = -1;
    /* SA_SIGINFO 记录停止信号来源 pid (kill 发送者; 终端/内核产生的为 0) */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGUSR1, on_reload_signal); /* __smart thresh <lo> <hi> 就地更新阈值 */
    struct ff_state ff;
    ff_init(&ff);
    int wfail = 0; /* 连续写入失败数: SMC 楔住时指数退避 (5s→60s) 的级数 */
    printf(L("智能模式: <=%.0f°C 自动 (基线跟随系统), >=%.0f°C 全速, 中间线性插值, 功耗前馈; Ctrl+C 退出并恢复自动\n",
             "Smart mode: <=%.0f°C auto (baseline follows system), >=%.0f°C full speed, "
             "linear in between, power feedforward; Ctrl+C exits and restores auto\n"),
           t_lo, t_hi);
    mach_timebase_info(&g_timebase);
    uint64_t tick_ns = cont_ns();
    int rc = 0, notified = 0;
    while (!g_stop) {
        /* SMC 楔住退避: 上拍写入连续失败 (温控器被外部楔住/占用), 不硬敲不退出,
           隔指数时长再试, 期间温度照读 —— 恢复写入的那拍自然回到控制 */
        if (wfail > 0) {
            uint64_t until = tick_ns + wedge_delay_ns(wfail);
            while (cont_ns() < until && !g_stop) usleep(100 * 1000);
            tick_ns = cont_ns();
            if (g_stop) break;
        }
        if (g_reload) { /* pidfile 里的新阈值就地生效, 不打断风扇控制 */
            g_reload = 0;
            int p; double nl = 0, nh = 0;
            if (smart_pid_read(&p, &nl, &nh) && p == (int)getpid() &&
                nl >= 20 && nh >= nl + 5 && nh <= 120) {
                t_lo = nl;
                t_hi = nh;
                printf(L("[阈值更新为 %.0f~%.0f°C]\n", "[thresholds updated to %.0f~%.0f°C]\n"), t_lo, t_hi);
                fflush(stdout);
            }
        }
        const char *tkey = NULL;
        double T = hottest_temp(&tkey);
        if (T < -999) {
            /* 睡眠唤醒等场景 SMC 连接可能短暂失效: 重开连接重试, 连续失败才放弃 */
            for (int i = 0; i < 3 && T < -999 && !g_stop; i++) {
                sleep(1);
                smc_close();
                if (smc_open() != 0) continue;
                T = hottest_temp(&tkey);
            }
            if (T < -999) { fprintf(stderr, "%s", L("温度读取失败(已重试)\n", "temperature read failed (after retries)\n")); rc = 1; break; }
        }
        double frac = (T - t_lo) / (t_hi - t_lo);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        /* 高温告警: 达到高阈值(=满速)通知一次, 回落 2°C 后解除, 下次越限再报 */
        if (frac >= 1 && !notified) {
            notified = 1;
            char m[160];
            snprintf(m, sizeof m,
                     L("温度 %.1f°C 达到高阈值 %.0f°C, 风扇已全速 —— 请检查散热",
                       "temperature %.1f°C reached the %.0f°C high threshold, fans at full speed — check cooling"),
                     T, t_hi);
            notify_send(m);
            printf("%s", L("[已发送高温通知]\n", "[high-temperature notification sent]\n"));
            fflush(stdout);
        } else if (notified && T < t_hi - 2.0) {
            notified = 0;
        }
        /* 功耗前馈: PSTR 整机功耗 (秒级), 充电时扣掉充电分量 (那部分入电池不发热) */
        double pw = -1;
        read_key_value("PSTR", NULL, &pw, NULL);
        double ff_w = 0; /* 前馈输入: 净功耗 (0=无数据) */
        if (pw > 0 && pw < 1000) {
            struct power_info p;
            if (power_read(&p) == 0 && p.charging && p.charge_w > 0) {
                pw -= p.charge_w;
                if (pw <= 0) pw = 0;
            }
            ff_w = pw;
        }
        double boost = ff_tick(&ff, ff_w);
        for (int f = 0; f < n; f++) {
            double mn = 0, mx = 6000;
            read_rpm_key(f, "Mn", &mn);
            read_rpm_key(f, "Mx", &mx);
            if (manual[f] && T < t_lo - 1.0) { /* 迟滞 1°C 防抖 */
                if (fan_auto(f) != 0) { rc = 1; break; }
                manual[f] = 0;
                last[f] = -1;
            }
            if (manual[f]) {
                /* MODE_REASSERT: 核验手动权与目标转速, 被外部工具/系统改写
                   (改回自动、覆盖目标)则立即重新接管或重写目标 */
                int m = fan_mode(f);
                double tg = -1;
                read_rpm_key(f, "Tg", &tg);
                if ((m >= 0 && m != 1) ||
                    (m == 1 && tg > 0 && last[f] > 0 && fabs(tg - last[f]) > 150)) {
                    printf(L("[风扇%d 被外部改写(模式=%d 目标=%.0f 期望=%.0f), 重新接管]\n",
                             "[fan %d overridden externally (mode=%d target=%.0f expected=%.0f), retaking]\n"),
                           f, m, tg, last[f]);
                    fflush(stdout);
                    if (m == 1) last[f] = -1; /* 目标被改, 本拍重写 */
                    else { manual[f] = 0; last[f] = -1; } /* 手动权丢失, 重新接管 */
                }
            }
            if (frac <= 0 && !manual[f]) {
                double tg;
                if (read_rpm_key(f, "Tg", &tg) == 0) base[f] = tg < mn ? mn : tg;
                want[f] = 0;
                continue;
            }
            if (!manual[f]) {
                /* 基线 = 接管瞬间系统想要的目标。但若风扇已处于手动(上个会话/
                   全速残留), Tg 是残留的自设值而非系统意图, 不可采信, 退回 Mn */
                double tg;
                if (fan_mode(f) == 1) base[f] = mn;
                else if (read_rpm_key(f, "Tg", &tg) == 0 && tg > mn) base[f] = tg;
                if (base[f] < mn) base[f] = mn;
                if (engage_manual(f) != 0) { rc = 1; break; }
                manual[f] = 1;
            }
            double w = base[f] + frac * (mx - base[f]);
            if (boost > 0 && frac > 0) w += boost; /* 前馈只加在有控制权的拍 */
            if (w < mn) w = mn;
            if (w > mx) w = mx;
            want[f] = w;
            if (last[f] < 0 || fabs(w - last[f]) > 10) {
                if (write_tg(f, w) != 0) {
                    /* 写楔住: 进退避, 不退出 (守护进程还在看温度, 恢复即回控制) */
                    wfail++;
                    if (wfail == 1)
                        printf("%s", L("[写入被 SMC 拒绝, 进入退避监测 (最长 60 秒/次)]\n",
                                       "[write refused by SMC, backing off (up to 60 s between retries)]\n"));
                    fflush(stdout);
                    continue;
                }
                wfail = 0;
                last[f] = w;
            }
        }
        if (rc != 0) break;
        printf(L("[%.0f~%.0f°C] %s %.1f°C 曲线%.0f%% |", "[%.0f~%.0f°C] %s %.1f°C curve %.0f%% |"),
               t_lo, t_hi, tkey ? tkey : "?", T, frac * 100);
        if (pw > 0 && boost > 0)
            printf(L(" %.0fW 前馈+%.0f |", " %.0fW ff+%.0f |"), pw, boost);        for (int f = 0; f < n; f++) {
            double ac = -1;
            read_rpm_key(f, "Ac", &ac);
            if (manual[f] && want[f] > 0 && base[f] > 0)
                printf(L(" 风扇%d %6.0f rpm (+%.0f%%) 手动", " fan %d %6.0f rpm (+%.0f%%) manual"),
                       f, ac, (want[f] - base[f]) / base[f] * 100);
            else
                printf(L(" 风扇%d %6.0f rpm 自动", " fan %d %6.0f rpm auto"), f, ac);
        }
        printf("\n");
        fflush(stdout);
        sleep(1);
        /* 唤醒让权: 1 秒拍长被拉长到 5 秒以上 = 系统刚从睡眠唤醒。
           先交还系统控制并静置 5 秒(温控器/传感器稳定), 之后按新基线重新接管 */
        uint64_t dt = cont_ns() - tick_ns;
        tick_ns = cont_ns();
        if (dt > 5ULL * 1000000000ULL) {
            printf(L("[间隔 %.0f 秒, 判定为系统唤醒, 暂交系统控制 5 秒]\n",
                     "[gap %.0f s, system wake detected, handing control back for 5 s]\n"), dt / 1e9);
            for (int f = 0; f < n; f++)
                if (manual[f]) {
                    if (fan_auto(f) != 0) { rc = 1; break; }
                    manual[f] = 0;
                    last[f] = -1;
                }
            fflush(stdout);
            if (rc != 0) break;
            for (int i = 0; i < 5 && !g_stop; i++) sleep(1);
            tick_ns = cont_ns();
        }
    }
    for (int f = 0; f < n; f++)
        if (manual[f]) fan_auto(f);
    if (g_stop) {
        printf("%s", L("\n已退出智能模式, 全部恢复自动", "\nSmart mode exited, all fans restored to auto"));
        if (g_stop_pid > 0) printf(L(" (停止信号来自 pid %d)", " (stop signal from pid %d)"), (int)g_stop_pid);
        printf("\n");
    }
    return rc;
}

static void watch_loop(int interval) {
    for (;;) {
        if (g_json) json_status(); /* 每拍一个完整 JSON 对象 (JSONL) */
        else print_status_line();
        fflush(stdout);
        sleep(interval);
    }
}

/* power --watch: 持续功率监测, 间隔默认 5 秒 (功率采集比 status 重: 含 USB 枚举与
   进程采样)。--json 时每行一个完整对象 (JSONL), 可直接接 jq/记录管道 */
static void power_watch_loop(int interval) {
    for (;;) {
        if (g_json) json_power();
        else power_cmd();
        fflush(stdout);
        sleep(interval);
    }
}

/* ==================== 功率/供电一览 (只读, 无需 root) ==================== */

/* USB 设备声明的电流需求: 读配置描述符 bMaxPower (内核缓存, 不产生总线流量)。
   USB2 单位 2mA, USB3+ 单位 8mA; bmAttributes 位6 = 自供电。
   这是设备自报的 5V 需求——macOS 无公开接口查询外设实际拉取功率/PD 协商结果 */
static int usb_declared_ma(io_object_t service, int bcd_usb, int *self_powered) {
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    if (self_powered) *self_powered = 0;
    if (IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                          kIOCFPlugInInterfaceID, &plug, &score) != KERN_SUCCESS)
        return -1;
    IOUSBDeviceInterface **usb = NULL;
    int ma = -1;
    if ((*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID197),
                                (LPVOID *)&usb) == S_OK && usb) {
        IOUSBConfigurationDescriptorPtr cfg = NULL;
        (*usb)->USBDeviceOpen(usb); /* 有的固件要求 open 才给描述符; 失败也继续读缓存 */
        if ((*usb)->GetConfigurationDescriptorPtr(usb, 0, &cfg) == kIOReturnSuccess &&
            cfg && cfg->bLength >= 9) {
            ma = (int)cfg->MaxPower * (bcd_usb >= 0x0300 ? 8 : 2);
            if (self_powered) *self_powered = (cfg->bmAttributes & 0x40) ? 1 : 0;
        }
        (*usb)->USBDeviceClose(usb);
        (*usb)->Release(usb);
    }
    (*plug)->Release(plug);
    return ma;
}

/* USB 枚举采集: 供人类输出与 --json 共用, 返回条数 (数组调用方 free), 失败 -1 */
struct usb_dev { char name[128], vendor[128]; const char *speed; int ma; int self_powered; };

static int collect_usb(struct usb_dev **out) {
    io_iterator_t it = MACH_PORT_NULL;
    *out = NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching("IOUSBHostDevice"), &it) != KERN_SUCCESS)
        return -1;
    int cap = 8, n = 0;
    struct usb_dev *list = calloc((size_t)cap, sizeof *list);
    if (!list) { IOObjectRelease(it); return -1; }
    io_object_t dev;
    while ((dev = IOIteratorNext(it)) != 0) {
        CFMutableDictionaryRef props = NULL;
        if (IORegistryEntryCreateCFProperties(dev, &props,
                                              kCFAllocatorDefault, kNilOptions) != KERN_SUCCESS || !props) {
            IOObjectRelease(dev);
            continue;
        }
        char name[128] = "", vendor[128] = "";
        CFStringRef s = CFDictionaryGetValue(props, CFSTR("USB Product Name"));
        if (s && CFGetTypeID(s) == CFStringGetTypeID())
            CFStringGetCString(s, name, sizeof name, kCFStringEncodingUTF8);
        s = CFDictionaryGetValue(props, CFSTR("USB Vendor Name"));
        if (s && CFGetTypeID(s) == CFStringGetTypeID())
            CFStringGetCString(s, vendor, sizeof vendor, kCFStringEncodingUTF8);
        if (name[0]) { /* 无产品名的节点(罕见)跳过 */
            double spd = cfnum_to_double(CFDictionaryGetValue(props, CFSTR("Device Speed")));
            double bcd = cfnum_to_double(CFDictionaryGetValue(props, CFSTR("bcdUSB")));
            const char *sp = "";
            if (spd >= 0 && spd <= 6) {
                static const char *spds[] =
                    {"1.5 Mb/s", "12 Mb/s", "480 Mb/s", "5 Gb/s", "10 Gb/s", "10 Gb/s", "20 Gb/s"};
                sp = spds[(int)spd];
            }
            int selfp = 0;
            int ma = usb_declared_ma(dev, bcd > 0 ? (int)bcd : 0, &selfp);
            if (n == cap) {
                cap *= 2;
                struct usb_dev *grown = realloc(list, (size_t)cap * sizeof *list);
                if (!grown) { free(list); CFRelease(props); IOObjectRelease(dev); IOObjectRelease(it); return -1; }
                list = grown;
            }
            snprintf(list[n].name, sizeof list[n].name, "%s", name);
            snprintf(list[n].vendor, sizeof list[n].vendor, "%s", vendor);
            list[n].speed = sp;
            list[n].ma = ma;
            list[n].self_powered = selfp;
            n++;
        }
        CFRelease(props);
        IOObjectRelease(dev);
    }
    IOObjectRelease(it);
    *out = list;
    return n;
}

static void print_usb_power(void) {
    printf("%s", L("USB 设备 (声明的 5V 电流需求, 非实测):\n",
                   "USB devices (declared 5V current draw, not measured):\n"));
    struct usb_dev *d = NULL;
    int n = collect_usb(&d);
    if (n < 0) { printf("%s", L("  枚举失败\n", "  enumeration failed\n")); return; }
    for (int i = 0; i < n; i++) {
        if (d[i].ma > 0)
            printf(L("  - %s (%s)  %s  %d mA ≈ %.1f W%s\n", "  - %s (%s)  %s  %d mA ≈ %.1f W%s\n"),
                   d[i].name, d[i].vendor, d[i].speed, d[i].ma, d[i].ma * 5.0 / 1000.0,
                   d[i].self_powered ? L(" [自供电]", " [self-powered]") : "");
        else
            printf(L("  - %s (%s)  %s  电流未知\n", "  - %s (%s)  %s  current unknown\n"),
                   d[i].name, d[i].vendor, d[i].speed);
    }
    if (!n) printf("%s", L("  (无 USB 设备)\n", "  (no USB devices)\n"));
    free(d);
}

/* ---- 进程功耗排行: 瞬时 CPU% (0.4s 两次采样) + 平均% (累计÷存活时长) + 内存, 降序 ----
   每进程 GPU 占用无公开接口 (活动监视器也不分), 笔记本上 CPU 即功耗主导,
   故按 CPU 排序。瞬时% 会放大几秒的短任务 (缩略图/窗口渲染), 平均% 是
   启动以来的统计值, 不会尖峰; 两列对照即可区分瞬时尖峰与长期大户 */
struct proc_sample { int pid; uint64_t t_ns, rss; double start_s; char name[64]; };

static int sample_procs(struct proc_sample **out, struct proc_sample **denied_out, int *ndenied) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len < sizeof(struct kinfo_proc))
        return -1;
    struct kinfo_proc *kp = malloc(len);
    if (!kp) return -1;
    if (sysctl(mib, 4, kp, &len, NULL, 0) != 0) { free(kp); return -1; }
    int cnt = (int)(len / sizeof(struct kinfo_proc));
    struct proc_sample *list = calloc((size_t)(cnt > 0 ? cnt : 1), sizeof *list);
    struct proc_sample *denied = calloc((size_t)(cnt > 0 ? cnt : 1), sizeof *denied);
    if (!list || !denied) { free(kp); free(list); free(denied); return -1; }
    int n = 0, nd = 0;
    uint64_t tids[4096];
    for (int i = 0; i < cnt; i++) {
        int pid = kp[i].kp_proc.p_pid;
        if (pid <= 0 || kp[i].kp_proc.p_stat == SZOMB) continue;
        struct proc_taskinfo pti;
        if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &pti, sizeof pti) <= 0) {
            /* 在 sysctl 列表里但属性读失败 = 其他用户/受保护进程
               (task_read_for_pid 只对苹果平台二进制开放, 第三方拿不到) */
            denied[nd].pid = pid;
            denied[nd].rss = 0;
            snprintf(denied[nd].name, sizeof denied[nd].name, "%s", kp[i].kp_proc.p_comm);
            nd++;
            continue;
        }
        /* CPU 时间必须逐线程累加: proc_taskinfo 的 total 与 threads 两组字段
           都非单调累计 (threads 组随线程退出回落), 直接做差会回绕 */
        int tb = proc_pidinfo(pid, PROC_PIDLISTTHREADS, 0, tids, (int)sizeof tids);
        if (tb <= 0) {
            denied[nd].pid = pid;
            denied[nd].rss = pti.pti_resident_size;
            snprintf(denied[nd].name, sizeof denied[nd].name, "%s", kp[i].kp_proc.p_comm);
            nd++;
            continue;
        }
        uint64_t t = 0;
        for (int k = 0; k < tb / (int)sizeof(uint64_t); k++) {
            struct proc_threadinfo pth;
            if (proc_pidinfo(pid, PROC_PIDTHREADINFO, tids[k], &pth, sizeof pth) > 0)
                t += pth.pth_user_time + pth.pth_system_time;
        }
        char path[PROC_PIDPATHINFO_MAXSIZE] = "";
        const char *nm = kp[i].kp_proc.p_comm; /* 截断名兜底 */
        if (proc_pidpath(pid, path, sizeof path) > 0 && path[0]) {
            const char *b = strrchr(path, '/');
            if (b && b[1]) nm = b + 1;
        }
        list[n].pid = pid;
        list[n].t_ns = t;
        list[n].rss = pti.pti_resident_size;
        list[n].start_s = kp[i].kp_proc.p_starttime.tv_sec +
                          kp[i].kp_proc.p_starttime.tv_usec / 1e6; /* 进程启动时刻 */
        snprintf(list[n].name, sizeof list[n].name, "%s", nm);
        n++;
    }
    free(kp);
    *out = list;
    *denied_out = denied;
    *ndenied = nd;
    return n;
}

struct top_row { double cpu, avg, mempct; uint64_t rss, cpu_ns; int pid; char name[64]; };

/* 累计 CPU 时间紧凑格式: 45s / 15:49 / 3h05m / 2d05h */
static void fmt_cpu_ns(uint64_t ns, char out[16]) {
    double s = ns / 1e9;
    if (s < 60)         snprintf(out, 16, "%.0fs", s);
    else if (s < 3600)  snprintf(out, 16, "%d:%02d", (int)(s / 60), (int)fmod(s, 60));
    else if (s < 86400) snprintf(out, 16, "%dh%02dm", (int)(s / 3600), (int)fmod(s / 60, 60));
    else                snprintf(out, 16, "%dd%02dh", (int)(s / 86400), (int)fmod(s / 3600, 24));
}

/* 表头单元格: printf 的 %Ns 按字符数计宽, 中文表头每字显示占 2 列会错位,
   这里按显示宽度右对齐 (UTF-8 3 字节起的 CJK 记 2 列), 尾随一个空格分隔 */
static void hdr_cell(const char *s, int width) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        if (*p < 0x80)      { w += 1; p += 1; }
        else if (*p < 0xE0) { w += 1; p += 2; }
        else if (*p < 0xF0) { w += 2; p += 3; }
        else                { w += 2; p += 4; }
    }
    while (width > w) { putchar(' '); width--; }
    fputs(s, stdout);
    putchar(' ');
}

static int row_cmp(const void *x, const void *y) {
    double d = ((const struct top_row *)y)->cpu - ((const struct top_row *)x)->cpu;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static int denied_cmp(const void *x, const void *y) {
    uint64_t a = ((const struct proc_sample *)x)->rss;
    uint64_t b = ((const struct proc_sample *)y)->rss;
    return a < b ? 1 : a > b ? -1 : 0;
}

/* 两次采样 + 匹配 + 排序, 供人类输出与 --json 共用。
   返回行数 (rows 按 CPU% 降序; denied 为无权限进程, 调用方 free 两个数组), 失败 -1 */
static int gather_top(struct top_row **rows_out, struct proc_sample **denied_out, int *ndenied_out) {
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    struct proc_sample *a = NULL, *b = NULL, *da = NULL, *db = NULL;
    int nda = 0, ndb = 0;
    int na = sample_procs(&a, &da, &nda);
    usleep(400 * 1000); /* 采样窗口 */
    int nb = sample_procs(&b, &db, &ndb);
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    double elapsed_ns =
        (double)(ts1.tv_sec - ts0.tv_sec) * 1e9 + (double)(ts1.tv_nsec - ts0.tv_nsec);
    if (na <= 0 || nb <= 0 || elapsed_ns <= 0) {
        free(a); free(b); free(da); free(db);
        return -1;
    }
    uint64_t memsize = 0;
    size_t ml = sizeof memsize;
    sysctl((int[2]){CTL_HW, HW_MEMSIZE}, 2, &memsize, &ml, NULL, 0);
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    double now_s = rt.tv_sec + rt.tv_nsec / 1e9;
    struct top_row *rows = calloc((size_t)nb, sizeof *rows);
    if (!rows) { free(a); free(b); free(da); free(db); return -1; }
    int n = 0;
    for (int j = 0; j < nb; j++)
        for (int i = 0; i < na; i++)
            if (a[i].pid == b[j].pid) { /* 两次采样间新出现的进程无基准, 跳过 */
                /* 线程中途退出会使总时间回落 (无符号差会回绕成天文数字), 钳为 0 */
                uint64_t d = b[j].t_ns > a[i].t_ns ? b[j].t_ns - a[i].t_ns : 0;
                rows[n].cpu = (double)d * 100.0 / elapsed_ns;
                /* 统计均值: 启动以来累计 CPU ÷ 存活时长 (下限 0.5s 防除零) */
                double alive = now_s - b[j].start_s;
                if (alive < 0.5) alive = 0.5;
                rows[n].avg = (double)b[j].t_ns / 1e9 / alive * 100.0;
                rows[n].rss = b[j].rss;
                rows[n].cpu_ns = b[j].t_ns; /* 进程启动以来的累计 CPU 时间 */
                rows[n].pid = b[j].pid;
                rows[n].mempct = memsize ? (double)b[j].rss * 100.0 / (double)memsize : 0;
                snprintf(rows[n].name, sizeof rows[n].name, "%s", b[j].name);
                n++;
                break;
            }
    qsort(rows, (size_t)n, sizeof *rows, row_cmp);
    free(a); free(b); free(da); /* db 转交调用方 */
    *rows_out = rows;
    *denied_out = db;
    *ndenied_out = ndb;
    return n;
}

static void print_top_procs(void) {
    printf("%s", L("功耗 Top 进程 (按瞬时 CPU% 降序; 平均%=启动以来累计÷存活时长; GPU 无公开数据):\n",
                   "Top power processes (by instantaneous CPU%, desc; avg% = cumulative÷lifetime; no public GPU data):\n"));
    struct top_row *rows = NULL;
    struct proc_sample *db = NULL;
    int ndb = 0;
    int n = gather_top(&rows, &db, &ndb);
    if (n < 0) {
        printf("%s", L("  枚举失败\n", "  enumeration failed\n"));
        return;
    }
    fputs("  ", stdout);
    hdr_cell("PID", 6);                 /* 对应 %6d */
    hdr_cell("CPU", 6);                 /* %5.1f%% */
    hdr_cell(L("平均", "Avg"), 6);      /* %5.1f%% */
    hdr_cell(L("累计", "Total"), 7);    /* %7s */
    hdr_cell(L("内存", "Mem"), 9);      /* %8.0fM / %8.2fG */
    hdr_cell("%MEM", 5);                /* %5.1f */
    printf(" %s\n", L("进程", "Process"));
    int shown = 0;
    for (int i = 0; i < n && shown < 10 && rows[i].cpu >= 0.1; i++) {
        double mb = rows[i].rss / 1048576.0;
        char ct[16];
        fmt_cpu_ns(rows[i].cpu_ns, ct);
        if (mb >= 1024)
            printf("  %6d %5.1f%% %5.1f%% %7s %8.2fG %5.1f  %s\n",
                   rows[i].pid, rows[i].cpu, rows[i].avg, ct, mb / 1024, rows[i].mempct, rows[i].name);
        else
            printf("  %6d %5.1f%% %5.1f%% %7s %8.0fM %5.1f  %s\n",
                   rows[i].pid, rows[i].cpu, rows[i].avg, ct, mb, rows[i].mempct, rows[i].name);
        shown++;
    }
    if (!shown) printf("%s", L("  (全部空闲)\n", "  (all idle)\n"));
    /* 无权限读占用的系统/其他用户进程: 列几个名字, 提示 root 可见全部 */
    if (ndb > 0) {
        qsort(db, (size_t)ndb, sizeof *db, denied_cmp);
        printf(L("  另有 %d 个系统/其他用户进程无权限读取占用 (sudo fansctl power 可见), 如:",
                 "  %d more system/other-user processes unreadable without privileges (visible via sudo fansctl power), e.g.:"),
               ndb);
        for (int i = 0; i < ndb && i < 3; i++)
            printf("%s%s", i ? "," : " ", db[i].name);
        printf("\n");
    }
    free(rows); free(db);
}

static int power_cmd(void) {
    struct power_info p;
    int has_batt = power_read(&p) == 0;
    /* 机器功率: PSTR 秒级实时, 与菜单栏第一行同源; 充电时拆出充电分量 */
    double pw = -1;
    read_key_value("PSTR", NULL, &pw, NULL);
    if (pw > 0 && pw < 1000) {
        if (p.charge_w > 0 && pw > p.charge_w)
            printf(L("机器功率: %.0f W (PSTR) = 系统 %.0f W + 充电 %.0f W\n",
                     "System power: %.0f W (PSTR) = %.0f W system + %.0f W charging\n"),
                   pw, pw - p.charge_w, p.charge_w);
        else
            printf(L("机器功率: %.0f W (PSTR, 秒级实时)\n",
                     "System power: %.0f W (PSTR, real-time at second level)\n"), pw);
    } else {
        printf("%s", L("机器功率: 未知 (本机无 PSTR 键)\n",
                       "System power: unknown (no PSTR key on this machine)\n"));
    }
    if (p.sys_v > 0 && p.sys_i > 0) {
        double w = p.sys_w > 0 ? p.sys_w : p.sys_v * p.sys_i;
        printf(L("电源输入: %.1f V × %.2f A = %.1f W (实测, 遥测约分钟级刷新)\n",
                 "Power input: %.1f V × %.2f A = %.1f W (measured, telemetry refreshes ~every minute)\n"),
               p.sys_v, p.sys_i, w);
    } else if (p.ext) {
        printf("%s", L("电源输入: 已连接 (无实时遥测)\n", "Power input: connected (no real-time telemetry)\n"));
    } else {
        printf("%s", L("电源输入: 未连接 (电池供电)\n", "Power input: none (running on battery)\n"));
    }
    if (p.adapter_w > 0) {
        char av[32] = "";
        if (p.adapter_v > 0) snprintf(av, sizeof av, L(", 协商 %d V", ", negotiated %d V"), p.adapter_v);
        printf(L("适配器: %d W 额定%s\n", "Adapter: %d W rated%s\n"), p.adapter_w, av);
    } else if (p.ext) {
        printf("%s", L("适配器: 已连接 (额定功率未知)\n", "Adapter: connected (rated watts unknown)\n"));
    }
    if (has_batt) {
        if (p.charging && p.charge_w > 0)
            printf(L("电池: 充电中 %.2f V / %.0f mA (%.1f W)\n",
                     "Battery: charging %.2f V / %.0f mA (%.1f W)\n"), p.batt_v, p.batt_a, p.charge_w);
        else if (p.batt_a < -50)
            printf(L("电池: 放电 %.2f V / %.0f mA (%.1f W)\n",
                     "Battery: discharging %.2f V / %.0f mA (%.1f W)\n"),
                   p.batt_v, -p.batt_a, -p.batt_a * p.batt_v / 1000.0);
        else
            printf(L("电池: %.2f V / %.0f mA (未充放)\n",
                     "Battery: %.2f V / %.0f mA (idle)\n"), p.batt_v, p.batt_a);
    } else {
        printf("%s", L("电池: 本机无电池\n", "Battery: none on this machine\n"));
    }
    print_usb_power();
    print_top_procs();
    return 0;
}

static void json_power(void) {
    struct power_info p;
    int has_batt = power_read(&p) == 0;
    double pw = -1;
    read_key_value("PSTR", NULL, &pw, NULL);
    printf("{\"system_w\":");          /* PSTR, 充电时含充电分量 (battery.charge_w 可拆出) */
    if (pw > 0 && pw < 1000) printf("%.1f", pw); else fputs("null", stdout);
    if (p.sys_v > 0 && p.sys_i > 0) {  /* 输入遥测 (约分钟级刷新) */
        double w = p.sys_w > 0 ? p.sys_w : p.sys_v * p.sys_i;
        printf(",\"input\":{\"voltage_v\":%.1f,\"current_a\":%.2f,\"watts\":%.1f}",
               p.sys_v, p.sys_i, w);
    }
    if (p.adapter_w > 0) {
        printf(",\"adapter\":{\"rated_w\":%d", p.adapter_w);
        if (p.adapter_v > 0) printf(",\"negotiated_v\":%d", p.adapter_v);
        printf("}");
    }
    printf(",\"external_power\":%s", p.ext ? "true" : "false");
    if (has_batt)
        printf(",\"battery\":{\"present\":true,\"charging\":%s,\"voltage_v\":%.2f,"
               "\"current_ma\":%.0f,\"charge_w\":%.1f}",
               p.charging ? "true" : "false", p.batt_v, p.batt_a, p.charge_w);
    else
        printf(",\"battery\":{\"present\":false}");
    struct usb_dev *usb = NULL;
    int nusb = collect_usb(&usb);
    if (nusb < 0) nusb = 0;
    printf(",\"usb\":[");
    for (int i = 0; i < nusb; i++) {
        if (i) putchar(',');
        printf("{\"name\":"); json_str(usb[i].name);
        printf(",\"vendor\":"); json_str(usb[i].vendor);
        printf(",\"speed\":\"%s\"", usb[i].speed);
        if (usb[i].ma > 0) printf(",\"declared_ma\":%d", usb[i].ma);
        printf(",\"self_powered\":%s}", usb[i].self_powered ? "true" : "false");
    }
    printf("]");
    free(usb);
    struct top_row *rows = NULL;
    struct proc_sample *db = NULL;
    int ndb = 0;
    int n = gather_top(&rows, &db, &ndb);
    printf(",\"top_processes\":[");
    for (int i = 0; i < n && i < 10 && rows[i].cpu >= 0.1; i++) {
        if (i) putchar(',');
        printf("{\"pid\":%d,\"name\":", rows[i].pid);
        json_str(rows[i].name);
        printf(",\"cpu_pct\":%.1f,\"avg_pct\":%.1f,\"total_cpu_s\":%.1f,"
               "\"memory_mb\":%.1f,\"mem_pct\":%.1f}",
               rows[i].cpu, rows[i].avg, rows[i].cpu_ns / 1e9,
               rows[i].rss / 1048576.0, rows[i].mempct);
    }
    printf("],\"unprivileged_processes\":%d}\n", ndb);
    free(rows); free(db);
}

/* ============ 菜单栏的 root 侧入口 (经授权弹窗重新执行自身) ============ */

/* pidfile 里的 pid 现在还是不是我们的守护进程? pid 复用后盲目 kill 会误伤无辜
   进程: proc_pidpath 对比可执行文件路径, 与自身 (含菜单栏等价的启动路径) 不符
   即拒绝发送信号。查不到路径时保守放弃 kill, 提示手工处理 */
static int self_path(char out[PATH_MAX]); /* 定义在 LaunchAgent 管理一节 */
static int pid_is_ours(int pid) {
    char path[PATH_MAX];
    if (proc_pidpath(pid, path, sizeof path) <= 0) return 0;
    char self[PATH_MAX];
    if (self_path(self) != 0) return 0;
    return strcmp(path, self) == 0;
}

/* 结束运行中的智能模式: SIGTERM 优雅退出(其信号处理器恢复自动并清 pidfile)。
   CLI "smart stop" 与隐藏 "__smart stop" 共用 */
static int smart_stop(void) {
    int pid; double lo, hi;
    if (!smart_pid_read(&pid, &lo, &hi)) { fprintf(stderr, "%s", L("智能模式未在运行\n", "smart mode is not running\n")); return 1; }
    if (!pid_is_ours(pid)) {
        fprintf(stderr, L("pid %d 已不是 fansctl 进程 (pid 复用?), 拒绝误杀; 请手工清理 %s\n",
                          "pid %d is no longer a fansctl process (pid reuse?), refusing to kill; clean %s manually\n"),
                pid, SMART_PIDFILE);
        return 1;
    }
    if (kill(pid, SIGTERM) != 0) { perror("kill"); return 1; }
    /* 等守护进程完成收尾(恢复自动 + 清 pidfile)再返回, 否则它的退出清理会
       覆盖调用方紧接着的设速(实测: stop 后立即 __apply max 被 fan_auto 覆盖)。
       以 pidfile 消失为准(清理在恢复自动之后), 超时如实报失败而非假装成功 */
    for (int i = 0; i < 100; i++) {
        int p; double l, h;
        if (!smart_pid_read(&p, &l, &h) || p != pid) break;
        usleep(100 * 1000);
    }
    {
        int p; double l, h;
        if (smart_pid_read(&p, &l, &h) && p == pid) {
            fprintf(stderr, "%s", L("智能模式 10 秒内未完成退出, 请查 /tmp/fansctl.smart.log\n",
                                    "smart mode did not exit within 10 s, check /tmp/fansctl.smart.log\n"));
            return 1;
        }
    }
    printf("%s", L("智能模式已停止(恢复自动)\n", "smart mode stopped (auto restored)\n"));
    return 0;
}

/* ==================== 恒温模式 (需 root, PI 闭环, Ctrl+C 退出并恢复自动) ==================== */

/* 与 smart_stop 同款"等收尾"语义 */
static int hold_stop(void) {
    int pid; double t;
    if (!hold_pid_read(&pid, &t)) { fprintf(stderr, "%s", L("恒温模式未在运行\n", "thermostat is not running\n")); return 1; }
    if (!pid_is_ours(pid)) {
        fprintf(stderr, L("pid %d 已不是 fansctl 进程 (pid 复用?), 拒绝误杀; 请手工清理 %s\n",
                          "pid %d is no longer a fansctl process (pid reuse?), refusing to kill; clean %s manually\n"),
                pid, HOLD_PIDFILE);
        return 1;
    }
    if (kill(pid, SIGTERM) != 0) { perror("kill"); return 1; }
    for (int i = 0; i < 100; i++) {
        int p; double x;
        if (!hold_pid_read(&p, &x) || p != pid) break;
        usleep(100 * 1000);
    }
    {
        int p; double x;
        if (hold_pid_read(&p, &x) && p == pid) {
            fprintf(stderr, "%s", L("恒温模式 10 秒内未完成退出, 请查 /tmp/fansctl.hold.log\n",
                                    "thermostat did not exit within 10 s, check /tmp/fansctl.hold.log\n"));
            return 1;
        }
    }
    printf("%s", L("恒温模式已停止(恢复自动)\n", "thermostat stopped (auto restored)\n"));
    return 0;
}

/* PI 闭环: 转速增量 = Kp*(e-e_prev) + Ki*e + Kd*dT (速度形式, 天然无积分饱和;
   dT 为趋势项, 温度已在下降时提前收油门, 抑制超调)。
   控制输入用 EMA 平滑温度(α=0.3) — 最热传感器秒级抖动可达 ±3°C, 直接进 PI 会锯齿。
   死区(|e|<0.3 且 |dT|<0.05)内保持防抖; 低于目标 3°C 让权系统自动(风扇可停转),
   回到 1.5°C 内重新接管; 每拍增量限幅 ±250rpm 防跳变 */
static int hold_loop(double t_set) {
    int n = fan_count();
    if (n < 1) n = 1;
    double base[10] = {0};   /* 接管时的起始转速 */
    double rpm[10] = {0}, last[10];
    int manual[10] = {0};
    for (int i = 0; i < 10; i++) last[i] = -1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGUSR1, on_reload_signal); /* __hold target <°C> 就地更新目标 */
    printf(L("恒温模式: 目标最热传感器 %.1f°C, PI 闭环控制; Ctrl+C 退出并恢复自动\n",
             "Thermostat: holding hottest sensor at %.1f°C, PI closed loop; Ctrl+C exits and restores auto\n"), t_set);
    mach_timebase_info(&g_timebase);
    uint64_t tick_ns = cont_ns();
    double e_prev = 0, t_prev = -999;
    int rc = 0, notified = 0;
    int wfail = 0; /* 连续写入失败数: 与智能模式同款退避 */
    while (!g_stop) {
        if (wfail > 0) { /* SMC 楔住退避: 不硬敲不退出, 温度照读 */
            uint64_t until = tick_ns + wedge_delay_ns(wfail);
            while (cont_ns() < until && !g_stop) usleep(100 * 1000);
            tick_ns = cont_ns();
            if (g_stop) break;
        }
        if (g_reload) { /* pidfile 里的新目标就地生效, 不打断控制 */
            g_reload = 0;
            int p; double nt = 0;
            if (hold_pid_read(&p, &nt) && p == (int)getpid() && nt >= 30 && nt <= 110) {
                t_set = nt;
                printf(L("[目标更新为 %.1f°C]\n", "[target updated to %.1f°C]\n"), t_set);
                fflush(stdout);
            }
        }
        const char *tkey = NULL;
        double T = hottest_temp(&tkey);
        if (T < -999) {
            for (int i = 0; i < 3 && T < -999 && !g_stop; i++) {
                sleep(1);
                smc_close();
                if (smc_open() != 0) continue;
                T = hottest_temp(&tkey);
            }
            if (T < -999) { fprintf(stderr, "%s", L("温度读取失败(已重试)\n", "temperature read failed (after retries)\n")); rc = 1; break; }
        }
        double Ts = t_prev > -999 ? t_prev + 0.3 * (T - t_prev) : T; /* EMA 平滑 */
        double e = Ts - t_set;
        double dT = t_prev > -999 ? Ts - t_prev : 0;
        /* 压不住告警: 持续高于目标 3°C (PI 此时必然已顶到满速) 通知一次, 回落后解除 */
        if (Ts > t_set + 3.0 && !notified) {
            notified = 1;
            char m[160];
            snprintf(m, sizeof m,
                     L("恒温目标 %.1f°C 压不住: 当前 %.1f°C, 风扇已满速 —— 请检查散热",
                       "thermostat cannot hold %.1f°C: now %.1f°C, fans at full speed — check cooling"),
                     t_set, Ts);
            notify_send(m);
            printf("%s", L("[已发送过热通知]\n", "[overheat notification sent]\n"));
            fflush(stdout);
        } else if (notified && Ts < t_set + 1.0) {
            notified = 0;
        }
        for (int f = 0; f < n; f++) {
            double mn = 0, mx = 6000;
            read_rpm_key(f, "Mn", &mn);
            read_rpm_key(f, "Mx", &mx);
            if (manual[f] && Ts < t_set - 3.0) { /* 让权迟滞: 远低于目标交给系统 */
                if (fan_auto(f) != 0) { rc = 1; break; }
                manual[f] = 0;
                last[f] = -1;
            }
            if (manual[f]) { /* MODE_REASSERT: 手动权/目标被外部改写则重新接管 */
                int m = fan_mode(f);
                double tg = -1;
                read_rpm_key(f, "Tg", &tg);
                if ((m >= 0 && m != 1) ||
                    (m == 1 && tg > 0 && last[f] > 0 && fabs(tg - last[f]) > 150)) {
                    printf(L("[风扇%d 被外部改写(模式=%d 目标=%.0f 期望=%.0f), 重新接管]\n",
                             "[fan %d overridden externally (mode=%d target=%.0f expected=%.0f), retaking]\n"),
                           f, m, tg, last[f]);
                    fflush(stdout);
                    if (m == 1) last[f] = -1;
                    else { manual[f] = 0; last[f] = -1; }
                }
            }
            if (!manual[f] && Ts > t_set - 1.5) { /* 回到迟滞带内重新接管 */
                double tg = -1, ac = -1;
                read_rpm_key(f, "Tg", &tg);
                read_rpm_key(f, "Ac", &ac);
                if (fan_mode(f) == 1) base[f] = mn;          /* 手动残留, Tg 不可信 */
                else if (tg >= mn && tg <= mx) base[f] = tg; /* 系统当前意图 */
                else if (ac > mn) base[f] = ac; /* 刚停其他控制器, Tg 还没恢复 */
                if (base[f] < mn) base[f] = mn;
                if (engage_manual(f) != 0) { rc = 1; break; }
                manual[f] = 1;
                rpm[f] = base[f];
                last[f] = -1;
            }
            if (!manual[f]) continue;
            double delta = 0;
            if (!(fabs(e) < 0.3 && fabs(dT) < 0.05))
                delta = 25.0 * (e - e_prev) + 8.0 * e + 150.0 * dT;
            if (delta > 250) delta = 250;
            if (delta < -250) delta = -250;
            rpm[f] += delta;
            if (rpm[f] < mn) rpm[f] = mn;
            if (rpm[f] > mx) rpm[f] = mx;
            if (last[f] < 0 || fabs(rpm[f] - last[f]) > 10) {
                if (write_tg(f, rpm[f]) != 0) {
                    /* 写楔住: 进退避, 不退出 (同智能模式) */
                    wfail++;
                    if (wfail == 1)
                        printf("%s", L("[写入被 SMC 拒绝, 进入退避监测 (最长 60 秒/次)]\n",
                                       "[write refused by SMC, backing off (up to 60 s between retries)]\n"));
                    fflush(stdout);
                    continue;
                }
                wfail = 0;
                last[f] = rpm[f];
            }
        }
        if (rc != 0) break;
        printf(L("[→%.1f°C] %s %.1f°C(平%.1f) 误差%+.1f 趋势%+.2f |",
                 "[→%.1f°C] %s %.1f°C(avg %.1f) error %+.1f trend %+.2f |"),
               t_set, tkey ? tkey : "?", T, Ts, e, dT);
        for (int f = 0; f < n; f++) {
            double ac = -1;
            read_rpm_key(f, "Ac", &ac);
            if (manual[f]) printf(L(" 风扇%d %6.0f→%.0f 手动", " fan %d %6.0f→%.0f manual"), f, ac, rpm[f]);
            else           printf(L(" 风扇%d %6.0f rpm 自动", " fan %d %6.0f rpm auto"), f, ac);
        }
        printf("\n");
        fflush(stdout);
        e_prev = e;
        t_prev = Ts;
        sleep(1);
        /* 唤醒让权 (与智能模式同款) */
        uint64_t dt = cont_ns() - tick_ns;
        tick_ns = cont_ns();
        if (dt > 5ULL * 1000000000ULL) {
            printf(L("[间隔 %.0f 秒, 判定为系统唤醒, 暂交系统控制 5 秒]\n",
                     "[gap %.0f s, system wake detected, handing control back for 5 s]\n"), dt / 1e9);
            for (int f = 0; f < n; f++)
                if (manual[f]) {
                    if (fan_auto(f) != 0) { rc = 1; break; }
                    manual[f] = 0;
                    last[f] = -1;
                }
            fflush(stdout);
            if (rc != 0) break;
            for (int i = 0; i < 5 && !g_stop; i++) sleep(1);
            tick_ns = cont_ns();
            t_prev = -999; /* 唤醒后第一拍无趋势 */
        }
    }
    for (int f = 0; f < n; f++)
        if (manual[f]) fan_auto(f);
    if (g_stop) {
        printf("%s", L("\n已退出恒温模式, 全部恢复自动", "\nThermostat exited, all fans restored to auto"));
        if (g_stop_pid > 0) printf(L(" (停止信号来自 pid %d)", " (stop signal from pid %d)"), (int)g_stop_pid);
        printf("\n");
    }
    return rc;
}

/* fansctl __hold <°C> — root 后台运行的恒温模式 (菜单栏启动), pidfile 标记状态。
   自行 fork 守护化 (与 __smart 同款)。argv[2]=="stop"/"target" 给 setuid 助手用。 */
static int hold_hidden_cmd(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[2], "stop") == 0) return hold_stop();
    if (argc > 2 && strcmp(argv[2], "target") == 0) {
        /* 更新运行中恒温模式的目标: 改 pidfile + SIGUSR1, 守护进程就地生效 */
        if (argc < 4) { fprintf(stderr, "%s", L("用法: __hold target <°C>\n", "usage: __hold target <°C>\n")); return 1; }
        double nt = atof(argv[3]);
        if (nt < 30 || nt > 110) { fprintf(stderr, "%s", L("目标温度无效\n", "invalid target temperature\n")); return 1; }
        int pid; double t;
        if (!hold_pid_read(&pid, &t)) { fprintf(stderr, "%s", L("恒温模式未在运行\n", "thermostat is not running\n")); return 1; }
        FILE *f = fopen(HOLD_PIDFILE, "w");
        if (!f) { perror("pidfile"); return 1; }
        fprintf(f, "%d %.1f\n", pid, nt);
        fclose(f);
        if (kill(pid, SIGUSR1) != 0) { perror("kill"); return 1; }
        printf(L("已通知恒温模式更新目标 %.1f°C\n", "thermostat notified: target %.1f°C\n"), nt);
        return 0;
    }
    double t_set = argc > 2 ? atof(argv[2]) : 70;
    if (t_set < 30 || t_set > 110) return 1;
    int pid; double t;
    if (hold_pid_read(&pid, &t)) return 1; /* 已有实例在跑 */
    int spid; double slo, shi;
    if (smart_pid_read(&spid, &slo, &shi)) smart_stop(); /* 互斥: 先收智能的控制权 */
    pid_t d = fork();
    if (d < 0) return 1;
    if (d > 0) _exit(0);
    setsid();
    freopen("/dev/null", "r", stdin);
    freopen("/tmp/fansctl.hold.log", "a", stdout);
    freopen("/tmp/fansctl.hold.log", "a", stderr);
    if (smc_open() != 0) return 1;
    hold_pid_write(t_set);
    int rc = hold_loop(t_set);
    hold_pid_clear();
    smc_close();
    return rc;
}

/* fansctl __apply max|auto|set <rpm>  — 静默作用于全部风扇, 给菜单栏用 */
static int apply_cmd(int argc, char **argv) {
    if (argc < 3) return 1;
    const char *act = argv[2];
    if (smc_open() != 0) return 1;
    int n = fan_count();
    if (n < 1) n = 1;
    int rc = 0;
    if (strcmp(act, "max") == 0) {
        for (int i = 0; i < n; i++) if (fan_max(i) != 0) rc = 1;
    } else if (strcmp(act, "auto") == 0) {
        for (int i = 0; i < n; i++) if (fan_auto(i) != 0) rc = 1;
    } else if (strcmp(act, "set") == 0 && argc > 3) {
        double rpm = atof(argv[3]);
        for (int i = 0; i < n; i++) if (fan_set(i, rpm) != 0) rc = 1;
    } else {
        rc = 1;
    }
    smc_close();
    return rc;
}

/* fansctl __smart 低 高 — root 后台运行的智能模式 (菜单栏启动), pidfile 标记状态。
   自行 fork 守护化: 授权父进程立即退出(菜单栏的授权管道即刻关闭), 子进程脱会话继续跑。
   argv[2]=="stop" 时结束运行中的智能模式 (给 setuid 助手/菜单栏用)。 */
static int smart_hidden_cmd(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[2], "stop") == 0) return smart_stop();
    if (argc > 2 && strcmp(argv[2], "thresh") == 0) {
        /* 更新运行中智能模式的阈值: 改 pidfile + SIGUSR1, 守护进程就地生效 */
        if (argc < 5) { fprintf(stderr, "%s", L("用法: __smart thresh <低°C> <高°C>\n", "usage: __smart thresh <lo°C> <hi°C>\n")); return 1; }
        double nl = atof(argv[3]), nh = atof(argv[4]);
        if (nl < 20 || nh < nl + 5 || nh > 120) { fprintf(stderr, "%s", L("阈值无效\n", "invalid thresholds\n")); return 1; }
        int pid; double lo, hi;
        if (!smart_pid_read(&pid, &lo, &hi)) { fprintf(stderr, "%s", L("智能模式未在运行\n", "smart mode is not running\n")); return 1; }
        FILE *f = fopen(SMART_PIDFILE, "w");
        if (!f) { perror("pidfile"); return 1; }
        fprintf(f, "%d %.0f %.0f\n", pid, nl, nh);
        fclose(f);
        if (kill(pid, SIGUSR1) != 0) { perror("kill"); return 1; }
        printf(L("已通知智能模式更新阈值 %.0f~%.0f°C\n", "smart mode notified: thresholds %.0f~%.0f°C\n"), nl, nh);
        return 0;
    }
    double t_lo = argc > 2 ? atof(argv[2]) : 40;
    double t_hi = argc > 3 ? atof(argv[3]) : 80;
    if (t_lo < 20 || t_hi < t_lo + 5 || t_hi > 120) return 1;
    int pid; double a, b;
    if (smart_pid_read(&pid, &a, &b)) return 1; /* 已有实例在跑 */
    int hpid; double hx;
    if (hold_pid_read(&hpid, &hx)) hold_stop(); /* 互斥: 先收恒温的控制权 */
    pid_t d = fork();
    if (d < 0) return 1;
    if (d > 0) _exit(0);
    setsid();
    freopen("/dev/null", "r", stdin);
    freopen("/tmp/fansctl.smart.log", "a", stdout);
    freopen("/tmp/fansctl.smart.log", "a", stderr);
    if (smc_open() != 0) return 1;
    smart_pid_write(t_lo, t_hi);
    int rc = smart_loop(t_lo, t_hi);
    smart_pid_clear();
    smc_close();
    return rc;
}

/* ==================== 菜单栏 LaunchAgent 管理 (bar install/uninstall/status) ==================== */

#define BAR_LABEL   "local.fansctl.bar"
#define BAR_PIDFILE "/tmp/fansctl.bar.pid"

/* 自身可执行文件绝对路径 (过 realpath 解符号链接: brew 的 bin/ 是链到 Cellar 的) */
static int self_path(char out[PATH_MAX]) {
    char raw[PATH_MAX];
    uint32_t size = sizeof raw;
    if (_NSGetExecutablePath(raw, &size) != 0) return -1;
    return realpath(raw, out) ? 0 : -1;
}

static int run_launchctl(char *const args[], int quiet) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (quiet) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        }
        execv("/bin/launchctl", args);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (g_debug) fprintf(stderr, "[dbg] launchctl %s %s -> rc=%d\n", args[1], args[2], rc);
    return rc;
}

/* 杀掉占着 pidfile 的存活菜单栏实例 (SIGTERM, 等它退场最多 ~2s)。
   菜单栏进程 fork+setsid 后 PPID=1, launchd 的 bootout/kickstart 只认自己
   拉起的那一代 —— 手动启动或已退代实例必须从这里清, 否则单实例守卫
   会让 install/restart 拉起来的新实例立刻退出, 栏上静默跑着旧版 */
static void bar_kill_running(void) {
    FILE *f = fopen(BAR_PIDFILE, "r");
    int pid = 0;
    if (f) {
        if (fscanf(f, "%d", &pid) != 1) pid = 0;
        fclose(f);
    }
    if (pid <= 1 || kill(pid, 0) != 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 20 && kill(pid, 0) == 0; i++) usleep(100 * 1000);
}

static void xml_esc(FILE *f, const char *s) {
    for (; *s; s++) {
        if (*s == '&')      fputs("&amp;", f);
        else if (*s == '<') fputs("&lt;", f);
        else if (*s == '>') fputs("&gt;", f);
        else                fputc(*s, f);
    }
}

/* 从 plist 文本里取 ProgramArguments 的第一个 <string> (我们只写这一种结构, 轻量解析够用) */
static void plist_prog(const char *plist, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(plist, "r");
    if (!f) return;
    char buf[2048];
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[got] = 0;
    char *pa = strstr(buf, "<key>ProgramArguments</key>");
    if (!pa) return;
    char *s = strstr(pa, "<string>");
    if (!s) return;
    s += 8;
    char *e = strstr(s, "</string>");
    if (!e) return;
    size_t len = (size_t)(e - s) < n - 1 ? (size_t)(e - s) : n - 1;
    memcpy(out, s, len);
    out[len] = 0;
}

static int bar_cmd(int argc, char **argv) {
    const char *sub = argc > 2 ? argv[2] : "";
    const char *home = getenv("HOME");
    if (!home || !*home) { fprintf(stderr, "%s", L("无 HOME 环境\n", "no HOME environment\n")); return 1; }
    if (geteuid() == 0) {
        fprintf(stderr, "%s", L("bar 子命令无需 sudo — 它管理的是当前用户的 LaunchAgent\n",
                                "the bar subcommand needs no sudo — it manages the current user's LaunchAgent\n"));
        return 1;
    }
    char plist[PATH_MAX];
    snprintf(plist, sizeof plist, "%s/Library/LaunchAgents/%s.plist", home, BAR_LABEL);
    char domain[32];
    snprintf(domain, sizeof domain, "gui/%d", (int)getuid());
    /* bootout/print 用连写 service-target 形式 (gui/uid/label): 本机实测
       macOS 15 的 bootout 两参数空格形式恒报 EIO(5), 连写形式正常 */
    char target[64];
    snprintf(target, sizeof target, "gui/%d/%s", (int)getuid(), BAR_LABEL);
    char *bootout[]   = {(char *)"launchctl", (char *)"bootout",   target, NULL};
    char *bootstrap[] = {(char *)"launchctl", (char *)"bootstrap", domain, plist, NULL};
    char *printc[]    = {(char *)"launchctl", (char *)"print",     target, NULL};

    if (strcmp(sub, "install") == 0) {
        char exe[PATH_MAX];
        if (self_path(exe) != 0) { fprintf(stderr, "%s", L("无法定位自身路径\n", "cannot resolve own path\n")); return 1; }
        bar_kill_running(); /* launchd 不管的手动实例先退场, 否则单实例守卫让新实例起不来 */
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/Library/LaunchAgents", home);
        mkdir(dir, 0755); /* 已存在则 EEXIST, 忽略 */
        FILE *f = fopen(plist, "w");
        if (!f) { perror(plist); return 1; }
        fprintf(f,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n<dict>\n"
            "\t<key>Label</key><string>%s</string>\n"
            "\t<key>ProgramArguments</key>\n\t<array>\n\t\t<string>", BAR_LABEL);
        xml_esc(f, exe);
        fprintf(f, "</string>\n\t</array>\n"
            "\t<key>RunAtLoad</key><true/>\n"
            "\t<key>KeepAlive</key>\n\t<dict>\n\t\t<key>SuccessfulExit</key><false/>\n\t</dict>\n"
            "\t<key>StandardOutPath</key><string>/tmp/fansctl.bar.log</string>\n"
            "\t<key>StandardErrorPath</key><string>/tmp/fansctl.bar.log</string>\n"
            "</dict>\n</plist>\n");
        fclose(f);
        chmod(plist, 0644);
        /* 已加载的旧实例先按旧配置退场, 再以新 plist 拉起 (幂等重装)。
           bootout 异步收尾, 立即 bootstrap 会撞 EIO(5): 等 print 查不到为止 */
        run_launchctl(bootout, 1);
        for (int i = 0; i < 30; i++) {
            if (run_launchctl(printc, 1) != 0) break;
            usleep(100 * 1000);
        }
        if (run_launchctl(bootstrap, 0) != 0) {
            fprintf(stderr, "%s", L("launchctl bootstrap 失败\n", "launchctl bootstrap failed\n"));
            return 1;
        }
        printf(L("已安装并启动菜单栏:\n  %s\n  程序 %s\n  日志 /tmp/fansctl.bar.log (登录自启, 异常退出自动拉起)\n",
                 "Menu-bar app installed and started:\n  %s\n  program %s\n  log /tmp/fansctl.bar.log (starts at login, auto-restarted on crash)\n"),
               plist, exe);
        return 0;
    }
    if (strcmp(sub, "uninstall") == 0) {
        run_launchctl(bootout, 1); /* 未加载也无妨 */
        bar_kill_running(); /* launchd 之外的存活实例不会被 bootout 带走 */
        if (unlink(plist) != 0 && errno != ENOENT) { perror(plist); return 1; }
        printf("%s", L("已卸载: 菜单栏停止, LaunchAgent 已移除\n",
                       "Uninstalled: menu-bar app stopped, LaunchAgent removed\n"));
        return 0;
    }
    if (strcmp(sub, "status") == 0) {
        int has_plist = access(plist, F_OK) == 0;
        printf(L("开机自启: %s\n  %s\n", "Auto-start: %s\n  %s\n"),
               has_plist ? L("已安装", "installed") : L("未安装 (fansctl bar install 可装)",
                                                        "not installed (see fansctl bar install)"), plist);
        if (has_plist) {
            char prog[PATH_MAX] = "";
            plist_prog(plist, prog, sizeof prog);
            if (prog[0])
                printf(L("  程序 %s%s\n", "  program %s%s\n"), prog,
                       access(prog, X_OK) == 0 ? "" : L(" (文件不存在!)", " (missing!)"));
            int rc = run_launchctl(printc, 1);
            printf(L("launchd: %s\n", "launchd: %s\n"),
                   rc == 0 ? L("已加载", "loaded") : L("未加载", "not loaded"));
            FILE *pf = fopen(BAR_PIDFILE, "r");
            int pid = 0, alive = 0;
            if (pf) {
                if (fscanf(pf, "%d", &pid) == 1 && pid > 0 && kill(pid, 0) == 0) alive = 1;
                fclose(pf);
            }
            if (alive)
                printf(L("菜单栏进程: 运行中 (pid %d)\n", "Menu-bar process: running (pid %d)\n"), pid);
            else
                printf("%s", L("菜单栏进程: 未运行\n", "Menu-bar process: not running\n"));
        }
        return 0;
    }
    fprintf(stderr, "%s", L("用法: fansctl bar install|uninstall|status\n",
                            "usage: fansctl bar install|uninstall|status\n"));
    return 1;
}

/* ==================== 诊断 (doctor): 报 issue 时附上, 免来回追问 ==================== */

static void sysctl_str(const char *name, char *out, size_t n) {
    out[0] = 0;
    sysctlbyname(name, out, &n, NULL, 0); /* 失败留空串 */
}

static int doctor_cmd(void) {
    printf("fansctl %s\n", FANSCTL_VERSION);
    char model[64] = "", chip[128] = "", osv[32] = "", osb[32] = "";
    struct utsname u;
    memset(&u, 0, sizeof u);
    uname(&u);
    sysctl_str("hw.model", model, sizeof model);
    sysctl_str("machdep.cpu.brand_string", chip, sizeof chip);
    sysctl_str("kern.osproductversion", osv, sizeof osv);
    sysctl_str("kern.osversion", osb, sizeof osb);
    printf(L("系统: %s  芯片 %s  macOS %s (%s)  %s\n",
             "System: %s  chip %s  macOS %s (%s)  %s\n"),
           model, chip, osv, osb, u.machine);

    if (smc_open() == 0) {
        printf(L("AppleSMC: 可用, %u 个键\n", "AppleSMC: available, %u keys\n"), total_keys());
        int n = fan_count();
        if (n > 0) {
            printf(L("风扇: %d 个\n", "Fans: %d\n"), n);
            for (int i = 0; i < n; i++) {
                double ac = -1, mn = -1, mx = -1;
                read_rpm_key(i, "Ac", &ac);
                read_rpm_key(i, "Mn", &mn);
                read_rpm_key(i, "Mx", &mx);
                char md[5];
                int have_md = fan_md_key(i, md);
                int mode = fan_mode(i);
                printf(L("  风扇%d: 当前 %.0f rpm  范围 %.0f~%.0f  模式键 %s  模式 %s\n",
                         "  fan %d: current %.0f rpm  range %.0f~%.0f  mode key %s  mode %s\n"),
                       i, ac, mn, mx, have_md ? md : "-",
                       mode == 1 ? L("手动", "manual") : mode == 0 ? L("自动", "auto") : L("未知", "unknown"));
            }
        } else {
            printf("%s", L("风扇: 未发现 (FNum 不可读)\n", "Fans: none found (FNum unreadable)\n"));
        }
        scan_temp_keys();
        const char *hk = NULL;
        double ht = hottest_temp(&hk);
        if (g_tkey_n > 0) {
            const char *hn = hk ? temp_key_name(hk, g_lang_en) : NULL;
            if (hn)
                printf(L("温度传感器: %d 个, 最热 %s (%s) %.1f°C\n",
                         "Temperature sensors: %d, hottest %s (%s) %.1f°C\n"),
                       g_tkey_n, hk ? hk : "?", hn, ht);
            else
                printf(L("温度传感器: %d 个, 最热 %s %.1f°C\n",
                         "Temperature sensors: %d, hottest %s %.1f°C\n"),
                       g_tkey_n, hk ? hk : "?", ht);
        }
        else
            printf("%s", L("温度传感器: 未发现\n", "Temperature sensors: none found\n"));
        int unnamed = 0;
        for (int i = 0; i < g_tkey_n; i++)
            if (!temp_key_name(g_tkeys[i], 0)) unnamed++;
        printf(L("未命名传感器: %d 个 (欢迎提 issue 补充命名表)\n",
                 "unnamed sensors: %d (issues welcome to extend the naming table)\n"),
               unnamed);
        printf(L("PSTR (机器功率键): %s\n", "PSTR (system power key): %s\n"),
               key_exists("PSTR") ? L("存在", "present") : L("不存在", "absent"));
        printf(L("Ftst (手动模式解锁键): %s\n", "Ftst (manual-mode unlock key): %s\n"),
               key_exists("Ftst") ? L("存在", "present") : L("不存在", "absent"));
        smc_close();
    } else {
        printf("%s", L("AppleSMC: 不可用 (虚拟机或无 SMC 的机型), 风扇功能不可用\n",
                       "AppleSMC: unavailable (VM or a machine without SMC), fan control unavailable\n"));
    }

    struct stat hst;
    if (stat("/usr/local/bin/fansctl-root", &hst) == 0) {
        int suid = (hst.st_mode & S_ISUID) != 0;
        printf(L("root 助手: 已安装, setuid=%s\n",
                 "root helper: installed, setuid=%s\n"),
               suid ? L("是 (菜单栏免密控制)", "yes (passwordless control from the menu bar)")
                    : L("否 (重装: make install-root)", "no (reinstall: make install-root)"));
    } else {
        printf("%s", L("root 助手: 未安装 (菜单栏控制将每次弹密码框; make install-root 可装)\n",
                       "root helper: not installed (menu bar prompts for a password each time; make install-root)\n"));
    }

    char plist[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(plist, sizeof plist, "%s/Library/LaunchAgents/%s.plist",
             (home && *home) ? home : "", BAR_LABEL);
    int has_plist = access(plist, F_OK) == 0;
    printf(L("开机自启 (LaunchAgent): %s\n", "Auto-start (LaunchAgent): %s\n"),
           has_plist ? L("已安装", "installed")
                     : L("未安装 (fansctl bar install 可装)", "not installed (see fansctl bar install)"));
    if (has_plist) {
        char prog[PATH_MAX] = "";
        plist_prog(plist, prog, sizeof prog);
        if (prog[0])
            printf(L("  程序 %s%s\n", "  program %s%s\n"), prog,
                   access(prog, X_OK) == 0 ? "" : L(" (文件不存在!)", " (missing!)"));
    }

    int pid;
    double a, b, t;
    if (smart_pid_read(&pid, &a, &b))
        printf(L("智能模式: 运行中 (pid %d, %.0f~%.0f°C)\n",
                 "Smart mode: running (pid %d, %.0f~%.0f°C)\n"), pid, a, b);
    else
        printf("%s", L("智能模式: 未运行\n", "Smart mode: not running\n"));
    if (hold_pid_read(&pid, &t))
        printf(L("恒温模式: 运行中 (pid %d, →%.1f°C)\n",
                 "Thermostat: running (pid %d, →%.1f°C)\n"), pid, t);
    else
        printf("%s", L("恒温模式: 未运行\n", "Thermostat: not running\n"));

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("AppleSmartBattery"));
    if (svc) {
        CFMutableDictionaryRef props = NULL;
        if (IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, kNilOptions) == KERN_SUCCESS && props) {
            double cyc = cfnum_to_double(CFDictionaryGetValue(props, CFSTR("CycleCount")));
            CFTypeRef x = CFDictionaryGetValue(props, CFSTR("ExternalConnected"));
            int ext = (x && x == kCFBooleanTrue);
            if (cyc >= 0)
                printf(L("电池: 有, 循环 %.0f 次, %s\n", "Battery: present, %.0f cycles, %s\n"),
                       cyc, ext ? L("外接电源", "on AC power") : L("电池供电", "on battery"));
            else
                printf("%s", L("电池: 有 (循环数未知)\n", "Battery: present (cycle count unknown)\n"));
            CFRelease(props);
        }
        IOObjectRelease(svc);
    } else {
        printf("%s", L("电池: 无 (台式机)\n", "Battery: none (desktop)\n"));
    }
    printf("%s", L("提交 issue 请附上以上全部输出。\n",
                   "Please attach this full output when filing an issue.\n"));
    return 0;
}
/* ==================== 内置自测 (__selftest, 无需 SMC/root) ====================
   纯函数表驱动断言, CI 虚拟机 (无 AppleSMC) 也能跑; 命名表/JSON 转义/plist 解析
   这类逻辑改动当场回归。守护 pidfile 往返测试在有守护进程运行时会踩状态文件,
   探测到即跳过 */
static int g_st_n = 0, g_st_fail = 0;
static void st_ok(int cond, const char *what) {
    g_st_n++;
    if (cond) { printf("  PASS  %s\n", what); return; }
    g_st_fail++;
    printf("  FAIL  %s\n", what);
}
static void st_streq(const char *got, const char *want, const char *what) {
    g_st_n++;
    if (got && strcmp(got, want) == 0) { printf("  PASS  %s\n", what); return; }
    g_st_fail++;
    printf("  FAIL  %s\n        got \"%s\" want \"%s\"\n",
           what, got ? got : "(null)", want);
}
static int selftest_cmd(void) {
    printf("%s", L("[自测] 传感器命名表\n", "[selftest] sensor naming table\n"));
    st_streq(temp_key_name("TB0T", 0), "电池", "TB0T zh");
    st_streq(temp_key_name("TB0T", 1), "Battery", "TB0T en");
    st_streq(temp_key_name("TW0P", 1), "AirPort (Wi-Fi)", "TW0P en");
    st_streq(temp_key_name("TS0P", 0), "掌托左", "TS0P zh");
    st_streq(temp_key_name("TAOL", 1), "Ambient", "TAOL en");
    st_streq(temp_key_name("Tp05", 0), "CPU 性能核", "Tp05 zh family");
    st_streq(temp_key_name("Te01", 1), "CPU E-core", "Te01 en family");
    st_streq(temp_key_name("Tg04", 1), "GPU core", "Tg04 en family");
    st_streq(temp_key_name("Tm02", 0), "内存", "Tm02 zh family");
    st_streq(temp_key_name("TH0x", 1), "NAND flash", "TH0x en family");
    st_streq(temp_key_name("Th00", 1), "NAND flash", "Th00 en family");
    st_streq(temp_key_name("Tz11", 0), "热区", "Tz11 zh family");
    st_streq(temp_key_name("TCMz", 1), "SoC max", "TCMz en family");
    st_streq(temp_key_name("TC10", 0), "SoC 组", "TC10 zh family");
    st_streq(temp_key_name("TaLP", 0), "气流·左", "TaLP zh family");
    st_streq(temp_key_name("TaRF", 1), "Airflow R", "TaRF en family");
    st_streq(temp_key_name("Ta05", 0), "气流", "Ta05 zh family");
    st_streq(temp_key_name("TB3T", 1), "Battery", "TB3T en pattern");
    st_ok(temp_key_name("TD00", 0) == NULL, "TD00 无映射");
    st_ok(temp_key_name("TPD0", 1) == NULL, "TPD0 无映射");
    st_ok(temp_key_name("TVMD", 0) == NULL, "TVMD 无映射");
    st_ok(temp_key_name("Tpx", 0) == NULL, "短键 Tpx 不误匹配");
    st_ok(temp_key_name("Tp05x", 0) == NULL, "长键不误匹配");
    st_ok(temp_key_name("F0Ac", 0) == NULL, "非温度键无映射");

    printf("%s", L("[自测] JSON 转义与数值\n", "[selftest] JSON escaping & numbers\n"));
    char b[256];
    json_escape(b, sizeof b, "a\"b\\c\nd");
    st_streq(b, "a\\\"b\\\\c\\nd", "json_escape 转义 \" \\ \\n");
    json_escape(b, sizeof b, "中文°C");
    st_streq(b, "中文°C", "json_escape UTF-8 透传");
    json_escape(b, sizeof b, "a\x01");
    st_streq(b, "a\\u0001", "json_escape 控制字符");
    jnum_buf(b, sizeof b, -1, 0);   st_streq(b, "null", "jnum 未知=null");
    jnum_buf(b, sizeof b, 70.256, 2); st_streq(b, "70.26", "jnum 精度");
    jnum_buf(b, sizeof b, -0.4, 0); st_streq(b, "0", "jnum -0.4 边界");

    printf("%s", L("[自测] plist 解析与 XML 转义\n", "[selftest] plist parsing & XML escaping\n"));
    const char *tpl = "/tmp/fansctl-selftest.plist";
    FILE *f = fopen(tpl, "w");
    fputs("<?xml version=\"1.0\"?><plist><dict><key>Label</key><string>local.test</string>"
          "<key>ProgramArguments</key><array><string>/bin/fansctl</string></array>"
          "</dict></plist>", f);
    fclose(f);
    char prog[64];
    plist_prog(tpl, prog, sizeof prog);
    st_streq(prog, "/bin/fansctl", "plist_prog 取 ProgramArguments");
    f = fopen(tpl, "w"); fputs("<plist><dict><key>Label</key></dict></plist>", f); fclose(f);
    plist_prog(tpl, prog, sizeof prog);
    st_streq(prog, "", "plist_prog 缺 ProgramArguments -> 空");
    unlink(tpl);
    f = tmpfile();
    xml_esc(f, "a&b<c>");
    rewind(f);
    size_t got = fread(b, 1, sizeof b - 1, f);
    b[got] = 0;
    fclose(f);
    st_streq(b, "a&amp;b&lt;c&gt;", "xml_esc 转义");

    printf("%s", L("[自测] 温度合理区间\n", "[selftest] temperature plausibility\n"));
    st_ok(!plausible_temp(-10) && plausible_temp(0) && plausible_temp(60) &&
          plausible_temp(129.9) && !plausible_temp(130), "plausible_temp 边界");

    printf("%s", L("[自测] 功耗前馈闭环仿真 (热惯性模型)\n",
                   "[selftest] power feedforward closed-loop simulation (thermal-mass model)\n"));
    {
        /* 合成热模型: C·dT/dt = P_heat - h(rpm)·(T - T_amb)。前馈只依赖功耗序列
           (纯函数 ff_tick), 温度只用来核验前馈效果 —— 功耗阶跃后温度还在爬的
           最初几拍, 前馈应已把转速抬起来 */
        const double TAMB = 30.0, HEAT = 0.9, HMAX = 1.6;
        double T = 45.0, rpm = 1200.0;
        struct ff_state st;
        ff_init(&st);
        int boosted_in_3 = 0; /* 功耗 30→90W 后 3 拍内前馈是否已 >0 */
        double peak_ff = 0, peak_T = 0;
        for (int tick = 0; tick < 60; tick++) {
            double W = tick < 20 ? 30.0 : 90.0; /* 第 20 拍 3 倍负载阶跃 */
            double ff = ff_tick(&st, W);
            rpm += (1200.0 + ff - rpm) * 0.8;   /* 转速一阶滞后趋近目标 */
            double h = HEAT + (HMAX - HEAT) * (rpm - 1200.0) / 4800.0;
            T += (W * 0.02 * HEAT - h * (T - TAMB) * 0.06);
            if (tick >= 20 && tick < 24 && ff > 0) boosted_in_3 = 1;
            if (ff > peak_ff) peak_ff = ff;
            if (T > peak_T) peak_T = T;
        }
        st_ok(boosted_in_3, "前馈在功耗阶跃后 3 拍内响应");
        st_ok(peak_ff > 500 && peak_ff <= FF_MAX + 1e-9, "前馈峰值量级与限幅");
        st_ok(peak_T < 75.0, "闭环仿真温度有界 (<75°C)");
        /* 稳态归零: 功耗回到稳态后快慢 EMA 追平, 前馈必须退场, 不长期多转 */
        {
            struct ff_state st2;
            ff_init(&st2);
            for (int i = 0; i < 40; i++) ff_tick(&st2, 90.0); /* 长时间满载 */
            st_ok(st2.boost == 0, "稳态前馈归零 (不长期多转)");
            /* 数据消失 (拔电/无 PSTR): 增量平滑退场 */
            double b = ff_tick(&st2, 0);
            st_ok(b == 0, "无功耗数据时增量即刻归零");
        }
        /* 充电扣除链: 90W 输入含 30W 充电 -> 前馈输入应等效 60W, 阶跃检测用 60 基线 */
        {
            struct ff_state st3;
            ff_init(&st3);
            for (int i = 0; i < 30; i++) ff_tick(&st3, 60.0);
            double b60 = ff_tick(&st3, 60.0);
            st_ok(b60 == 0, "充电扣除后稳态不前馈");
        }
    }

    printf("%s", L("[自测] 守护 pidfile 往返\n", "[selftest] daemon pidfile roundtrip\n"));
    if (access(SMART_PIDFILE, F_OK) == 0 || access(HOLD_PIDFILE, F_OK) == 0)
        printf("%s", L("  SKIP  有守护进程在跑, 不踩它的状态文件\n",
                       "  SKIP  a daemon is running, not touching its state files\n"));
    else {
        int p; double lo = 0, hi = 0, ht = 0;
        smart_pid_write(45, 85);
        st_ok(smart_pid_read(&p, &lo, &hi) && p == (int)getpid() && lo == 45 && hi == 85,
              "smart pidfile 写读往返");
        smart_pid_clear();
        st_ok(access(SMART_PIDFILE, F_OK) != 0, "smart pidfile 清除");
        hold_pid_write(70);
        st_ok(hold_pid_read(&p, &ht) && p == (int)getpid() && ht == 70,
              "hold pidfile 写读往返");
        hold_pid_clear();
        st_ok(access(HOLD_PIDFILE, F_OK) != 0, "hold pidfile 清除");
    }

    printf(L("自测: %d 项, 失败 %d 项\n", "selftest: %d checks, %d failed\n"), g_st_n, g_st_fail);
    return g_st_fail ? 1 : 0;
}

/* 用法输出 — 不依赖 SMC: 无 AppleSMC 的环境 (如 CI 虚拟机) 敲错命令也能看到帮助 */
static void print_usage(void) {
    fprintf(stderr, "%s", L(
        "用法: fansctl            启动菜单栏应用(fork 后台, 不占终端)\n"
        "      fansctl <子命令>   命令行模式\n"
        "  fans            风扇转速(只读)\n"
        "  status          风扇当前/目标转速与模式\n"
        "  temps [--sort] [--above N]  所有温度传感器 (--sort 最热在前)\n"
        "  power [--watch [秒]]  供电/功率一览: 机器功率, 电源输入, 适配器, USB 设备, 功耗Top进程\n"
        "  dump            导出全部 SMC 键\n"
        "  watch [秒]      循环刷新\n"
        "  (--json)        fans/status/temps/power/watch 的机器可读输出\n"
        "                  (watch / power --watch 每行一个对象)\n"
        "  set <rpm> [N]   设定转速, 不带 N 作用于全部风扇 (需 sudo)\n"
        "  max [N]         全速, 不带 N 作用于全部风扇 (需 sudo)\n"
        "  auto [N]        恢复自动, 不带 N 作用于全部风扇 (需 sudo)\n"
        "  smart [低 高]   智能曲线, 默认 40~80°C; 达到高阈值会发系统通知 (需 sudo)\n"
        "  smart stop      结束智能模式(含菜单栏启动的), 恢复自动 (需 sudo)\n"
        "  hold <°C>       恒温模式, PI 闭环把最热传感器稳定在目标温度,\n"
        "                  默认 70°C; 与智能模式互斥 (需 sudo)\n"
        "  hold stop       结束恒温模式(含菜单栏启动的), 恢复自动 (需 sudo)\n"
        "  bar install     安装菜单栏开机自启 (LaunchAgent, 无需 sudo)\n"
        "  bar uninstall   移除开机自启并停止菜单栏\n"
        "  bar status      查看自启安装与运行状态\n"
        "  doctor          收集诊断信息 (报 issue 请附输出)\n"
        "  version         版本号\n",
        "Usage: fansctl            start the menu-bar app (forks to background)\n"
        "      fansctl <command>   command line mode\n"
        "  fans            fan RPMs (read-only)\n"
        "  status          current/target RPMs and mode\n"
        "  temps [--sort] [--above N]  all temperature sensors (--sort: hottest first)\n"
        "  power [--watch [sec]]  power survey: system draw, input, adapter, USB, top processes\n"
        "  dump            dump all SMC keys\n"
        "  watch [sec]     refresh loop\n"
        "  (--json)        machine-readable JSON for fans/status/temps/power/watch\n"
        "                  (one object per line for watch / power --watch)\n"
        "  set <rpm> [N]   set RPM, all fans without N (sudo)\n"
        "  max [N]         full speed, all fans without N (sudo)\n"
        "  auto [N]        back to automatic, all fans without N (sudo)\n"
        "  smart [lo hi]   smart curve, default 40~80°C; sends a notification at the high threshold (sudo)\n"
        "  smart stop      stop smart mode (incl. menu-bar instances), restore auto (sudo)\n"
        "  hold <°C>       thermostat: PI loop holds the hottest sensor at target,\n"
        "                  default 70°C; mutually exclusive with smart (sudo)\n"
        "  hold stop       stop thermostat (incl. menu-bar instances), restore auto (sudo)\n"
        "  bar install     install the menu-bar auto-start LaunchAgent (no sudo)\n"
        "  bar uninstall   remove auto-start and stop the menu bar\n"
        "  bar status      show auto-start installation and run status\n"
        "  doctor          collect diagnostics (attach when filing issues)\n"
        "  version         print version\n"));
}

int main(int argc, char **argv) {
    cli_lang_init();
    const char *cmd = argc > 1 ? argv[1] : NULL;
    if (cmd && (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0)) {
        printf("fansctl %s\n", FANSCTL_VERSION);
        return 0;
    }
    /* osascript 提权链会把 SIGINT/SIGTERM/SIGHUP 阻塞并跨 exec 继承,
       导致 smart 的优雅退出和 Ctrl+C 全部失效, 在此解除 */
    {
        sigset_t un;
        sigemptyset(&un);
        sigaddset(&un, SIGINT);
        sigaddset(&un, SIGTERM);
        sigaddset(&un, SIGHUP);
        sigprocmask(SIG_UNBLOCK, &un, NULL);
    }
    if (!cmd) return bar_main(); /* 菜单栏应用, bar_main 自行打开 SMC */
    if (geteuid() == 0 && getuid() != 0) {
        /* setuid 助手上下文 (/usr/local/bin/fansctl-root): 只放行固定风扇动作,
           严防被用作通用提权入口 */
        int ok = strcmp(cmd, "__apply") == 0 || strcmp(cmd, "__smart") == 0 ||
                 strcmp(cmd, "__hold") == 0 ||
                 (strcmp(cmd, "smart") == 0 && argc >= 3 && strcmp(argv[2], "stop") == 0);
        if (!ok) { fprintf(stderr, "%s", L("setuid 助手只允许风扇操作\n", "setuid helper allows fan operations only\n")); return 1; }
    }
    if (strcmp(cmd, "__apply") == 0) return apply_cmd(argc, argv);
    if (strcmp(cmd, "__smart") == 0) return smart_hidden_cmd(argc, argv);
    if (strcmp(cmd, "__hold") == 0) return hold_hidden_cmd(argc, argv);
    if (strcmp(cmd, "__ask") == 0) return ask_main(argc, argv);
    g_debug = getenv("FANSCTL_DEBUG") != NULL;
    if (strcmp(cmd, "bar") == 0) return bar_cmd(argc, argv);       /* 不碰 SMC */
    if (strcmp(cmd, "doctor") == 0) return doctor_cmd();           /* SMC 可选, 自行容错 */
    if (strcmp(cmd, "__selftest") == 0) return selftest_cmd();     /* 纯逻辑断言, 无需 SMC */


    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--json") == 0) g_json = 1;
    if (g_json && strcmp(cmd, "fans") != 0 && strcmp(cmd, "status") != 0 &&
        strcmp(cmd, "temps") != 0 && strcmp(cmd, "power") != 0 && strcmp(cmd, "watch") != 0) {
        fprintf(stderr, "%s", L("--json 目前支持 fans/status/temps/power/watch\n",
                                "--json is currently supported for fans/status/temps/power/watch\n"));
        return 1;
    }

    /* 未知命令直接给用法再退出 — 不开 SMC (无 AppleSMC 的环境也可见帮助) */
    {
        static const char *const known[] = {"fans", "status", "temps", "power", "dump",
                                            "watch", "set", "max", "auto", "smart", "hold", NULL};
        int is_known = 0;
        for (int i = 0; known[i]; i++)
            if (strcmp(cmd, known[i]) == 0) { is_known = 1; break; }
        if (!is_known) { print_usage(); return 1; }
    }
    if (smc_open() != 0) return 1;
    int rc = 0;

    if (strcmp(cmd, "fans") == 0) {
        if (g_json) json_fans(); else print_fans();
    } else if (strcmp(cmd, "status") == 0) {
        if (g_json) json_status(); else print_status();
    } else if (strcmp(cmd, "temps") == 0) {
        if (g_json) json_temps(); else {
            int tsort = 0;
            double tabove = -100;
            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "--sort") == 0) tsort = 1;
                else if (strcmp(argv[i], "--above") == 0 && i + 1 < argc)
                    tabove = atof(argv[++i]);
            }
            print_temps(tsort, tabove);
        }
    } else if (strcmp(cmd, "power") == 0) {
        int widx = -1;
        for (int i = 2; i < argc; i++)
            if (strcmp(argv[i], "--watch") == 0) { widx = i; break; }
        if (widx >= 0) {
            int interval = 5;
            if (widx + 1 < argc && argv[widx + 1][0] != '-') interval = atoi(argv[widx + 1]);
            if (interval < 1) interval = 1;
            power_watch_loop(interval);
        } else if (g_json) json_power(); else power_cmd();
    } else if (strcmp(cmd, "dump") == 0) {
        print_dump();
    } else if (strcmp(cmd, "watch") == 0) {
        int interval = 2;
        for (int i = 2; i < argc; i++)
            if (argv[i][0] != '-') { interval = atoi(argv[i]); break; }
        if (interval < 1) interval = 1;
        watch_loop(interval);
    } else if (strcmp(cmd, "set") == 0) {
        if (geteuid() != 0) {
            fprintf(stderr, "%s", L("需要 root 权限:  sudo fansctl set <rpm> [风扇号]\n",
                                    "root required:  sudo fansctl set <rpm> [fan index]\n"));
            rc = 1;
        } else if (argc < 3) {
            fprintf(stderr, "%s", L("用法: fansctl set <rpm> [风扇号]   例: fansctl set 3000\n",
                                    "usage: fansctl set <rpm> [fan index]   e.g. fansctl set 3000\n"));
            rc = 1;
        } else {
            double rpm = atof(argv[2]);
            int have_idx = argc > 3;
            int idx = have_idx ? atoi(argv[3]) : 0;
            int n = fan_count();
            if (rpm <= 0) { fprintf(stderr, L("无效转速: %s\n", "invalid RPM: %s\n"), argv[2]); rc = 1; }
            else if (have_idx && n && (idx < 0 || idx >= n)) { fprintf(stderr, L("风扇号 %d 超出范围 (0~%d)\n", "fan index %d out of range (0~%d)\n"), idx, n - 1); rc = 1; }
            else {
                int cnt = have_idx ? 1 : (n ? n : 1);
                for (int i = 0; i < cnt; i++)
                    if (fan_set(have_idx ? idx : i, rpm) != 0) rc = 1;
            }
            if (rc == 0) {
                usleep(300 * 1000); /* 等 SMC 状态刷新, 否则读到旧值 */
                printf("%s", L("已设置:\n", "Applied:\n"));
                print_status();
            }
        }
    } else if (strcmp(cmd, "max") == 0 || strcmp(cmd, "auto") == 0) {
        int to_max = cmd[0] == 'm';
        if (geteuid() != 0) {
            fprintf(stderr, L("需要 root 权限:  sudo fansctl %s [风扇号]\n",
                              "root required:  sudo fansctl %s [fan index]\n"), cmd);
            rc = 1;
        } else {
            int have_idx = argc > 2;
            int idx = have_idx ? atoi(argv[2]) : 0;
            int n = fan_count();
            if (have_idx && n && (idx < 0 || idx >= n)) { fprintf(stderr, L("风扇号 %d 超出范围 (0~%d)\n", "fan index %d out of range (0~%d)\n"), idx, n - 1); rc = 1; }
            else {
                int cnt = have_idx ? 1 : (n ? n : 1);
                for (int i = 0; i < cnt; i++) {
                    int fi = have_idx ? idx : i;
                    if (to_max ? fan_max(fi) != 0 : fan_auto(fi) != 0) rc = 1;
                }
            }
            if (rc == 0) {
                usleep(300 * 1000); /* 等 SMC 状态刷新, 否则读到旧值 */
                printf(L("%s:\n", "%s:\n"), to_max ? L("已设全速", "Max speed set") : L("已恢复自动", "Auto restored"));
                print_status();
            }
        }
    } else if (strcmp(cmd, "smart") == 0) {
        if (geteuid() != 0) {
            fprintf(stderr, "%s", L("需要 root 权限:  sudo fansctl smart [低温°C] [高温°C] | stop\n",
                                    "root required:  sudo fansctl smart [lo°C] [hi°C] | stop\n"));
            rc = 1;
        } else if (argc > 2 && strcmp(argv[2], "stop") == 0) {
            rc = smart_stop();
        } else {
            double t_lo = argc > 2 ? atof(argv[2]) : 40;
            double t_hi = argc > 3 ? atof(argv[3]) : 80;
            int pid; double a, b;
            if (t_lo < 20 || t_hi < t_lo + 5 || t_hi > 120) {
                fprintf(stderr, "%s", L("阈值无效: 需 20 < 低温 且 高温 >= 低温+5 且 高温 <= 120\n",
                                        "invalid thresholds: need 20 < low, high >= low+5, high <= 120\n"));
                rc = 1;
            } else if (smart_pid_read(&pid, &a, &b)) {
                fprintf(stderr, L("智能模式已在运行 (pid %d, %.0f~%.0f°C), 先 sudo fansctl smart stop\n",
                                  "smart mode already running (pid %d, %.0f~%.0f°C), run sudo fansctl smart stop first\n"),
                        pid, a, b);
                rc = 1;
            } else {
                int hpid; double hx;
                if (hold_pid_read(&hpid, &hx)) hold_stop(); /* 互斥: 先收恒温的控制权 */
                smart_pid_write(t_lo, t_hi);
                rc = smart_loop(t_lo, t_hi);
                smart_pid_clear();
            }
        }
    } else if (strcmp(cmd, "hold") == 0) {
        if (geteuid() != 0) {
            fprintf(stderr, "%s", L("需要 root 权限:  sudo fansctl hold <目标°C> | stop\n",
                                    "root required:  sudo fansctl hold <target°C> | stop\n"));
            rc = 1;
        } else if (argc > 2 && strcmp(argv[2], "stop") == 0) {
            rc = hold_stop();
        } else {
            double t_set = argc > 2 ? atof(argv[2]) : 70;
            int hpid; double hx;
            if (t_set < 30 || t_set > 110) {
                fprintf(stderr, "%s", L("目标无效: 需 30~110°C\n", "invalid target: need 30~110°C\n"));
                rc = 1;
            } else if (hold_pid_read(&hpid, &hx)) {
                fprintf(stderr, L("恒温模式已在运行 (pid %d, →%.1f°C), 先 sudo fansctl hold stop\n",
                                  "thermostat already running (pid %d, →%.1f°C), run sudo fansctl hold stop first\n"),
                        hpid, hx);
                rc = 1;
            } else {
                int spid; double slo, shi;
                if (smart_pid_read(&spid, &slo, &shi)) smart_stop(); /* 互斥 */
                hold_pid_write(t_set);
                rc = hold_loop(t_set);
                hold_pid_clear();
            }
        }
    }
    smc_close();
    return rc;
}
