#!/bin/sh
# fansctl 冒烟测试 (make test): 只读命令 + setuid 助手越权防护。
# 不触碰风扇状态、不需要 root; 特权动词(__apply/__smart)不在测试范围。
set -u
cd "$(dirname "$0")"
fail=0
ok()  { echo "  PASS  $1"; }
bad() { echo "  FAIL  $1"; fail=1; }

echo "[fans] 列出风扇"
./fansctl fans | grep -q "风扇" && ok "有风扇输出" || bad "fans 无输出"

echo "[status] 转速与模式"
./fansctl status | grep -q "模式" && ok "输出含模式" || bad "status 异常"

echo "[temps] 温度传感器"
[ "$(./fansctl temps | grep -c '°C')" -ge 1 ] && ok "有温度读数" || bad "temps 无温度"

echo "[power] 供电/功率一览"
./fansctl power | grep -q "功率" && ok "有功率输出" || bad "power 无输出"

echo "[dump] 全量键导出 (回归: 曾因 >26 字节键 hex 拼接越界 SIGABRT)"
./fansctl dump > /dev/null 2>&1 && ok "完整导出不崩" || bad "dump 崩溃"

n=$(./fansctl fans | grep -c "风扇")
echo "[FNum] 风扇数量"
[ "$n" -ge 1 ] && ok "发现 $n 个风扇" || bad "无风扇"

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
