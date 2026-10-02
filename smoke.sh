#!/bin/sh
# fansctl 冒烟测试 (make test): 只读命令 + setuid 助手越权防护。
# 不触碰风扇状态、不需要 root; 特权动词(__apply/__smart)不在测试范围。
# CI 的 macOS runner 是虚拟机, 无 AppleSMC 服务 —— 硬件读数测试自动跳过,
# 只保留构建产物校验、双语帮助与助手越权防护。
set -u
cd "$(dirname "$0")"
export FANSCTL_LANG=zh   # 固定中文, 断言不受本机 locale 影响
fail=0
ok()  { echo "  PASS  $1"; }
bad() { echo "  FAIL  $1"; fail=1; }

if ./fansctl fans >/dev/null 2>&1; then
echo "[fans] 列出风扇"
./fansctl fans | grep -q "风扇" && ok "有风扇输出" || bad "fans 无输出"

echo "[status] 转速与模式"
./fansctl status | grep -q "模式" && ok "输出含模式" || bad "status 异常"

echo "[temps] 温度传感器"
[ "$(./fansctl temps | grep -c '°C')" -ge 1 ] && ok "有温度读数" || bad "temps 无温度"

echo "[power] 供电/功率一览"
./fansctl power | grep -q "功率" && ok "有功率输出" || bad "power 无输出"

echo "[--json] 机器可读输出 (回归: usb 数组曾漏闭合括号)"
for c in fans status temps power; do
    if command -v python3 >/dev/null 2>&1; then
        ./fansctl $c --json | python3 -m json.tool >/dev/null 2>&1 && ok "$c --json 合法" || bad "$c --json 非法 JSON"
    else
        ./fansctl $c --json | grep -q '^\{' && ok "$c --json 有输出" || bad "$c --json 无输出"
    fi
done
./fansctl watch --json 1 | head -1 | python3 -m json.tool >/dev/null 2>&1 && ok "watch --json JSONL" || bad "watch --json 异常"

echo "[dump] 全量键导出 (回归: 曾因 >26 字节键 hex 拼接越界 SIGABRT)"
./fansctl dump > /dev/null 2>&1 && ok "完整导出不崩" || bad "dump 崩溃"

n=$(./fansctl fans | grep -c "风扇")
echo "[FNum] 风扇数量"
[ "$n" -ge 1 ] && ok "发现 $n 个风扇" || bad "无风扇"

echo "[i18n] 英文输出 (SMC 命令)"
FANSCTL_LANG=en ./fansctl status | grep -q "mode" && ok "英文 status 生效" || bad "英文 status 失败"
else
echo "== 无 AppleSMC 服务 (CI 虚拟机?), 跳过硬件读数测试 =="
fi

echo "[i18n] CLI 双语 (帮助不依赖 SMC)"
./fansctl __nope__ 2>&1 | grep -q "用法" && ok "中文帮助" || bad "中文帮助缺失"
FANSCTL_LANG=en ./fansctl __nope__ 2>&1 | grep -q "Usage" && ok "英文帮助生效" || bad "FANSCTL_LANG=en 无效"

echo "[bar/doctor] 不依赖 SMC 的子命令"
./fansctl bar status >/dev/null 2>&1 && ok "bar status 可跑" || bad "bar status 异常"
./fansctl __nope__ --json >/dev/null 2>&1 && bad "未知命令+--json 应报错" || ok "--json 越界命令被拒"
./fansctl doctor >/dev/null 2>&1 && ok "doctor 可跑 (rc=0)" || bad "doctor 异常"

echo "[helper] setuid 助手越权防护"
H=/usr/local/bin/fansctl-root
if [ -x "$H" ]; then
    if "$H" fans > /dev/null 2>&1; then
        bad "助手放行了非风扇动词!"
    else
        ok "助手拒绝非风扇动词"
    fi
else
    echo "  SKIP  助手未安装 (make install-root)"
fi

if [ "$fail" = 0 ]; then echo "== 全部通过 =="; else echo "== 有失败 =="; exit 1; fi
