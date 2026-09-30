CC = /usr/bin/clang
CFLAGS = -O2 -Wall -framework IOKit -framework CoreFoundation
PREFIX = $(HOME)/.local/bin

fansctl: fansctl.c
	$(CC) $(CFLAGS) -o $@ $<

# 注意: 不能 cp 原地覆盖旧二进制 — AMFI 对 inode 缓存签名, 覆盖后 exec 会被 SIGKILL
install: fansctl
	rm -f $(PREFIX)/fansctl
	cp fansctl $(PREFIX)/fansctl

uninstall:
	rm -f $(PREFIX)/fansctl

clean:
	rm -f fansctl

.PHONY: clean install uninstall
