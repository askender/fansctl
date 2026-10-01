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

clean:
	rm -f fansctl

.PHONY: all clean install uninstall
