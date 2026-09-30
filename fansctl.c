/*
 * fansctl - macOS SMC 风扇/温度 CLI (Apple Silicon 兼容)
 *
 * 用法:
 *   fansctl               列出风扇转速(只读)
 *   fansctl status        当前/目标转速与模式
 *   fansctl temps         列出所有温度传感器
 *   fansctl dump          导出全部 SMC 键值(探索用)
 *   fansctl watch [秒]    循环刷新(默认 2 秒)
 *   sudo fansctl set <rpm> [N]   设定转速(不带 N = 全部风扇)
 *   sudo fansctl max [N]         全速(不带 N = 全部风扇)
 *   sudo fansctl auto [N]        恢复自动(不带 N = 全部风扇)
 *   sudo fansctl smart [低 高]   智能曲线(默认 40~80°C, Ctrl+C 退出恢复)
 */
#include <IOKit/IOKitLib.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void key_to_str(UInt32 key, char out[5]) {
    out[0] = (key >> 24) & 0xff; out[1] = (key >> 16) & 0xff;
    out[2] = (key >> 8) & 0xff;  out[3] = key & 0xff; out[4] = 0;
}

static UInt32 str_to_key(const char *s) {
    return ((UInt32)(UInt8)s[0] << 24) | ((UInt32)(UInt8)s[1] << 16) |
           ((UInt32)(UInt8)s[2] << 8)  |  (UInt32)(UInt8)s[3];
}

static int smc_open(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc) { fprintf(stderr, "未找到 AppleSMC 服务\n"); return -1; }
    kern_return_t r = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    if (r != KERN_SUCCESS) { fprintf(stderr, "IOServiceOpen 失败: 0x%x (试试 sudo)\n", r); return -1; }
    return 0;
}

static void smc_close(void) { if (g_conn) IOServiceClose(g_conn); }

static kern_return_t smc_call(SMCKeyData *in, SMCKeyData *out) {
    size_t sz = sizeof(SMCKeyData);
    return IOConnectCallStructMethod(g_conn, KERNEL_INDEX_SMC, in, sizeof(SMCKeyData), out, &sz);
}

static int g_debug;
static kern_return_t smc_read_key(const char *keyname, UInt8 *buf, size_t *len) {
    SMCKeyData in = {0}, out = {0};
    in.key = str_to_key(keyname);
    in.data8 = SMC_CMD_READ_KEYINFO;
    kern_return_t r = smc_call(&in, &out);
    if (g_debug) fprintf(stderr, "[dbg] %s keyinfo: kr=0x%x result=%u size=%u type=%#x\n", keyname, r, out.result, out.keyInfo.dataSize, out.keyInfo.dataType);
    if (r != KERN_SUCCESS) return r;
    if (out.result != 0) return kIOReturnNotFound;
    *len = out.keyInfo.dataSize;
    in.keyInfo.dataSize = out.keyInfo.dataSize;
    in.keyInfo.dataType = out.keyInfo.dataType;
    in.data8 = SMC_CMD_READ_BYTES;
    r = smc_call(&in, &out);
    if (r != KERN_SUCCESS) return r;
    memcpy(buf, out.bytes, *len);
    return KERN_SUCCESS;
}

static void type_str(UInt32 t, char out[5]) {    out[0] = (t >> 24) & 0xff; out[1] = (t >> 16) & 0xff;
    out[2] = (t >> 8) & 0xff;  out[3] = t & 0xff; out[4] = 0;
}

