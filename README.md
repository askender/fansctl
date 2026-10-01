# fansctl

macOS SMC 风扇/温度工具, MacBook Pro 16" M1 Max (MacBookPro18,4) 实测。
**一个程序两种形态**: 无参数 = 菜单栏应用(自动进后台, 不占终端); 带参数 = CLI。
零第三方依赖, 只链系统框架 (IOKit / CoreFoundation / AppKit / Security)。
版本: `fansctl version` (当前 1.2.1)。

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
fansctl power         供电/功率一览(只读): 功率, 电源输入, 适配器, USB 设备, 功耗Top进程
fansctl dump          导出全部 SMC 键值(探索用)
fansctl watch [秒]    循环刷新(默认 2 秒)
sudo fansctl set <rpm> [N]   设定转速(不带 N = 全部风扇)
sudo fansctl max [N]         全速
sudo fansctl auto [N]        恢复自动
sudo fansctl smart [低 高]   智能曲线(默认 40~80°C, Ctrl+C 或 smart stop 退出恢复)
sudo fansctl smart stop      结束智能模式(含菜单栏启动的)
sudo fansctl hold [°C]       恒温模式, PI 闭环稳定最热传感器(默认 70°C, 与智能互斥)
sudo fansctl hold stop       结束恒温模式(含菜单栏启动的)
```

## 菜单栏

- 状态栏标题: `最热°C|最大转速(krpm)`, 2 秒刷新
- 下拉第一行: 最热传感器/曲线%或恒温目标 + 机器功率 (SMC 键 `PSTR`,
  与 ioreg SystemPowerIn 同源; 无此键的机器自动隐藏)。充电时扣除
  Amperage×Voltage 充电分量并分列显示, 如 `34W+10W充` = 机器 34W +
  电池充电 10W (电池侧电压恒定 ~12.5V, 故用瓦数而非 V×A 展示)
- **全速 / 自动 / 智能模式 / 恒温模式** 四态互斥打钩; 点任意控制项先接管
  (停掉运行中的智能/恒温)再应用
- **智能阈值** 四档子菜单: 60~95(默认) / 55~95 / 45~85 / 40~80, 存 NSUserDefaults,
  智能运行中可热更新(SIGUSR1 + pidfile, 不打断控制)
- **恒温目标** 四档子菜单: 65 / 70(默认) / 75 / 80°C, 运行中同样可热更新
- 点击动作后下拉自动重开; 菜单展开期间定时器也刷新(NSRunLoopCommonModes)
- 退出(⌘Q): 智能未运行而风扇在手动时, 退出前经助手恢复自动(不留孤儿转速);
  智能运行中退出 UI 不影响守护进程
- 单实例守卫 `/tmp/fansctl.bar.pid`

## 智能模式

控制输入 = 全部温度传感器最热值(保守); T≤低阈值交回系统自动(可能停转),
低~高之间从基线线性插值到满速, ≥高阈值满速; 迟滞 1°C 防抖。
基线 = 接管瞬间系统的目标转速, 若风扇已是手动(上个会话残留)则退回 Mn。
状态 pidfile `/tmp/fansctl.smart.pid` = "pid 低 高", CLI 与菜单栏互通;
SIGTERM 优雅退出(恢复自动), 日志 `/tmp/fansctl.smart.log`
(含停止信号来源 pid, 排查莫名退出)。

健壮性:

- **唤醒让权** — 拍长被拉长(连续时钟 `mach_continuous_time`, 睡眠也走表)
  判定系统刚唤醒, 先交还系统控制静置 5 秒, 传感器稳定后按新基线重新接管
- **MODE_REASSERT** — 每拍核验手动权与目标转速, 被外部工具/系统改写
  (改回自动、覆盖目标)立即重新接管或重写目标
- **写入退避** — SMC 忙(0x82 温控器占用)时指数退避重试约 1.5 秒, 不硬敲
- **smart stop 等收尾** — 等守护进程完成"恢复自动 + 清 pidfile"再返回,
  防止其退出清理覆盖调用方随后的立即设速; 超时如实报失败
- SMC 读取失败时重开连接重试 3 次(唤醒初期连接可能短暂失效)

## 恒温模式

与智能模式(开环曲线)互补的**闭环**控制: 直接给定目标温度, PI 控制器把
最热传感器稳定在目标上。

- 速度形式 PI: 转速增量 = 25×Δ误差 + 8×误差 − 150×温度趋势(每秒),
  增量限幅 ±250rpm — 无积分饱和, 趋势项抑制超调
- 死区: 误差 <0.3°C 且温度平稳时保持转速, 防抖动
- 低于目标 3°C 交回系统自动(风扇可停转), 回到 1.5°C 内重新接管(迟滞)
- 与智能模式全面互斥: 任一启动会先收走另一方的控制权
- 状态 pidfile `/tmp/fansctl.hold.pid` = "pid 目标°C", `hold stop` 等收尾;
  日志 `/tmp/fansctl.hold.log`; 唤醒让权/MODE_REASSERT 与智能模式同款

## 功率查询

`fansctl power` 一次汇总供电全景 (只读, 无需 root):

- **机器功率** — SMC 键 `PSTR` (秒级实时, 与菜单栏第一行同源), 充电时
  拆分系统/充电分量
- **电源输入** — AppleSmartBattery 遥测 `SystemVoltageIn × SystemCurrentIn`,
  即电源口实际进来的功率; 这组遥测约分钟级刷新, 短期波动以 PSTR 为准
- **适配器** — `AdapterDetails` 额定瓦数与 PD 协商电压
- **USB 设备** — 逐个列出声明的 5V 电流需求 (IOUSBLib 读配置描述符
  bMaxPower, USB2 按 2mA、USB3+ 按 8mA 单位换算), 自供电设备会标注
- **功耗 Top 进程** — 0.4 秒两次采样的当前 CPU% 降序, 附内存/%MEM/PID;
  macOS 限制: 非特权进程只能读本用户进程的占用 (系统进程如 WindowServer
  需 `sudo fansctl power`), 每进程 GPU 占用无公开接口

局限: macOS 不提供外设**实际**拉取功率或对外 PD 协商结果的公开接口,
USB 侧只有设备自报的声明值 (实测需外接功率计)。

## 免密与安全模型

`/usr/local/bin/fansctl-root` (root:wheel 4755) 是本程序副本, 菜单栏经它
免密执行特权动作。**白名单仅**: `__apply max|auto|set`、`__smart lo hi |
stop | thresh`、`__hold °C | stop | target`、`smart stop`; 每个参数有
范围校验, 无 shell、无环境展开, 不能被用作通用提权入口。
助手未安装时菜单栏回退 `__ask` 子进程
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

## 本机 SMC 事实 (MacBook Pro 16" M1 Max, macOS 15)

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

## 致谢

以下三项机制的思路来自 [TomEageer/fanctl](https://github.com/TomEageer/fanctl)
(Python 实现, MIT 许可), 本项目以 C 独立重新实现:

- 开机兜底恢复 (LaunchDaemon)
- 健壮性: 唤醒让权 / 模式重申 (MODE_REASSERT) / 写入退避
- 恒温模式: 目标温度 PI 闭环 (含趋势阻尼)

感谢原作者的开源分享。
