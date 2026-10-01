/*
 * fansctl - macOS SMC 风扇/温度工具 (Apple Silicon 兼容)
 * 一个程序两种形态: 无参数 = 菜单栏应用(fork 后台); 带参数 = CLI
 * SMC 读取核心在 fansctl.h, 菜单栏界面在 fansbar.m
 * 完整子命令列表见 main() 的用法输出 (fans/status/temps/dump/watch 只读;
 * set/max/auto/smart 需 root)
 * 许可证: AGPL-3.0-or-later (见 LICENSE), 商用需开源衍生代码
 */
#include <mach/mach_time.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <sys/sysctl.h>
#include <sys/proc_info.h>
#include <libproc.h>
#include "fansctl.h"

/* ==================== 写入/控制 (需要 root) ==================== */

static void print_fans(void);

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
        fprintf(stderr, "开启手动模式失败: 权限不足, 请用 sudo 运行\n");
        return -1;
    }
    if (!key_exists("Ftst")) {
        fprintf(stderr, "开启手动模式失败: kr=0x%x (无 Ftst 解锁键可用)\n", r);
        return -1;
    }
    UInt8 one = 1;
    if (smc_write_key("Ftst", &one, 1) != KERN_SUCCESS) {
        fprintf(stderr, "写入 Ftst 解锁失败\n");
        return -1;
    }
    ftst_held = 1;
    fprintf(stderr, "已向温控管理器申请解锁(Ftst=1)，等待 3 秒...\n");
    sleep(3);
    for (int attempt = 0; attempt < 300; attempt++) {
        if (write_mode_bit(idx, 1) == KERN_SUCCESS) return 0;
        usleep(100 * 1000);
    }
    fprintf(stderr, "解锁后仍无法开启手动模式\n");
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
        fprintf(stderr, "未找到 %s 键\n", k);
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
        fprintf(stderr, "不支持的目标键类型: %s\n", ts);
        return -1;
    }
    /* 写入退避: SMC 忙(0x82 温控器占用)时指数退避重试, 总计约 1.5 秒, 不硬敲 */
    for (int attempt = 0, ms = 50; attempt < 6; attempt++, ms *= 2) {
        if (smc_write_key(k, data, dlen) == KERN_SUCCESS) return 0;
        if (attempt < 5) usleep(ms * 1000);
    }
    fprintf(stderr, "写入 %s 失败 (SMC 可能拒绝: 0x82=温控器占用, 0x86=键只读)\n", k);
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
        fprintf(stderr, "转速限制到 %.0f rpm (安全范围 %.0f~%.0f)\n", clamped, mn, mx);
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
        fprintf(stderr, "恢复自动失败: kr=0x%x%s\n", r,
                r == KR_NOT_PRIVILEGED ? " (需要 sudo)" : "");
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
    if (!n) { printf("FNum 未找到, 尝试枚举...\n"); print_fans(); return; }
    for (int i = 0; i < n; i++) {
        double ac = -1, tg = -1, mn = -1, mx = -1;
        read_rpm_key(i, "Ac", &ac);
        read_rpm_key(i, "Tg", &tg);
        read_rpm_key(i, "Mn", &mn);
        read_rpm_key(i, "Mx", &mx);
        int mode = fan_mode(i);
        printf("风扇%d  当前 %7.0f  目标 %7.0f  [%.0f~%.0f]  模式 %s\n",
               i, ac, tg, mn, mx, mode == 1 ? "手动" : mode == 0 ? "自动" : "未知");
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
            size_t L = idlen < sizeof(id) - 1 ? idlen : sizeof(id) - 1;
            memcpy(id, idbuf, L); id[L] = 0;
        }
        if (id[0])
            printf("风扇%c  %-16s 当前 %7.0f rpm   [下限 %7.0f / 上限 %7.0f]\n", c, id, v, mn, mx);
        else
            printf("风扇%c  当前 %7.0f rpm   [下限 %7.0f / 上限 %7.0f]\n", c, v, mn, mx);
        n++;
    }
    if (!n) printf("未发现风扇键(F*Ac)\n");
}

