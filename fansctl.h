/* fansctl.h - SMC 读取核心, 由 fansctl (CLI) 和 fansbar (菜单栏) 共享 */
#ifndef FANSCTL_H
#define FANSCTL_H

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FANSCTL_VERSION "1.8.1"

#define KERNEL_INDEX_SMC     2
#define SMC_CMD_READ_BYTES   5
#define SMC_CMD_WRITE_BYTES  6
#define SMC_CMD_READ_INDEX   8
#define SMC_CMD_READ_KEYINFO 9

typedef struct { UInt8 major, minor, build, reserved; UInt16 release; } SMCVersion;
typedef struct { UInt16 version, length; UInt32 cpuPLimit, gpuPLimit, memPLimit; } SMCPLimitData;
typedef struct { UInt32 dataSize; UInt32 dataType; UInt8 attr; } SMCKeyInfoData;
typedef UInt8 SMCBytes[32];

/* macOS 12+ (Apple Silicon) 布局: 总长 80 字节 */
typedef struct {
    UInt32 key;               /* 0 */
    SMCVersion vers;          /* 4..10 */
    SMCPLimitData pLimitData; /* 12..28 */
    SMCKeyInfoData keyInfo;   /* 28..40 */
    UInt8  result;            /* 40 */
    UInt8  status;            /* 41 */
    UInt8  data8;             /* 42 */
    UInt32 data32;            /* 44 */
    SMCBytes bytes;           /* 48..80 */
} SMCKeyData;

_Static_assert(sizeof(SMCKeyData) == 80, "SMCKeyData 必须是 80 字节");

static io_connect_t g_conn = 0;
static int g_debug = 0;

static inline void key_to_str(UInt32 key, char out[5]) {
    out[0] = (key >> 24) & 0xff; out[1] = (key >> 16) & 0xff;
    out[2] = (key >> 8) & 0xff;  out[3] = key & 0xff; out[4] = 0;
}

static inline UInt32 str_to_key(const char *s) {
    return ((UInt32)(UInt8)s[0] << 24) | ((UInt32)(UInt8)s[1] << 16) |
           ((UInt32)(UInt8)s[2] << 8)  |  (UInt32)(UInt8)s[3];
}

static inline int smc_open(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc) { fprintf(stderr, "未找到 AppleSMC 服务\n"); return -1; }
    kern_return_t r = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    if (r != KERN_SUCCESS) { fprintf(stderr, "IOServiceOpen 失败: 0x%x (试试 sudo)\n", r); return -1; }
    return 0;
}

static inline void smc_close(void) { if (g_conn) IOServiceClose(g_conn); }

static inline kern_return_t smc_call(SMCKeyData *in, SMCKeyData *out) {
    size_t sz = sizeof(SMCKeyData);
    return IOConnectCallStructMethod(g_conn, KERNEL_INDEX_SMC, in, sizeof(SMCKeyData), out, &sz);
}

/* 读键: 先 KEYINFO 拿长度/类型, 再 READ_BYTES 取数据。type_out 可为 NULL */
static inline kern_return_t smc_read_key(const char *keyname, UInt8 *buf, size_t *len, UInt32 *type_out) {
    SMCKeyData in = {0}, out = {0};
    in.key = str_to_key(keyname);
    in.data8 = SMC_CMD_READ_KEYINFO;
    kern_return_t r = smc_call(&in, &out);
    if (g_debug) fprintf(stderr, "[dbg] %s keyinfo: kr=0x%x result=%u size=%u type=%#x\n", keyname, r, out.result, out.keyInfo.dataSize, out.keyInfo.dataType);
    if (r != KERN_SUCCESS) return r;
    if (out.result != 0) return kIOReturnNotFound;
    *len = out.keyInfo.dataSize;
    if (type_out) *type_out = out.keyInfo.dataType;
    in.keyInfo.dataSize = out.keyInfo.dataSize;
    in.keyInfo.dataType = out.keyInfo.dataType;
    in.data8 = SMC_CMD_READ_BYTES;
    r = smc_call(&in, &out);
    if (r != KERN_SUCCESS) return r;
    memcpy(buf, out.bytes, *len);
    return KERN_SUCCESS;
}

