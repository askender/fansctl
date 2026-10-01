# fansctl

macOS SMC 风扇/温度工具, Mac Studio M1 Max (Apple Silicon) 实测。
**一个程序两种形态**: 无参数 = 菜单栏应用(自动进后台, 不占终端); 带参数 = CLI。
零第三方依赖, 只链系统框架 (IOKit / CoreFoundation / AppKit / Security)。
版本: `fansctl version` (当前 1.0.0)。

## 构建与安装

```sh
make                # 编译出 ./fansctl
make install        # 装到 ~/.local/bin (先删后拷, AMFI 坑)
make install-root   # 装 setuid root 助手到 /usr/local/bin/fansctl-root (要一次密码)
make test           # 冒烟测试 (只读, 不动风扇)
make restart        # 重启菜单栏 (LaunchAgent 拉起)
```

**不变量**: 特权逻辑(`__apply`/`__smart`)变更后, `make` + `make install` 之外
必须再 `make install-root` 同步助手; 纯 UI/只读改动不需要。

## CLI

```
fansctl               启动菜单栏应用(fork 后台, 不占终端)
fansctl fans          列出风扇转速(只读)
fansctl status        当前/目标转速与模式(只读)
fansctl temps         所有温度传感器(只读)
fansctl dump          导出全部 SMC 键值(探索用)
fansctl watch [秒]    循环刷新(默认 2 秒)
sudo fansctl set <rpm> [N]   设定转速(不带 N = 全部风扇)
sudo fansctl max [N]         全速
sudo fansctl auto [N]        恢复自动
sudo fansctl smart [低 高]   智能曲线(默认 40~80°C, Ctrl+C 或 smart stop 退出恢复)
sudo fansctl smart stop      结束智能模式(含菜单栏启动的)
```

## 菜单栏

- 状态栏标题: `最热°C|最大转速(krpm)`, 2 秒刷新
- **全速 / 自动 / 智能模式** 三态互斥打钩; 点全速/自动先接管(停掉智能)再应用
- **智能阈值** 四档子菜单: 60~95(默认) / 55~95 / 45~85 / 40~80, 存 NSUserDefaults,
  智能运行中可热更新(SIGUSR1 + pidfile, 不打断控制)
- 点击动作后下拉自动重开; 菜单展开期间定时器也刷新(NSRunLoopCommonModes)
- 单实例守卫 `/tmp/fansctl.bar.pid`

## 智能模式

控制输入 = 全部温度传感器最热值(保守); T≤低阈值交回系统自动(可能停转),
低~高之间从基线线性插值到满速, ≥高阈值满速; 迟滞 1°C 防抖。
基线 = 接管瞬间系统的目标转速, 若风扇已是手动(上个会话残留)则退回 Mn。
状态 pidfile `/tmp/fansctl.smart.pid` = "pid 低 高", CLI 与菜单栏互通;
SIGTERM 优雅退出(恢复自动), 日志 `/tmp/fansctl.smart.log`
(含停止信号来源 pid, 排查莫名退出)。

## 免密与安全模型

`/usr/local/bin/fansctl-root` (root:wheel 4755) 是本程序副本, 菜单栏经它
免密执行特权动作。**白名单仅**: `__apply max|auto|set`、`__smart lo hi |
stop | thresh`、`smart stop`; 每个参数有范围校验, 无 shell、无环境展开,
不能被用作通用提权入口。助手未安装时菜单栏回退 `__ask` 子进程
AuthorizationExecuteWithPrivileges(每次弹密码框)。

## 开机自启

`~/Library/LaunchAgents/local.fansctl.bar.plist` (仓库有副本):

```sh
# 先把 plist 里的 /Users/USERNAME 改成本机用户名
cp local.fansctl.bar.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.fansctl.bar.plist
```

RunAtLoad + 崩溃自动拉起(SuccessfulExit=false), 菜单点"退出"不复活。

**开机兜底恢复** (`make install-restore`, 需一次密码): 系统级
LaunchDaemon `/Library/LaunchDaemons/local.fansctl.restore.plist`,
每次开机以 root 执行 `fansctl-root __apply auto` 一次——崩溃/断电后
SMC 若残留手动模式, 开机即恢复自动, 风扇不会卡死在最后转速。

## 本机 SMC 事实 (Mac Studio M1 Max, macOS 15)

- SMCKeyData 必须 80 字节(Apple Silicon), selector 2
- FNum=2; Mn=1200, Mx=5779/6241; 模式键 F0md/F1md (ui8, 1=手动),
  目标键 F0Tg/F1Tg ('flt ' f32 小端)
- 无 Ftst、无 FS! 键, 不需要解锁流程
- Apple Silicon 低温时风扇可完全停转, Ac/Tg=0 是真实状态

## 许可证

AGPL-3.0-or-later (见 [LICENSE](LICENSE))。任何人可自由使用/修改/分发,
但基于本项目的产品(含仅部署为网络服务、不分发二进制的形态)必须以
AGPL-3.0 开源其衍生代码。**商用允许, 闭源商用违反许可证**;
若用于商业产品, 欢迎告知作者。