static void print_temps(void) {
    UInt32 n = total_keys();
    if (!n) { fprintf(stderr, "无法获取键总数\n"); return; }
    int found = 0;
    for (UInt32 i = 0; i < n; i++) {
        char k[5];
        if (get_key_at(i, k) != 0) continue;
        if (k[0] != 'T') continue;
        double v;
        if (read_key_value(k, NULL, &v, NULL) == 0 && plausible_temp(v)) {
            printf("%-6s %9.2f °C\n", k, v);
            found++;
        }
    }
    if (!found) printf("未发现温度传感器\n");
}

static void print_dump(void) {
    UInt32 n = total_keys();
    if (!n) { fprintf(stderr, "无法获取键总数\n"); return; }
    printf("SMC 键总数: %u\n\n", n);
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
    if (mx > -999) printf(" 最热 %s %.1f°C", mxk ? mxk : "?", mx);
    printf("\n");
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
    printf("智能模式: <=%.0f°C 自动 (基线跟随系统), >=%.0f°C 全速, 中间线性插值; Ctrl+C 退出并恢复自动\n",
           t_lo, t_hi);
    mach_timebase_info(&g_timebase);
    uint64_t tick_ns = cont_ns();
    int rc = 0;
    while (!g_stop) {
        if (g_reload) { /* pidfile 里的新阈值就地生效, 不打断风扇控制 */
            g_reload = 0;
            int p; double nl = 0, nh = 0;
            if (smart_pid_read(&p, &nl, &nh) && p == (int)getpid() &&
                nl >= 20 && nh >= nl + 5 && nh <= 120) {
                t_lo = nl;
                t_hi = nh;
                printf("[阈值更新为 %.0f~%.0f°C]\n", t_lo, t_hi);
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
            if (T < -999) { fprintf(stderr, "温度读取失败(已重试)\n"); rc = 1; break; }
        }
        double frac = (T - t_lo) / (t_hi - t_lo);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
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
                    printf("[风扇%d 被外部改写(模式=%d 目标=%.0f 期望=%.0f), 重新接管]\n",
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
            if (w < mn) w = mn;
            if (w > mx) w = mx;
            want[f] = w;
            if (last[f] < 0 || fabs(w - last[f]) > 10) {
                if (write_tg(f, w) != 0) { rc = 1; break; }
                last[f] = w;
            }
        }
        if (rc != 0) break;
        printf("[%.0f~%.0f°C] %s %.1f°C 曲线%.0f%% |", t_lo, t_hi, tkey ? tkey : "?", T, frac * 100);
        for (int f = 0; f < n; f++) {
            double ac = -1;
            read_rpm_key(f, "Ac", &ac);
            if (manual[f] && want[f] > 0 && base[f] > 0)
                printf(" 风扇%d %6.0f rpm (+%.0f%%) 手动", f, ac, (want[f] - base[f]) / base[f] * 100);
            else
                printf(" 风扇%d %6.0f rpm 自动", f, ac);
        }
        printf("\n");
        fflush(stdout);
        sleep(1);
        /* 唤醒让权: 1 秒拍长被拉长到 5 秒以上 = 系统刚从睡眠唤醒。
           先交还系统控制并静置 5 秒(温控器/传感器稳定), 之后按新基线重新接管 */
        uint64_t dt = cont_ns() - tick_ns;
        tick_ns = cont_ns();
        if (dt > 5ULL * 1000000000ULL) {
            printf("[间隔 %.0f 秒, 判定为系统唤醒, 暂交系统控制 5 秒]\n", dt / 1e9);
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
        printf("\n已退出智能模式, 全部恢复自动");
        if (g_stop_pid > 0) printf(" (停止信号来自 pid %d)", (int)g_stop_pid);
        printf("\n");
    }
    return rc;
}

static void watch_loop(int interval) {
    for (;;) {
        print_status_line();
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

static void print_usb_power(void) {
    io_iterator_t it = MACH_PORT_NULL;
    printf("USB 设备 (声明的 5V 电流需求, 非实测):\n");
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching("IOUSBHostDevice"), &it) != KERN_SUCCESS) {
        printf("  枚举失败\n");
        return;
    }
    io_object_t dev;
    int n = 0;
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
            if (ma > 0)
                printf("  - %s (%s)  %s  %d mA ≈ %.1f W%s\n", name, vendor, sp, ma,
                       ma * 5.0 / 1000.0, selfp ? " [自供电]" : "");
            else
                printf("  - %s (%s)  %s  电流未知\n", name, vendor, sp);
            n++;
        }
        CFRelease(props);
        IOObjectRelease(dev);
    }
    IOObjectRelease(it);
    if (!n) printf("  (无 USB 设备)\n");
}

