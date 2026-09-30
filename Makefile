CC = /usr/bin/clang
CFLAGS = -O2 -Wall -framework IOKit -framework CoreFoundation
BARFLAGS = -O2 -Wall -framework IOKit -framework CoreFoundation -framework AppKit
PREFIX = $(HOME)/.local/bin

all: fansctl fansbar

fansctl: fansctl.c fansctl.h
	$(CC) $(CFLAGS) -o $@ fansctl.c

fansbar: fansbar.m fansctl.h
	$(CC) $(BARFLAGS) -o $@ fansbar.m

# 注意: 不能 cp 原地覆盖旧二进制 — AMFI 对 inode 缓存签名, 覆盖后 exec 会被 SIGKILL
install: all
	rm -f $(PREFIX)/fansctl $(PREFIX)/fansbar
	cp fansctl fansbar $(PREFIX)/

uninstall:
	rm -f $(PREFIX)/fansctl $(PREFIX)/fansbar

clean:
	rm -f fansctl fansbar

.PHONY: all clean install uninstall
