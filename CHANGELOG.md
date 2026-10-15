# Changelog

格式: 每个版本一节, GitHub Actions 发布 workflow 按 `## vX.Y.Z` 切片生成 Release 正文。

## v1.8.3

- 语言检测修正: sudo/launchd 拉起时不再被终端的 LANG 串味。此前终端
  LANG=en 时 `sudo fansctl smart` 的日志与系统通知全是英文, 菜单栏拉起的
  守护却是中文 —— 现在统一优先读系统设置"语言与地区"的 AppleLanguages
  (控制台用户 GlobalPreferences, 经 /dev/console uid 定位, root 守护可读),
  `FANSCTL_LANG` 仍可强制, locale 环境变量退居其后
- smoke 修复 `temps --sort 降序` 断言竞态: 原在两次独立调用间比较前两行,
  机器满载温度秒级波动时误报乱序; 改为同一次输出内比较

## v1.8.2

- 恒温模式接入功耗前馈 (v1.8.0 只在智能模式有): 负载上坡时先垫转速再让
  PI 跟上, 超调更小。前馈加在输出上、不进 PI 累积状态, 退场不带出积分
  残差; 与智能模式共用 `read_net_w`/`ff_tick`, 日志同为 `ff+` 可 grep
- 守护日志每拍带本地时间戳 `[月-日 时:分:秒]` —— 日志一攒几天, 事后分析
  "前馈何时触发/温度何时冲高"必须有时间轴 (此前 27MB 日志无时间, 只能靠
  行号倒推)
- `doctor` 新增负载历史: 摘取 smart/hold 日志尾部 (最近 128KB) 的前馈触发
  行与最新状态行, 报"风扇为什么突然转"类 issue 时一眼对照功耗跳变
- man 页补充前馈语义与守护健壮性小节 (唤醒让权/MODE_REASSERT/写入退避/
  stop 核验), README 补真实触发日志示例
- 13 天实测调参结论 (v1.8.0 前馈参数复核): 前馈时代 2.9 万拍仅 1 次上坡
  事件 (5 拍), boost 峰值 +281rpm 远低于 1500 上限, 触发时温度 70.8→78.1°C
  见顶回落, 行为符合设计 —— **参数维持不变**。触发少的原因: 智能模式多在
  负载已稳态后启动, EMA 差只对启动后的上坡敏感

## v1.8.1

- 修复 v1.8.0 引入的 `smart stop`/`hold stop` 误判: pid 核验原要求进程可执行
  路径与自身完全一致, 但守护进程由 setuid 助手 (fansctl-root) 拉起时路径
  必然不同 —— 一律拒绝发信号, 停止失败。改按文件名核验 (fansctl /
  fansctl-root), 既防 pid 复用误杀, 又兼容三种拉起入口 (CLI/助手/仓库二进制)

## v1.8.0

- 智能模式功耗前馈: 热量还没到风扇先动。PSTR 秒级整机功耗经"快慢 EMA 差"
  检测负载上坡 (编译/加载大模型), 转速立刻抬升而不是等芯片热了才反应,
  实测可提前约 6 秒 (参考 TomEageer/fanctl 的思路)。稳态时前馈自然归零,
  不长期多转; 充电时自动扣除充电分量 (那部分能量入电池不发热);
  电池供电/无 PSTR 键的机器自动退回纯温度曲线
- 健壮性: SMC 写入被楔住 (温控器占用 0x82 等) 时智能/恒温模式不再直接退出,
  改为指数退避 (5s→60s 封顶) 持续监测温度, 写入恢复即自动回到控制
- 健壮性: `smart stop`/`hold stop` 先用 proc_pidpath 核验 pidfile 里的 pid
  还是本程序再发信号, pid 复用时拒绝误杀无辜进程
- 健壮性: 菜单栏同文不重设标题/菜单行, 消除全天 2 秒一拍的空重绘
  (setTitle 即使内容相同也会标脏整个半透明菜单栏背景)
- `__selftest` 新增功耗前馈闭环仿真 (合成热惯性模型驱动纯函数控制律,
  断言阶跃响应/限幅/温度有界/稳态归零), 40 项断言

## v1.7.1

- 修复 `bar install/uninstall` 与 `make restart` 对"launchd 之外实例"无效:
  菜单栏进程 setsid 后 launchd 只认自己拉起的那一代, 手动启动的旧实例
  会顶住单实例守卫让新实例静默退出, 栏上一直跑旧版。现在 install/uninstall
  会按 pidfile 显式清存活实例, restart 同理

## v1.7.0

- `fansctl temps --sort [--above N]`: 按温度降序、下限过滤 (237 行温度墙三行看清)
- `fansctl __selftest`: 内置纯逻辑自测 (命名表/JSON 转义/plist 解析/pidfile 往返,
  34 项), 无需 SMC, CI 虚拟机可跑; 顺手修了命名表匹配器短键越界隐患与
  JSON 数值 "-0" 输出
- `fansctl doctor` 增加未命名传感器计数 (欢迎提 issue 众包命名表)
- .github issue 模板 (bug 报告直接附 doctor 输出)
- 修复 v1.6.0 引入的 `power --watch` 参数解析 off-by-one —— flag 被静默忽略
  只输出一次; 冒烟测试新增真循环断言 (3 秒必须 3 行)

## v1.6.0

- 温度传感器人类可读名: `temps` 第三列与 `doctor` 最热行显示
  `TCMz (SoC)` 这样的名字; `--json` 增加 `name` 字段 (未知为 `null`)。
  映射依据 exelban/stats 传感器表 + Intel SMC 命名惯例
- 高温告警通知: 智能模式达到高阈值 (风扇满速) 或恒温模式压不住目标
  (持续高于目标 3°C) 时发一次系统通知 (osascript 横幅, 回落后解除)
- `fansctl power --watch [秒]`: 持续功率监测 (默认 5 秒一拍),
  `--json` 时为 JSONL, 可直接接 `jq`/记录管道

## v1.5.0

- `--json` 机器可读输出: fans/status/temps/power/watch (watch 为 JSONL),
  键名恒英文, 未知值 `null`
- `fansctl bar install|uninstall|status`: 开机自启一键管理 (LaunchAgent 自动生成,
  无需 sudo)
- `fansctl doctor`: 一页诊断 (机型/SMC/风扇/传感器/供电/助手/自启/守护/电池)
- man 手册页 + zsh/bash 补全 (`make install` 与 Homebrew 包自动落位)
- 修复 macOS 15 launchctl bootout 两参数形式 EIO; 修复 power --json USB
  数组漏闭合括号; 9 处 -Wformat-security