/* ---- 进程功耗排行: 两次采样当前 CPU% (0.4s 窗口) + 常驻内存, 降序 ----
   每进程 GPU 占用无公开接口 (活动监视器也不分), 笔记本上 CPU 即功耗主导,
   故按 CPU 排序 */
struct proc_sample { int pid; uint64_t t_ns, rss; char name[64]; };

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
        snprintf(list[n].name, sizeof list[n].name, "%s", nm);
        n++;
    }
    free(kp);
    *out = list;
    *denied_out = denied;
    *ndenied = nd;
    return n;
}

struct top_row { double cpu, mempct; uint64_t rss; int pid; char name[64]; };

static int row_cmp(const void *x, const void *y) {
    double d = ((const struct top_row *)y)->cpu - ((const struct top_row *)x)->cpu;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static int denied_cmp(const void *x, const void *y) {
    uint64_t a = ((const struct proc_sample *)x)->rss;
    uint64_t b = ((const struct proc_sample *)y)->rss;
    return a < b ? 1 : a > b ? -1 : 0;
}

static void print_top_procs(void) {
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
    printf("功耗 Top 进程 (按当前 CPU%% 降序, 0.4 秒采样; 每进程 GPU 无公开数据):\n");
    if (na <= 0 || nb <= 0 || elapsed_ns <= 0) {
        printf("  枚举失败\n");
        free(a); free(b); free(da); free(db);
        return;
    }
    uint64_t memsize = 0;
    size_t ml = sizeof memsize;
    sysctl((int[2]){CTL_HW, HW_MEMSIZE}, 2, &memsize, &ml, NULL, 0);
    struct top_row *rows = calloc((size_t)nb, sizeof *rows);
    if (!rows) { printf("  内存不足\n"); free(a); free(b); free(da); free(db); return; }
    int n = 0;
    for (int j = 0; j < nb; j++)
        for (int i = 0; i < na; i++)
            if (a[i].pid == b[j].pid) { /* 两次采样间新出现的进程无基准, 跳过 */
                /* 线程中途退出会使总时间回落 (无符号差会回绕成天文数字), 钳为 0 */
                uint64_t d = b[j].t_ns > a[i].t_ns ? b[j].t_ns - a[i].t_ns : 0;
                rows[n].cpu = (double)d * 100.0 / elapsed_ns;
                rows[n].rss = b[j].rss;
                rows[n].pid = b[j].pid;
                rows[n].mempct = memsize ? (double)b[j].rss * 100.0 / (double)memsize : 0;
                snprintf(rows[n].name, sizeof rows[n].name, "%s", b[j].name);
                n++;
                break;
            }
    qsort(rows, (size_t)n, sizeof *rows, row_cmp);
    printf("  %6s %6s %9s %5s  %s\n", "PID", "CPU", "内存", "%MEM", "进程");
    int shown = 0;
    for (int i = 0; i < n && shown < 10 && rows[i].cpu >= 0.1; i++) {
        double mb = rows[i].rss / 1048576.0;
        if (mb >= 1024)
            printf("  %6d %5.1f%% %8.2fG %5.1f  %s\n",
                   rows[i].pid, rows[i].cpu, mb / 1024, rows[i].mempct, rows[i].name);
        else
            printf("  %6d %5.1f%% %8.0fM %5.1f  %s\n",
                   rows[i].pid, rows[i].cpu, mb, rows[i].mempct, rows[i].name);
        shown++;
    }
    if (!shown) printf("  (全部空闲)\n");
    /* 无权限读占用的系统/其他用户进程: 列几个名字, 提示 root 可见全部 */
    if (ndb > 0) {
        qsort(db, (size_t)ndb, sizeof *db, denied_cmp);
        printf("  另有 %d 个系统/其他用户进程无权限读取占用 (sudo fansctl power 可见), 如:", ndb);
        for (int i = 0; i < ndb && i < 3; i++)
            printf("%s%s", i ? "," : " ", db[i].name);
        printf("\n");
    }
    free(rows); free(a); free(b); free(da); free(db);
}

static int power_cmd(void) {
    struct power_info p;
    int has_batt = power_read(&p) == 0;
    /* 机器功率: PSTR 秒级实时, 与菜单栏第一行同源; 充电时拆出充电分量 */
    double pw = -1;
    read_key_value("PSTR", NULL, &pw, NULL);
    if (pw > 0 && pw < 1000) {
        if (p.charge_w > 0 && pw > p.charge_w)
            printf("机器功率: %.0f W (PSTR) = 系统 %.0f W + 充电 %.0f W\n",
                   pw, pw - p.charge_w, p.charge_w);
        else
            printf("机器功率: %.0f W (PSTR, 秒级实时)\n", pw);
    } else {
        printf("机器功率: 未知 (本机无 PSTR 键)\n");
    }
    if (p.sys_v > 0 && p.sys_i > 0) {
        double w = p.sys_w > 0 ? p.sys_w : p.sys_v * p.sys_i;
        printf("电源输入: %.1f V × %.2f A = %.1f W (实测, 遥测约分钟级刷新)\n",
               p.sys_v, p.sys_i, w);
    } else if (p.ext) {
        printf("电源输入: 已连接 (无实时遥测)\n");
    } else {
        printf("电源输入: 未连接 (电池供电)\n");
    }
    if (p.adapter_w > 0) {
        char av[32] = "";
        if (p.adapter_v > 0) snprintf(av, sizeof av, ", 协商 %d V", p.adapter_v);
        printf("适配器: %d W 额定%s\n", p.adapter_w, av);
    } else if (p.ext) {
        printf("适配器: 已连接 (额定功率未知)\n");
    }
    if (has_batt) {
        if (p.charging && p.charge_w > 0)
            printf("电池: 充电中 %.2f V / %.0f mA (%.1f W)\n", p.batt_v, p.batt_a, p.charge_w);
        else if (p.batt_a < -50)
            printf("电池: 放电 %.2f V / %.0f mA (%.1f W)\n",
                   p.batt_v, -p.batt_a, -p.batt_a * p.batt_v / 1000.0);
        else
            printf("电池: %.2f V / %.0f mA (未充放)\n", p.batt_v, p.batt_a);
    } else {
        printf("电池: 本机无电池\n");
    }
    print_usb_power();
    print_top_procs();
    return 0;
}

/* ============ 菜单栏的 root 侧入口 (经授权弹窗重新执行自身) ============ */

/* 结束运行中的智能模式: SIGTERM 优雅退出(其信号处理器恢复自动并清 pidfile)。
   CLI "smart stop" 与隐藏 "__smart stop" 共用 */
static int smart_stop(void) {
    int pid; double lo, hi;
    if (!smart_pid_read(&pid, &lo, &hi)) { fprintf(stderr, "智能模式未在运行\n"); return 1; }
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
            fprintf(stderr, "智能模式 10 秒内未完成退出, 请查 /tmp/fansctl.smart.log\n");
            return 1;
        }
    }
    printf("智能模式已停止(恢复自动)\n");
    return 0;
}