/* 返回: 0=成功解码并填入 value; 1=类型不支持(打印原始 hex); -1=读取失败 */
static int read_key_value(const char *keyname, UInt32 *type_out, double *value, char rawhex[80]) {
    UInt8 buf[32]; size_t len = sizeof(buf);
    kern_return_t r = smc_read_key(keyname, buf, &len);
    if (r != KERN_SUCCESS) {
        if (r == kIOReturnNotFound) return -1;
        return -1;
    }
    SMCKeyData in = {0}, out = {0};
    in.key = str_to_key(keyname);
    in.data8 = SMC_CMD_READ_KEYINFO;
    smc_call(&in, &out);
    UInt32 type = out.keyInfo.dataType;
    if (type_out) *type_out = type;
    char t[5]; type_str(type, t);
    rawhex[0] = 0;
    for (size_t i = 0; i < len; i++) snprintf(rawhex + strlen(rawhex), 8, "%02x ", buf[i]);

    if (strcmp(t, "fpe2") == 0 && len >= 2) { *value = (double)((buf[0] << 8) | buf[1]) / 4.0; return 0; }
    if (strcmp(t, "ui8 ") == 0 && len >= 1) { *value = buf[0]; return 0; }
    if (strcmp(t, "ui16") == 0 && len >= 2) { *value = (buf[0] << 8) | buf[1]; return 0; }
    if (strcmp(t, "ui32") == 0 && len >= 4) { *value = ((UInt32)buf[0]<<24)|((UInt32)buf[1]<<16)|((UInt32)buf[2]<<8)|buf[3]; return 0; }
    if (strcmp(t, "sp78") == 0 && len >= 2) { *value = (double)(int16_t)((buf[0] << 8) | buf[1]) / 256.0; return 0; }
    if (strcmp(t, "flt ") == 0 && len >= 4) { float f; memcpy(&f, buf, 4); *value = f; return 0; }
    return 1;
}

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

static int key_exists(const char *k) {
    SMCKeyData in = {0}, out = {0};
    in.key = str_to_key(k);
    in.data8 = SMC_CMD_READ_KEYINFO;
    kern_return_t r = smc_call(&in, &out);
    return r == KERN_SUCCESS && out.result == 0;
}

static int fan_count(void) {
    double v = 0; UInt32 t; char hex[80];
    if (read_key_value("FNum", &t, &v, hex) == 0 && v > 0 && v < 10) return (int)v;
    return 0;
}

static int read_rpm_key(int idx, const char *suffix, double *out) {
    char k[5];
    snprintf(k, sizeof(k), "F%d%s", idx, suffix);
    UInt32 t; char hex[80];
    if (read_key_value(k, &t, out, hex) == 0 && *out >= 0) return 0;
    return -1;
}

/* F?md / F?Md 探测; 找到返回1并填键名, 否则返回0 (用 FS! 位掩码) */
static int find_md_key(int idx, char out[5]) {
    char a[5], b[5];
    snprintf(a, sizeof(a), "F%dmd", idx);
    snprintf(b, sizeof(b), "F%dMd", idx);
    if (key_exists(a)) { snprintf(out, 5, "%s", a); return 1; }
    if (key_exists(b)) { snprintf(out, 5, "%s", b); return 1; }
    out[0] = 0;
    return 0;
}

/* 0=自动 1=手动 -1=未知 */
static int read_mode(int idx) {
    char md[5];
    double v = 0; UInt32 t; char hex[80];
    if (find_md_key(idx, md)) {
        if (read_key_value(md, &t, &v, hex) != 0) return -1;
        return (int)v == 1 ? 1 : 0;
    }
    if (read_key_value("FS! ", &t, &v, hex) == 0)
        return (((int)v) >> idx) & 1;
    return -1;
}

