# Changelog

格式: 每个版本一节, GitHub Actions 发布 workflow 按 `## vX.Y.Z` 切片生成 Release 正文。

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