static inline void type_str(UInt32 t, char out[5]) {
    out[0] = (t >> 24) & 0xff; out[1] = (t >> 16) & 0xff;
    out[2] = (t >> 8) & 0xff;  out[3] = t & 0xff; out[4] = 0;
}

/* 返回: 0=成功解码并填入 value; 1=类型不支持(rawhex 为原始 hex); -1=读取失败。
   type_out / rawhex 可为 NULL (rawhex 容量 80, 写入按剩余空间封顶, 超长截断) */
static inline int read_key_value(const char *keyname, UInt32 *type_out, double *value, char rawhex[80]) {
    UInt8 buf[32]; size_t len = sizeof(buf);
    UInt32 type = 0;
    if (smc_read_key(keyname, buf, &len, &type) != KERN_SUCCESS) return -1;
    if (type_out) *type_out = type;
    if (rawhex) {
        size_t o = 0;
        rawhex[0] = 0;
        for (size_t i = 0; i < len && o + 4 <= 80; i++)
            o += (size_t)snprintf(rawhex + o, 80 - o, "%02x ", buf[i]);
    }
    char t[5]; type_str(type, t);

    if (strcmp(t, "fpe2") == 0 && len >= 2) { *value = (double)((buf[0] << 8) | buf[1]) / 4.0; return 0; }
    if (strcmp(t, "ui8 ") == 0 && len >= 1) { *value = buf[0]; return 0; }
    if (strcmp(t, "ui16") == 0 && len >= 2) { *value = (buf[0] << 8) | buf[1]; return 0; }
    if (strcmp(t, "ui32") == 0 && len >= 4) { *value = ((UInt32)buf[0]<<24)|((UInt32)buf[1]<<16)|((UInt32)buf[2]<<8)|buf[3]; return 0; }
    if (strcmp(t, "sp78") == 0 && len >= 2) { *value = (double)(int16_t)((buf[0] << 8) | buf[1]) / 256.0; return 0; }
    if (strcmp(t, "flt ") == 0 && len >= 4) { float f; memcpy(&f, buf, 4); *value = f; return 0; }
    return 1;
}

static inline int plausible_temp(double v) { return v > -5 && v < 130; }

/* ==================== 温度传感器键 -> 人类可读名 ====================
   依据: exelban/stats 传感器表 (Apple Silicon Tp/Te/Tg/Tm/TH/Ta 族) + Intel 时代
   SMC 命名惯例 (TS0P 掌托 / TW0P 无线网卡 / TB*T 电池); Tp=性能核 Te=能效核
   跨 M1~M5 一致。TCMz 实测 == max(Tp*)/max(Te*) —— Apple 自己的"SoC 最高温"聚合键
   (热管理与降频看的就是它; 参考 Okle42 的 IOReport/powermetrics 对照 gist), TCM? 族
   标 "SoC max/SoC 最高"; TC?? 标 "SoC 组" 是推断。
   未知键返回 NULL, 调用方回退显示键名。'?' = 通配一个字符, 先精确后族匹配。 */
