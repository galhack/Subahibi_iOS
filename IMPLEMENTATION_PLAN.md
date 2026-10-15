# 素晴らしき日々 Apple 平台移植 — 实施计划

游戏引擎为 Ethornell/BGI 1.647（Compatibility 1.72，产品名 `SubarashikihibiHD`，1280×960）。
路线：以开源 C99 重实现 OpenBGI（GPL-2.0，`engine/openbgi`，上游提交见 `engine/UPSTREAM_COMMIT`）
为核心，补齐 1.647 兼容性，新写 Apple OS 后端（Metal 呈现、CoreText 字体、AudioUnit、pl_mpeg 影片），
用 SwiftUI 外壳覆盖 iPhone / iPad / Apple TV / Vision Pro。

关键发现：
- 1.647 操作码集 = 1.640 + {基本 55, 80 9F, 81 48, 91 00, 91 09, 92 99}（`tools/re/optables.py` 从原版 exe 恢复）。
- 汉化（BGI-CHS.exe）= 原版引擎 + Enigma Virtual Box 虚拟文件（剧本/图片/3 个系统脚本），文本为 GBK 原始字节；
  常用字中引导字节落在 0xA1–0xDF 的被替换成 GBK 扩展区生僻字，由 font.ttf（素晴字体）画成正确字形。
- 影片为 MPEG-1 PS（1280×960 25fps + MP2），音频为 BW 容器 Ogg Vorbis。
- fcxzzd.arc 是误放入目录的无关文本文件，游戏不需要。

## Stage 1: 引擎 1.647 兼容（macOS 无头模拟）
**Goal**: wasm-sim 无头构建在日文与中文资源下都能启动、进入标题、开始新游戏并推进剧情、存读档。
**Success Criteria**: 新增 `GEN_1_647` 代与游戏识别；脚本无未实现/错误指令；截图显示标题画面与对话文本。
**Tests**: `make test` 全过；`tools/sim/run.sh` 场景脚本（标题→新游戏→点击若干次→截图）日/中各一次。
**Status**: In Progress

## Stage 2: Apple OS 后端
**Goal**: `platform/apple` 实现 os.h：文件（沙盒路径+大小写不敏感）、线程/时间、Metal 呈现、触摸/遥控器/键盘→鼠标键盘事件、
CoreText 字体（日文 SJIS / 中文 GBK+font.ttf 回退）、AudioUnit 拉流、pl_mpeg 影片、设置存储。
**Success Criteria**: iOS 模拟器中游戏可玩，声音、影片、文字正确。
**Tests**: 后端单元测试（编码转换、路径解析、字体渲染尺寸）；模拟器截图比对。
**Status**: Not Started

## Stage 3: 多平台 App 外壳
**Goal**: XcodeGen 工程，iOS/iPadOS、tvOS、visionOS 原生目标；数据导入（Finder 文件共享 + 局域网推送 `tools/push_data.py`）、
汉化补丁自动解包（Enigma VFS）、语言选择、遥控器/注视捏合交互、画面适配。
**Success Criteria**: 四类实机安装运行，能导入数据并游玩。
**Tests**: 各平台构建通过；实机启动→标题→剧情→存读档。
**Status**: Not Started

## Stage 4: 打磨与验证
**Goal**: 性能（60fps 呈现、低耗电）、后台/前台切换、存档持久化（tvOS 存储约束）、影片与特效校验。
**Success Criteria**: 实机长时间游玩无卡顿与崩溃。
**Tests**: 关键场景回归脚本；实机人工验证清单。
**Status**: Not Started

## Stage 5: 文档与发布
**Goal**: README.md（安装、数据准备、各设备导入步骤、许可）；推送到 GitHub 私有仓库。
**Success Criteria**: 仓库不含游戏数据、Claude.md、凭证。
**Tests**: `git ls-files` 审计。
**Status**: Not Started