/* ==================== 恒温模式 (需 root, PI 闭环, Ctrl+C 退出并恢复自动) ==================== */

/* 与 smart_stop 同款"等收尾"语义 */
static int hold_stop(void) {
    int pid; double t;
    if (!hold_pid_read(&pid, &t)) { fprintf(stderr, "恒温模式未在运行\n"); return 1; }
    if (kill(pid, SIGTERM) != 0) { perror("kill"); return 1; }
    for (int i = 0; i < 100; i++) {
        int p; double x;
        if (!hold_pid_read(&p, &x) || p != pid) break;
        usleep(100 * 1000);
    }
    {
        int p; double x;
        if (hold_pid_read(&p, &x) && p == pid) {
            fprintf(stderr, "恒温模式 10 秒内未完成退出, 请查 /tmp/fansctl.hold.log\n");
            return 1;
        }
    }
    printf("恒温模式已停止(恢复自动)\n");
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
    printf("恒温模式: 目标最热传感器 %.1f°C, PI 闭环控制; Ctrl+C 退出并恢复自动\n", t_set);
    mach_timebase_info(&g_timebase);
    uint64_t tick_ns = cont_ns();
    double e_prev = 0, t_prev = -999;
    int rc = 0;
    while (!g_stop) {
        if (g_reload) { /* pidfile 里的新目标就地生效, 不打断控制 */
            g_reload = 0;
            int p; double nt = 0;
            if (hold_pid_read(&p, &nt) && p == (int)getpid() && nt >= 30 && nt <= 110) {
                t_set = nt;
                printf("[目标更新为 %.1f°C]\n", t_set);
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
            if (T < -999) { fprintf(stderr, "温度读取失败(已重试)\n"); rc = 1; break; }
        }
        double Ts = t_prev > -999 ? t_prev + 0.3 * (T - t_prev) : T; /* EMA 平滑 */
        double e = Ts - t_set;
        double dT = t_prev > -999 ? Ts - t_prev : 0;
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
                    printf("[风扇%d 被外部改写(模式=%d 目标=%.0f 期望=%.0f), 重新接管]\n",
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
                if (write_tg(f, rpm[f]) != 0) { rc = 1; break; }
                last[f] = rpm[f];
            }
        }
        if (rc != 0) break;
        printf("[→%.1f°C] %s %.1f°C(平%.1f) 误差%+.1f 趋势%+.2f |",
               t_set, tkey ? tkey : "?", T, Ts, e, dT);
        for (int f = 0; f < n; f++) {
            double ac = -1;
            read_rpm_key(f, "Ac", &ac);
            if (manual[f]) printf(" 风扇%d %6.0f→%.0f 手动", f, ac, rpm[f]);
            else           printf(" 风扇%d %6.0f rpm 自动", f, ac);
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
            printf("[间隔 %.0f 秒, 判定为系统唤醒, 暂交系统控制 5 秒]\n", dt / 1e9);
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
        printf("\n已退出恒温模式, 全部恢复自动");
        if (g_stop_pid > 0) printf(" (停止信号来自 pid %d)", (int)g_stop_pid);
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
        if (argc < 4) { fprintf(stderr, "用法: __hold target <°C>\n"); return 1; }
        double nt = atof(argv[3]);
        if (nt < 30 || nt > 110) { fprintf(stderr, "目标温度无效\n"); return 1; }
        int pid; double t;
        if (!hold_pid_read(&pid, &t)) { fprintf(stderr, "恒温模式未在运行\n"); return 1; }
        FILE *f = fopen(HOLD_PIDFILE, "w");
        if (!f) { perror("pidfile"); return 1; }
        fprintf(f, "%d %.1f\n", pid, nt);
        fclose(f);
        if (kill(pid, SIGUSR1) != 0) { perror("kill"); return 1; }
        printf("已通知恒温模式更新目标 %.1f°C\n", nt);
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
        if (argc < 5) { fprintf(stderr, "用法: __smart thresh <低°C> <高°C>\n"); return 1; }
        double nl = atof(argv[3]), nh = atof(argv[4]);
        if (nl < 20 || nh < nl + 5 || nh > 120) { fprintf(stderr, "阈值无效\n"); return 1; }
        int pid; double lo, hi;
        if (!smart_pid_read(&pid, &lo, &hi)) { fprintf(stderr, "智能模式未在运行\n"); return 1; }
        FILE *f = fopen(SMART_PIDFILE, "w");
        if (!f) { perror("pidfile"); return 1; }
        fprintf(f, "%d %.0f %.0f\n", pid, nl, nh);
        fclose(f);
        if (kill(pid, SIGUSR1) != 0) { perror("kill"); return 1; }
        printf("已通知智能模式更新阈值 %.0f~%.0f°C\n", nl, nh);
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

int main(int argc, char **argv) {
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
        if (!ok) { fprintf(stderr, "setuid 助手只允许风扇操作\n"); return 1; }
    }
    if (strcmp(cmd, "__apply") == 0) return apply_cmd(argc, argv);
    if (strcmp(cmd, "__smart") == 0) return smart_hidden_cmd(argc, argv);
    if (strcmp(cmd, "__hold") == 0) return hold_hidden_cmd(argc, argv);
    if (strcmp(cmd, "__ask") == 0) return ask_main(argc, argv);
    g_debug = getenv("FANSCTL_DEBUG") != NULL;
    if (smc_open() != 0) return 1;
    int rc = 0;

    if (strcmp(cmd, "fans") == 0) {
        print_fans();
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strcmp(cmd, "temps") == 0) {
        print_temps();
    } else if (strcmp(cmd, "power") == 0) {
        power_cmd();
    } else if (strcmp(cmd, "dump") == 0) {
        print_dump();
    } else if (strcmp(cmd, "watch") == 0) {
        int interval = argc > 2 ? atoi(argv[2]) : 2;
        if (interval < 1) interval = 1;
        watch_loop(interval);
    } else if (strcmp(cmd, "set") == 0) {
        if (geteuid() != 0) {
            fprintf(stderr, "需要 root 权限:  sudo fansctl set <rpm> [风扇号]\n");
            rc = 1;
        } else if (argc < 3) {
            fprintf(stderr, "用法: fansctl set <rpm> [风扇号]   例: fansctl set 3000\n");
            rc = 1;
        } else {
            double rpm = atof(argv[2]);
            int have_idx = argc > 3;
            int idx = have_idx ? atoi(argv[3]) : 0;
            int n = fan_count();
            if (rpm <= 0) { fprintf(stderr, "无效转速: %s\n", argv[2]); rc = 1; }
            else if (have_idx && n && (idx < 0 || idx >= n)) { fprintf(stderr, "风扇号 %d 超出范围 (0~%d)\n", idx, n - 1); rc = 1; }
            else {
                int cnt = have_idx ? 1 : (n ? n : 1);
                for (int i = 0; i < cnt; i++)
                    if (fan_set(have_idx ? idx : i, rpm) != 0) rc = 1;
            }
            if (rc == 0) {
                usleep(300 * 1000); /* 等 SMC 状态刷新, 否则读到旧值 */
                printf("已设置:\n");
                print_status();
            }
        }
    } else if (strcmp(cmd, "max") == 0 || strcmp(cmd, "auto") == 0) {
        int to_max = cmd[0] == 'm';
        if (geteuid() != 0) {
            fprintf(stderr, "需要 root 权限:  sudo fansctl %s [风扇号]\n", cmd);
            rc = 1;
        } else {
            int have_idx = argc > 2;
            int idx = have_idx ? atoi(argv[2]) : 0;
            int n = fan_count();
            if (have_idx && n && (idx < 0 || idx >= n)) { fprintf(stderr, "风扇号 %d 超出范围 (0~%d)\n", idx, n - 1); rc = 1; }
            else {
                int cnt = have_idx ? 1 : (n ? n : 1);
                for (int i = 0; i < cnt; i++) {
                    int fi = have_idx ? idx : i;
                    if (to_max ? fan_max(fi) != 0 : fan_auto(fi) != 0) rc = 1;
                }
            }
            if (rc == 0) {
                usleep(300 * 1000); /* 等 SMC 状态刷新, 否则读到旧值 */
                printf("%s:\n", to_max ? "已设全速" : "已恢复自动");
                print_status();
            }
        }
    } else if (strcmp(cmd, "smart") == 0) {
        if (geteuid() != 0) {
            fprintf(stderr, "需要 root 权限:  sudo fansctl smart [低温°C] [高温°C] | stop\n");
            rc = 1;
        } else if (argc > 2 && strcmp(argv[2], "stop") == 0) {
            rc = smart_stop();
        } else {
            double t_lo = argc > 2 ? atof(argv[2]) : 40;
            double t_hi = argc > 3 ? atof(argv[3]) : 80;
            int pid; double a, b;
            if (t_lo < 20 || t_hi < t_lo + 5 || t_hi > 120) {
                fprintf(stderr, "阈值无效: 需 20 < 低温 且 高温 >= 低温+5 且 高温 <= 120\n");
                rc = 1;
            } else if (smart_pid_read(&pid, &a, &b)) {
                fprintf(stderr, "智能模式已在运行 (pid %d, %.0f~%.0f°C), 先 sudo fansctl smart stop\n",
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
            fprintf(stderr, "需要 root 权限:  sudo fansctl hold <目标°C> | stop\n");
            rc = 1;
        } else if (argc > 2 && strcmp(argv[2], "stop") == 0) {
            rc = hold_stop();
        } else {
            double t_set = argc > 2 ? atof(argv[2]) : 70;
            int hpid; double hx;
            if (t_set < 30 || t_set > 110) {
                fprintf(stderr, "目标无效: 需 30~110°C\n");
                rc = 1;
            } else if (hold_pid_read(&hpid, &hx)) {
                fprintf(stderr, "恒温模式已在运行 (pid %d, →%.1f°C), 先 sudo fansctl hold stop\n",
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
    } else {
        fprintf(stderr,
            "用法: fansctl            启动菜单栏应用(fork 后台, 不占终端)\n"
            "      fansctl <子命令>   命令行模式\n"
            "  fans            风扇转速(只读)\n"
            "  status          风扇当前/目标转速与模式\n"
            "  temps           所有温度传感器\n"
            "  power           供电/功率一览: 机器功率, 电源输入, 适配器, USB 设备, 功耗Top进程\n"
            "  dump            导出全部 SMC 键\n"
            "  watch [秒]      循环刷新\n"
            "  set <rpm> [N]   设定转速, 不带 N 作用于全部风扇 (需 sudo)\n"
            "  max [N]         全速, 不带 N 作用于全部风扇 (需 sudo)\n"
            "  auto [N]        恢复自动, 不带 N 作用于全部风扇 (需 sudo)\n"
            "  smart [低 高]   智能曲线, 默认 40~80°C (需 sudo)\n"
            "  smart stop      结束智能模式(含菜单栏启动的), 恢复自动 (需 sudo)\n"
            "  hold <°C>       恒温模式, PI 闭环把最热传感器稳定在目标温度,\n"
            "                  默认 70°C; 与智能模式互斥 (需 sudo)\n"
            "  hold stop       结束恒温模式(含菜单栏启动的), 恢复自动 (需 sudo)\n"
            "  version         版本号\n");
        smc_close();
        return 1;
    }
    smc_close();
    return rc;
}