struct temp_name_tab { const char *pat, *zh, *en; };
static inline const char *temp_key_name(const char *key, int en) {
    static const struct temp_name_tab exact[] = {
        {"TB0T", "电池",     "Battery"},
        {"TB1T", "电池 1",   "Battery 1"},
        {"TB2T", "电池 2",   "Battery 2"},
        {"TW0P", "无线网卡", "AirPort (Wi-Fi)"},
        {"TS0P", "掌托左",   "Palm rest L"},
        {"TS1P", "掌托右",   "Palm rest R"},
        {"TAOL", "环境",     "Ambient"},
        {NULL, NULL, NULL}
    };
    static const struct temp_name_tab fam[] = {
        {"Tp??", "CPU 性能核", "CPU P-core"},
        {"Te??", "CPU 能效核", "CPU E-core"},
        {"Tg??", "GPU 核心",   "GPU core"},
        {"Tm??", "内存",       "Memory"},
        {"TH??", "闪存 NAND",  "NAND flash"},
        {"Th??", "闪存 NAND",  "NAND flash"},
        {"Tz??", "热区",       "Thermal zone"},
        {"TCM?", "SoC 最高",   "SoC max"},
        {"TC??", "SoC 组",     "SoC group"},
        {"TaL?", "气流·左",    "Airflow L"},
        {"TaR?", "气流·右",    "Airflow R"},
        {"Ta??", "气流",       "Airflow"},
        {"TB?T", "电池",       "Battery"},
        {NULL, NULL, NULL}
    };
    for (int t = 0; t < 2; t++) {
        const struct temp_name_tab *tab = t == 0 ? exact : fam;
        for (int i = 0; tab[i].pat; i++) {
            int ok = 1;
            for (int c = 0; tab[i].pat[c]; c++) {
                if (tab[i].pat[c] == '?') { /* 通配要求该位确有字符, 短键在此安全落选 */
                    if (key[c] == 0) { ok = 0; break; }
                    continue;
                }
                if (key[c] != tab[i].pat[c]) { ok = 0; break; }
            }
            if (ok && key[strlen(tab[i].pat)] == 0) return en ? tab[i].en : tab[i].zh;
        }
    }
    return NULL;
}

static inline int fan_count(void) {
    double v = 0;
    if (read_key_value("FNum", NULL, &v, NULL) == 0 && v > 0 && v < 10) return (int)v;
    return 0;
}

static inline int read_rpm_key(int idx, const char *suffix, double *out) {
    char k[5];
    snprintf(k, sizeof(k), "F%d%s", idx, suffix);
    if (read_key_value(k, NULL, out, NULL) == 0 && *out >= 0) return 0;
    return -1;
}

static inline int key_exists(const char *k) {
    SMCKeyData in = {0}, out = {0};
    in.key = str_to_key(k);
    in.data8 = SMC_CMD_READ_KEYINFO;
    return smc_call(&in, &out) == KERN_SUCCESS && out.result == 0;
}

/* F?md / F?Md 探测; 找到返回1并填键名 */
static inline int fan_md_key(int idx, char out[5]) {
    char a[5], b[5];
    snprintf(a, sizeof(a), "F%dmd", idx);
    snprintf(b, sizeof(b), "F%dMd", idx);
    if (key_exists(a)) { snprintf(out, 5, "%s", a); return 1; }
    if (key_exists(b)) { snprintf(out, 5, "%s", b); return 1; }
    out[0] = 0;
    return 0;
}

/* 风扇模式: 0=自动 1=手动 -1=未知 */
static inline int fan_mode(int idx) {
    char md[5];
    double v = 0;
    if (fan_md_key(idx, md)) {
        if (read_key_value(md, NULL, &v, NULL) != 0) return -1;
        return (int)v == 1 ? 1 : 0;
    }
    if (read_key_value("FS! ", NULL, &v, NULL) == 0)
        return (((int)v) >> idx) & 1;
    return -1;
}

/* 温度键缓存: 首次枚举全部键, 之后只重读这些 */
static char (*g_tkeys)[5];
static int g_tkey_n;

static inline UInt32 total_keys(void);
static inline int get_key_at(UInt32 idx, char keyname[5]);

static inline void scan_temp_keys(void) {
    if (g_tkey_n) return;
    UInt32 n = total_keys();
    if (!n) return;
    char (*list)[5] = malloc(n * sizeof(*list));
    if (!list) return;
    int cnt = 0;
    for (UInt32 i = 0; i < n; i++) {
        char k[5];
        if (get_key_at(i, k) != 0) continue;
        if (k[0] != 'T') continue;
        double v;
        if (read_key_value(k, NULL, &v, NULL) == 0 && plausible_temp(v)) {
            memcpy(list[cnt], k, 5);
            cnt++;
        }
    }
    g_tkeys = list;
    g_tkey_n = cnt;
}

static inline UInt32 total_keys(void) {
    double v = 0;
    if (read_key_value("#KEY", NULL, &v, NULL) == 0 && v > 0 && v < 100000) return (UInt32)v;
    UInt8 buf[32]; size_t len = sizeof(buf);
    if (smc_read_key("#KEY", buf, &len, NULL) == KERN_SUCCESS && len >= 4) {
        UInt32 le = buf[0] | buf[1]<<8 | buf[2]<<16 | (UInt32)buf[3]<<24;
        if (le > 0 && le < 100000) return le;
    }
    return 0;
}