/* 返回 kern_return_t: KERN_SUCCESS=成功 */
static kern_return_t write_mode_bit(int idx, int manual) {
    char md[5];
    if (find_md_key(idx, md)) {
        UInt8 v = manual ? 1 : 0;
        return smc_write_key(md, &v, 1);
    }
    UInt8 buf[32]; size_t len = sizeof(buf);
    kern_return_t r = smc_read_key("FS! ", buf, &len);
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
    if (read_mode(idx) == 1) return 0;
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
    for (int attempt = 0; attempt < 10; attempt++) {
        if (smc_write_key(k, data, dlen) == KERN_SUCCESS) return 0;
        usleep(50 * 1000);
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
        if (smc_read_key("Ftst", buf, &len) == KERN_SUCCESS && len >= 1 && buf[0] == 1) {
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
        int mode = read_mode(i);
        printf("风扇%d  当前 %7.0f  目标 %7.0f  [%.0f~%.0f]  模式 %s\n",
               i, ac, tg, mn, mx, mode == 1 ? "手动" : mode == 0 ? "自动" : "未知");
    }
}

static UInt32 total_keys(void) {
    double v; char hex[80]; UInt32 t;
    if (read_key_value("#KEY", &t, &v, hex) == 0 && v > 0 && v < 100000) return (UInt32)v;
    UInt8 buf[32]; size_t len = sizeof(buf);
    if (smc_read_key("#KEY", buf, &len) == KERN_SUCCESS && len >= 4) {
        UInt32 le = buf[0] | buf[1]<<8 | buf[2]<<16 | (UInt32)buf[3]<<24;
        if (le > 0 && le < 100000) return le;
    }
    return 0;
}

static int get_key_at(UInt32 idx, char keyname[5]) {
    SMCKeyData in = {0}, out = {0};
    in.data8 = SMC_CMD_READ_INDEX;
    in.data32 = idx;
    kern_return_t r = smc_call(&in, &out);
    if (r != KERN_SUCCESS) return -1;
    key_to_str(out.key, keyname);
    return 0;
}

static void print_fans(void) {
    int n = 0;
    for (char c = '0'; c <= '9'; c++) {
        char k[5];
        snprintf(k, sizeof(k), "F%cAc", c);
        double v; UInt32 t; char hex[80];
        if (read_key_value(k, &t, &v, hex) != 0) continue;
        char km[5], kx[5], kid[5];
        snprintf(km, sizeof(km), "F%cMn", c);
        snprintf(kx, sizeof(kx), "F%cMx", c);
        snprintf(kid, sizeof(kid), "F%cID", c);
        double mn = -1, mx = -1;
        char hex2[80]; UInt32 t2;
        read_key_value(km, &t2, &mn, hex2);
        read_key_value(kx, &t2, &mx, hex2);
        char id[32] = ""; UInt8 idbuf[32]; size_t idlen = sizeof(idbuf);
        if (smc_read_key(kid, idbuf, &idlen) == KERN_SUCCESS && idlen > 0) {
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

static int plausible_temp(double v) { return v > -5 && v < 130; }

static void print_temps(void) {
    UInt32 n = total_keys();
    if (!n) { fprintf(stderr, "无法获取键总数\n"); return; }
    int found = 0;
    for (UInt32 i = 0; i < n; i++) {
        char k[5];
        if (get_key_at(i, k) != 0) continue;
        if (k[0] != 'T') continue;
        double v; UInt32 t; char hex[80];
        int rc = read_key_value(k, &t, &v, hex);
        char ts[5]; type_str(t, ts);
        if (rc == 0 && plausible_temp(v)) {
            printf("%-6s %9.2f °C\n", k, v);
            found++;
        } else if (rc == 1 && strcmp(ts, "flt ") == 0) {
            /* flt 解码失败的情形不会到这里, 占位 */
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

/* watch 用的温度键缓存: 首次枚举全部键, 之后只重读这些 */
static char (*g_tkeys)[5];
static int g_tkey_n;

static void scan_temp_keys(void) {
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
        double v; UInt32 t; char hex[80];
        if (read_key_value(k, &t, &v, hex) == 0 && plausible_temp(v)) {
            memcpy(list[cnt], k, 5);
            cnt++;
        }
    }
    g_tkeys = list;
    g_tkey_n = cnt;
}

/* 全部温度键里的最高读数; name_out 可为 NULL。无数据返回 -1000 */
static double hottest_temp(const char **name_out) {
    scan_temp_keys();
    double mx = -999;
    const char *mxk = NULL;
    for (int i = 0; i < g_tkey_n; i++) {
        double v; UInt32 t; char hex[80];
        if (read_key_value(g_tkeys[i], &t, &v, hex) == 0 && plausible_temp(v) && v > mx) {
            mx = v;
            mxk = g_tkeys[i];
        }
    }
    if (name_out) *name_out = mxk;
    return mx;
}

static void print_status_line(void) {
    for (char c = '0'; c <= '9'; c++) {
        char k[5];
        snprintf(k, sizeof(k), "F%cAc", c);
        double v; UInt32 t; char hex[80];
        if (read_key_value(k, &t, &v, hex) != 0) continue;
        printf("风扇%c %6.0f rpm  ", c, v);
    }
    const char *mxk = NULL;
    double mx = hottest_temp(&mxk);
    if (mx > -999) printf(" 最热 %s %.1f°C", mxk ? mxk : "?", mx);
    printf("\n");
}

/* ==================== 智能模式 (需 root, Ctrl+C 退出并恢复自动) ==================== */

static volatile sig_atomic_t g_stop = 0;
static void on_stop_signal(int sig) { (void)sig; g_stop = 1; }

static int smart_loop(double t_lo, double t_hi) {
    int n = fan_count();
    if (n < 1) n = 1;
    double base[10] = {0};   /* 离开自动那一刻系统想要的目标转速 */
    double last[10], want[10] = {0};
    int manual[10] = {0};
    for (int i = 0; i < 10; i++) last[i] = -1;
    signal(SIGINT, on_stop_signal);
    signal(SIGTERM, on_stop_signal);
    printf("智能模式: <=%.0f°C 自动 (基线跟随系统), >=%.0f°C 全速, 中间线性插值; Ctrl+C 退出并恢复自动\n",
           t_lo, t_hi);
    int rc = 0;
    while (!g_stop) {
        const char *tkey = NULL;
        double T = hottest_temp(&tkey);
        if (T < -999) { fprintf(stderr, "温度读取失败\n"); rc = 1; break; }
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
            if (frac <= 0 && !manual[f]) {
                double tg;
                if (read_rpm_key(f, "Tg", &tg) == 0) base[f] = tg < mn ? mn : tg;
                want[f] = 0;
                continue;
            }
            if (!manual[f]) {
                double tg;
                if (read_rpm_key(f, "Tg", &tg) == 0 && tg > mn) base[f] = tg;
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
    }
    for (int f = 0; f < n; f++)
        if (manual[f]) fan_auto(f);
    if (g_stop) printf("\n已退出智能模式, 全部恢复自动\n");
    return rc;
}

static void watch_loop(int interval) {
    for (;;) {
        print_status_line();
        fflush(stdout);
        sleep(interval);
    }
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "fans";
    g_debug = getenv("FANSCTL_DEBUG") != NULL;
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
    if (smc_open() != 0) return 1;
    int rc = 0;

    if (strcmp(cmd, "fans") == 0 || strcmp(cmd, "") == 0) {
        print_fans();
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strcmp(cmd, "temps") == 0) {
        print_temps();
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
            fprintf(stderr, "需要 root 权限:  sudo fansctl smart [低温°C] [高温°C]\n");
            rc = 1;
        } else {
            double t_lo = argc > 2 ? atof(argv[2]) : 40;
            double t_hi = argc > 3 ? atof(argv[3]) : 80;
            if (t_lo < 20 || t_hi < t_lo + 5 || t_hi > 120) {
                fprintf(stderr, "阈值无效: 需 20 < 低温 且 高温 >= 低温+5 且 高温 <= 120\n");
                rc = 1;
            } else {
                rc = smart_loop(t_lo, t_hi);
            }
        }
    } else {
        fprintf(stderr,
            "用法: fansctl [fans|status|temps|dump|watch|set|max|auto]\n"
            "  fans            风扇转速(默认, 只读)\n"
            "  status          风扇当前/目标转速与模式\n"
            "  temps           所有温度传感器\n"
            "  dump            导出全部 SMC 键\n"
            "  watch [秒]      循环刷新\n"
            "  set <rpm> [N]   设定转速, 不带 N 作用于全部风扇 (需 sudo)\n"
            "  max [N]         全速, 不带 N 作用于全部风扇 (需 sudo)\n"
            "  auto [N]        恢复自动, 不带 N 作用于全部风扇 (需 sudo)\n"
            "  smart [低 高]   智能曲线, 默认 40~80°C, Ctrl+C 退出恢复 (需 sudo)\n");
        smc_close();
        return 1;
    }
    smc_close();
    return rc;
}
