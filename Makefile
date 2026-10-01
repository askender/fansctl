CC = /usr/bin/clang
CFLAGS = -O2 -Wall -framework IOKit -framework CoreFoundation -framework AppKit -framework Security
PREFIX = $(HOME)/.local/bin

all: fansctl

fansctl: fansctl.c fansbar.m fansctl.h
	$(CC) $(CFLAGS) -o $@ fansctl.c fansbar.m

# 注意: 不能 cp 原地覆盖旧二进制 — AMFI 对 inode 缓存签名, 覆盖后 exec 会被 SIGKILL
install: all
	rm -f $(PREFIX)/fansctl $(PREFIX)/fansbar
	cp fansctl $(PREFIX)/

uninstall:
	rm -f $(PREFIX)/fansctl $(PREFIX)/fansbar
	sudo rm -f /usr/local/bin/fansctl-root
	sudo launchctl bootout system/local.fansctl.restore 2>/dev/null || true
	sudo rm -f /Library/LaunchDaemons/local.fansctl.restore.plist

# 安装 setuid root 助手: 菜单栏控制免密执行 (仅放行固定风扇动作)
install-root: all
	sudo rm -f /usr/local/bin/fansctl-root
	sudo cp fansctl /usr/local/bin/fansctl-root
	sudo chown root:wheel /usr/local/bin/fansctl-root
	sudo chmod 4755 /usr/local/bin/fansctl-root

# 开机兜底: 系统级 LaunchDaemon, 每次开机以 root 恢复风扇自动,
# 防止崩溃/断电后 SMC 残留手动模式把风扇卡死在最后的转速
install-restore:
	sudo cp local.fansctl.restore.plist /Library/LaunchDaemons/
	sudo chown root:wheel /Library/LaunchDaemons/local.fansctl.restore.plist
	sudo launchctl bootstrap system /Library/LaunchDaemons/local.fansctl.restore.plist 2>/dev/null || true

clean:
	rm -f fansctl

# 冒烟测试: 只读命令 + setuid 助手越权防护, 不改变风扇状态
test: fansctl
	./smoke.sh

# 重启菜单栏 (LaunchAgent 拉起; 已在运行的智能模式不受影响, setsid 隔离)
restart:
	launchctl kickstart -k gui/$(shell id -u)/local.fansctl.bar

.PHONY: all clean install uninstall test restart