static inline int get_key_at(UInt32 idx, char keyname[5]) {
    SMCKeyData in = {0}, out = {0};
    in.data8 = SMC_CMD_READ_INDEX;
    in.data32 = idx;
    kern_return_t r = smc_call(&in, &out);
    if (r != KERN_SUCCESS) return -1;
    key_to_str(out.key, keyname);
    return 0;
}

/* 全部温度键里的最高读数; name_out 可为 NULL。无数据返回 -999 */
static inline double hottest_temp(const char **name_out) {
    scan_temp_keys();
    double mx = -999;
    const char *mxk = NULL;
    for (int i = 0; i < g_tkey_n; i++) {
        double v;
        if (read_key_value(g_tkeys[i], NULL, &v, NULL) == 0 && plausible_temp(v) && v > mx) {
            mx = v;
            mxk = g_tkeys[i];
        }
    }
    if (name_out) *name_out = mxk;
    return mx;
}

/* ============ 智能模式运行状态 (pidfile, CLI 与菜单栏互通) ============ */

#define SMART_PIDFILE "/tmp/fansctl.smart.pid"

/* 内容: "<pid> <低阈值> <高阈值>"。进程已死则顺手清掉残留文件 */
static inline int smart_pid_read(int *pid_out, double *lo_out, double *hi_out) {
    FILE *f = fopen(SMART_PIDFILE, "r");
    if (!f) return 0;
    int pid = 0; double lo = 0, hi = 0;
    int ok = fscanf(f, "%d %lf %lf", &pid, &lo, &hi) == 3;
    fclose(f);
    if (!ok) { unlink(SMART_PIDFILE); return 0; }
    if (kill(pid, 0) != 0 && errno == ESRCH) { unlink(SMART_PIDFILE); return 0; }
    if (pid_out) *pid_out = pid;
    if (lo_out) *lo_out = lo;
    if (hi_out) *hi_out = hi;
    return 1;
}

static inline void smart_pid_write(double lo, double hi) {
    FILE *f = fopen(SMART_PIDFILE, "w");
    if (!f) return;
    fprintf(f, "%d %.0f %.0f\n", (int)getpid(), lo, hi);
    fclose(f);
}

static inline void smart_pid_clear(void) { unlink(SMART_PIDFILE); }

/* ==================== 恒温模式状态 (与智能模式互斥) ==================== */

#define HOLD_PIDFILE "/tmp/fansctl.hold.pid"

/* 内容: "<pid> <目标°C>"。进程已死则顺手清掉残留文件 */
static inline int hold_pid_read(int *pid_out, double *t_out) {
    FILE *f = fopen(HOLD_PIDFILE, "r");
    if (!f) return 0;
    int pid = 0; double t = 0;
    int ok = fscanf(f, "%d %lf", &pid, &t) == 2;
    fclose(f);
    if (!ok) { unlink(HOLD_PIDFILE); return 0; }
    if (kill(pid, 0) != 0 && errno == ESRCH) { unlink(HOLD_PIDFILE); return 0; }
    if (pid_out) *pid_out = pid;
    if (t_out) *t_out = t;
    return 1;
}

static inline void hold_pid_write(double t) {
    FILE *f = fopen(HOLD_PIDFILE, "w");
    if (!f) return;
    fprintf(f, "%d %.1f\n", (int)getpid(), t);
    fclose(f);
}

static inline void hold_pid_clear(void) { unlink(HOLD_PIDFILE); }

/* ==================== 供电遥测 (IOKit AppleSmartBattery, 只读) ==================== */

/* 一次打开 AppleSmartBattery 读全: 输入遥测在嵌套字典 PowerTelemetryData,
   适配器信息在 AdapterDetails, 电池电流在 Amperage/Voltage。
   无电池的机器返回 -1; 缺键/未插适配器时对应字段保持 -1, 调用方按需展示。
   注意: 这组遥测约分钟级才刷新, 比 SMC PSTR (秒级) 慢 */
