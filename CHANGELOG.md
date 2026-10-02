# Changelog

格式: 每个版本一节, GitHub Actions 发布 workflow 按 `## vX.Y.Z` 切片生成 Release 正文。

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
