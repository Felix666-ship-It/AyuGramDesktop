# AyuGramMod

[![Release](https://img.shields.io/github/v/release/Felix666-ship-It/AyuGramDesktop?label=Release&color=brightgreen)](https://github.com/Felix666-ship-It/AyuGramDesktop/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Felix666-ship-It/AyuGramDesktop/total?label=Downloads&color=blue)](https://github.com/Felix666-ship-It/AyuGramDesktop/releases)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-0078D6)](https://github.com/Felix666-ship-It/AyuGramDesktop/releases/latest)

**AyuGramMod** 是基于 [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop)（上游内核 `7.2.9`）定制的 Windows x64 桌面客户端，专注于更顺手的中文体验、增强型批量媒体下载以及独立的应用内自动更新。

## 主要特性

- **增强型批量媒体下载**
  - **无限并发**：解除原生并发下载数量限制，支持高速批量保存视频与图片。
  - **原生进度集成**：完整接入客户端原生下载管理器，批量下载时左下角实时显示总进度，并可在下载管理面板查看详情。
  - **自动分类与去重**：支持按不同消息分组自动创建子文件夹；遇到同名文件自动追加 ` - 副本` 避免覆盖。
- **开箱即用的中文化与主题**
  - 首次启动默认使用**简体中文**界面，安装向导同步提供中文支持。
  - 内置并默认启用「**小而美 8.1 细节优化**」主题。
- **独立的应用内自动更新**
  - 已禁用上游更新通道，避免被官方包覆盖。
  - 直连本仓库 GitHub Releases，支持在客户端内一键检查更新、后台签名校验下载并通过 `Updater.exe` 自动覆盖重启。
- **完整保留 AyuGram 原生功能**
  - 保留防撤回、幽灵模式、本地消息记录等全部上游特性。

## 下载与安装

前往 [**Releases 页面**](https://github.com/Felix666-ship-It/AyuGramDesktop/releases/latest) 获取最新版本：

| 版本类型 | 文件名 | 适用场景 |
| :--- | :--- | :--- |
| **安装版** | `AyuGramMod-Setup-x64-*.exe` | 双击运行中文安装向导，自动创建桌面与开始菜单快捷方式 |
| **便携版** | `AyuGramMod-Portable-x64-*.zip` | 解压到任意具有写入权限的目录，直接运行 `AyuGramMod.exe` |

> **提示**：便携版请勿解压到 `C:\Program Files` 等受系统权限保护的目录，以免影响 `tdata` 写入及应用内自动更新。

## 应用内自动更新

1. 打开 **设置 → 高级 → 版本和更新**，开启 **自动更新**。
2. 点击 **检查更新**，检测到新版本后将自动在后台下载并校验签名，完成后点击重启即可无缝升级。
3. 更新包托管于 GitHub Releases，使用前请确保已在客户端内配置可用代理，或本机已开启系统代理 / TUN 模式。

## 致谢与协议

本项目基于 [AyuGramDesktop](https://github.com/AyuGram/AyuGramDesktop) 与 [Telegram Desktop](https://github.com/telegramdesktop/tdesktop) 进行二次修改与打包，遵循原项目开源许可协议（GPLv3）。