struct power_info {
    double sys_w;    /* 实际输入功率 W (SystemPowerIn); -1 未知 */
    double sys_v;    /* 输入电压 V (SystemVoltageIn); -1 未知 */
    double sys_i;    /* 输入电流 A (SystemCurrentIn); -1 未知 */
    int adapter_w;   /* 适配器额定 W; -1 未知 */
    int adapter_v;   /* 适配器协商电压 V (AdapterVoltage mV 换算); -1 未知 */
    int ext;         /* 1=外接电源已连接 */
    int charging;    /* 1=电池充电中 */
    double charge_w; /* 电池充电功率 W (Amperage>50mA 才算, 滤满电/读数噪声); 0=未充电 */
    double batt_v;   /* 电池电压 V; -1 未知 */
    double batt_a;   /* 电池电流 mA, 正=充电 负=放电 (未滤); 0 未知 */
};

/* CFNumber -> double, 非 CFNumber 或 NULL 返回 -1 */
static inline double cfnum_to_double(CFNumberRef n) {
    double d = -1;
    if (n && CFGetTypeID(n) == CFNumberGetTypeID())
        CFNumberGetValue(n, kCFNumberDoubleType, &d);
    return d;
}

static inline int power_read(struct power_info *p) {
    memset(p, 0, sizeof *p);
    p->sys_w = p->sys_v = p->sys_i = -1;
    p->adapter_w = p->adapter_v = -1;
    p->batt_v = -1;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("AppleSmartBattery"));
    if (!svc) return -1;
    CFMutableDictionaryRef props = NULL;
    kern_return_t r = IORegistryEntryCreateCFProperties(svc, &props,
                                                        kCFAllocatorDefault, kNilOptions);
    IOObjectRelease(svc);
    if (r != KERN_SUCCESS || !props) return -1;
    CFDictionaryRef tele = CFDictionaryGetValue(props, CFSTR("PowerTelemetryData"));
    if (tele) { /* 单位: mW / mV / mA */
        double w = cfnum_to_double(CFDictionaryGetValue(tele, CFSTR("SystemPowerIn")));
        double v = cfnum_to_double(CFDictionaryGetValue(tele, CFSTR("SystemVoltageIn")));
        double i = cfnum_to_double(CFDictionaryGetValue(tele, CFSTR("SystemCurrentIn")));
        if (w > 0) p->sys_w = w / 1000.0;
        if (v > 0) p->sys_v = v / 1000.0;
        if (i > 0) p->sys_i = i / 1000.0;
    }
    CFDictionaryRef ad = CFDictionaryGetValue(props, CFSTR("AdapterDetails"));
    if (ad) {
        double w = cfnum_to_double(CFDictionaryGetValue(ad, CFSTR("Watts")));
        double v = cfnum_to_double(CFDictionaryGetValue(ad, CFSTR("AdapterVoltage")));
        if (w > 0) p->adapter_w = (int)w;
        if (v > 0) p->adapter_v = (int)(v / 1000.0);
    }
    CFTypeRef b;
    if ((b = CFDictionaryGetValue(props, CFSTR("ExternalConnected"))) && b == kCFBooleanTrue)
        p->ext = 1;
    if ((b = CFDictionaryGetValue(props, CFSTR("IsCharging"))) && b == kCFBooleanTrue)
        p->charging = 1;
    double a = cfnum_to_double(CFDictionaryGetValue(props, CFSTR("Amperage"))); /* mA, 有符号 */
    double v = cfnum_to_double(CFDictionaryGetValue(props, CFSTR("Voltage")));  /* mV */
    if (a > -1) p->batt_a = a;
    if (v > 0) p->batt_v = v / 1000.0;
    if (a > 50 && v > 0) p->charge_w = a * v / 1e6; /* >50mA 才算充电 */
    CFRelease(props);
    return 0;
}

/* 菜单栏界面 (fansbar.m): 无参数启动时进入 */
int bar_main(void);
/* 授权执行 (fansbar.m): fansctl __ask <tool> [args...] */
int ask_main(int argc, char **argv);

#endif /* FANSCTL_H */
