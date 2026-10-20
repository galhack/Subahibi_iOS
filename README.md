# 美好的每一天 ～不连续存在～ · Apple 平台移植

项目仓库：[galhack/Subahibi_iOS](https://github.com/galhack/Subahibi_iOS)。
安装包：[Releases](https://github.com/galhack/Subahibi_iOS/releases)。

把 ケロQ《素晴らしき日々 ～不連続存在～》（BGI/Ethornell 引擎，HD 版，产品名 `SubarashikihibiHD`）
原生移植到 **iPhone、iPad、Mac、Apple TV、Apple Vision Pro**。日文原版与【冥月·凌雪汉化组】中文补丁都可以游玩。

> **游戏本体和本篇汉化需由玩家自行取得并导入。** 本仓库与应用仅内置本文署名的后日谈社区增量，
> 不附原游戏 EXE、游戏封包、音乐、影片、本篇汉化 EXE 或玩家存档。

项目及四个平台应用已内置 **Knockin' on heaven's door** 后日谈社区汉化，
导入自己的游戏本体和本篇汉化后，选择简体中文即可使用。启动器提供启用开关，
后日谈入口仍按原作进度开放。

**致敬并感谢原社区汉化作者「笑笑竺斋」！**
原汉化包出处：[素晴日后日谈笑笑汉化补丁](https://www.bilibili.com/video/BV19m4y1m7Us)。
本项目保留原译文及剧情中的署名，仅进行 Apple 增量封装、加载与兼容字体适配。
使用方法见 [后日谈补丁说明](docs/AFTERSTORY_CHS.md)，资源位于
[app/Resources/AfterstoryCHS](app/Resources/AfterstoryCHS)。

各版本的改动与验证记录见 **[版本更新记录](CHANGELOG.md)**。

---

## 它是怎么工作的

- 引擎：开源的 BGI 重实现 [OpenBGI](https://github.com/Cytlan/openbgi)（C99，GPL-2.0），
  本项目为它补上了这款游戏需要的部分：
  - 新增 **1.647** 引擎版本配置（指令集从原版 BGI.exe 中逐表恢复，见 `tools/re/`）；
  - **CompressedBG v2** 图像解码（本作 5697 张图中有 5650 张是 v2，全部零失败）；
  - 游戏用到而缺失的 3 条指令、1.647 的帧率与文字函数行为、**直接从封包播放影片**。
- Apple 后端（`engine/openbgi/src/os/apple/`）：引擎在独立线程运行；Metal 呈现；
  Core Text 渲染字形（日文用冬青黑体/明朝；中文按 GBK 解码，优先使用汉化包自带 font.ttf，缺字回退苹方）；
  AudioToolbox 输出声音；pl_mpeg 解码 MPEG-1 影片；存档与游戏数据分离保存。
- 中文补丁：应用会在首次选择中文时自动解开 `BGI-CHS.exe` 内的 Enigma Virtual Box 虚拟文件
  （汉化剧本与图片），不需要在电脑上做任何预处理。
- 应用外壳：SwiftUI + UIKit，一个工程同时构建 iOS/iPadOS、Mac（Mac Catalyst）、tvOS、visionOS 原生版本。

已在以下真机验证（2026-10）：iPhone 16 Pro Max、iPad Pro、Apple TV 4K、Apple Vision Pro（原生 visionOS 应用），
日文与中文均可进入剧情；存档、读档跨重启可用；影片可从封包直接播放。

---

## 你需要准备

1. 正版游戏的安装目录（含 `sysprg.arc`、`system.arc`、`sysgrp.arc`、`data01000.arc` … `data05010.arc` 等）。
2. 想玩中文版时，目录中还需要汉化补丁的 `BGI-CHS.exe` 与 `font.ttf`（即"已打汉化补丁"的目录）。
3. 一台装有 **Xcode**（含 iOS/tvOS/visionOS 平台）的 Mac；侧载 iPhone/iPad 也可以用 **Sideloadly**。
4. 一个 Apple ID（免费账户即可，签名有效期 7 天，需定期重新安装；付费开发者账户为 1 年）。

需要的设备空间：游戏数据约 4.3 GB，中文补丁解包后另需约 350 MB。

---

## 构建与安装

### 直接下载安装包（v0.1.7）

无需自行构建时，可直接下载 [v0.1.7 Release](https://github.com/galhack/Subahibi_iOS/releases/tag/v0.1.7) 中对应设备的文件：

| 设备 | Release 安装包 | 安装方式 |
|---|---|---|
| iPhone / iPad | [Subahibi-ios.ipa](https://github.com/galhack/Subahibi_iOS/releases/download/v0.1.7/Subahibi-ios.ipa) | 使用 [Sideloadly](https://sideloadly.io/) 和自己的 Apple ID 签名后侧载 |
| Mac | [Subahibi-mac.zip](https://github.com/galhack/Subahibi_iOS/releases/download/v0.1.7/Subahibi-mac.zip) | 解压后打开应用，首次启动时选择游戏本体目录 |
| Apple TV | [Subahibi-tvos.ipa](https://github.com/galhack/Subahibi_iOS/releases/download/v0.1.7/Subahibi-tvos.ipa) | 需自行签名安装；也可按下方 Xcode 步骤构建并安装 |
| Vision Pro | [Subahibi-visionos.ipa](https://github.com/galhack/Subahibi_iOS/releases/download/v0.1.7/Subahibi-visionos.ipa) | 需自行签名安装；也可按下方 Xcode 步骤构建并安装 |

校验文件：[SHA256SUMS.txt](https://github.com/galhack/Subahibi_iOS/releases/download/v0.1.7/SHA256SUMS.txt)。
上述应用均已内置后日谈社区汉化，游戏本体与本篇汉化仍需自行导入。
使用现成安装包可以跳过下面的本地构建步骤；覆盖更新时保留原应用，避免删除已有数据和存档。

### 1. 生成 Xcode 工程

```sh
git clone https://github.com/galhack/Subahibi_iOS.git
cd Subahibi_iOS
SUBAHIBI_TEAM=<你的 Team ID> tools/apple/gen_project.sh
open app/Subahibi.xcodeproj
```

Team ID 可在 Xcode › Settings › Accounts 中查看（免费账户的个人团队也有 Team ID）。
也可以把它写进 `app/.team`（单行，不会提交到 git），之后直接运行 `tools/apple/gen_project.sh`。
脚本会自动下载 XcodeGen（仅在本机 `build/` 目录中）。工程无需额外安装 Metal 工具链或任何第三方库。

### 2. Mac 版

```sh
tools/apple/build_mac.sh          # 生成 build/Subahibi-mac.zip（本机签名，内置后日谈社区汉化）
```

解压后把 `Subahibi.app` 拖到「应用程序」。首次打开如被系统拦截，请右键 › 打开（该应用未经公证）。
也可以在 Xcode 中选择 "My Mac (Mac Catalyst)" 直接运行。Mac 版无需 Apple ID。

启动后点「选择游戏目录…」，指向电脑上的游戏安装目录即可，**不需要复制数据**；选择会被记住。
存档、汉化解包保存在 `~/Library/Application Support/Subahibi/`。

### 3. 安装到 iPhone / iPad / Apple TV / Vision Pro

**用 Xcode（四种设备都适用，Apple TV / Vision Pro 推荐此方式）**

1. 用数据线或同一局域网把设备与 Mac 配对（Apple TV 与 Vision Pro 在 Xcode › Window › Devices and Simulators 中配对）。
2. 在 Xcode 顶部选择目标设备，点击 Run（▶）。首次需要在设备上开启"开发者模式"，并在
   设置 › 通用 › VPN 与设备管理 中信任你的开发者证书。

**用 Sideloadly（iPhone / iPad）**

```sh
tools/apple/build_ipa.sh ios      # 生成 build/Subahibi-ios.ipa（未签名，内置后日谈社区汉化）
```

在 Sideloadly 中选择该 IPA、你的设备和 Apple ID，点击 Start。

> 免费 Apple ID 每台设备最多同时保留 3 个侧载应用，若安装失败请先删除一个旧的侧载应用。

---

## 导入游戏数据

应用第一次打开时会提示"尚未找到游戏文件"。把**整个游戏目录**放进应用即可，目录名随意。

| 设备 | 推荐方式 |
|---|---|
| iPhone / iPad | 数据线连接 Mac → 访达 › 选中设备 › **文件** › 「美好的每一天」→ 把游戏目录拖进去 |
| Vision Pro | 用隔空投送把游戏目录发到 Vision Pro，保存到「文件 › 我的 Apple Vision Pro › 美好的每一天」；或用下面的局域网传输 |
| Apple TV | 只能用局域网传输（tvOS 没有文件管理） |
| 任意设备 | 局域网传输（见下） |

### 局域网传输

1. 在设备上打开应用，打开「局域网接收游戏文件」，记下屏幕上显示的 **地址** 和 **配对码**。
2. 在同一网络的 Mac 上运行（只需系统自带的 Python 3）：

   ```sh
   python3 tools/push_data.py --host <设备地址> --code <配对码> /path/to/你的游戏目录
   ```

   脚本只发送需要的文件（`.arc` 封包、`BGI-CHS.exe`、`font.ttf`、`BGI.gdb`、`BGI.hvl`），
   设备上已有且大小相同的文件会跳过，中断后重新运行即可续传。
3. 传完后在应用里点「重新检查」。

配对码每次开启服务都会重新生成；连续输错 5 次会自动更换配对码并暂停接收。

### 开发者：用 devicectl 直接推送

已连接 Xcode 的设备也可以直接把文件写进应用的数据容器（iPhone/iPad/Vision Pro 写到 `Documents/Game`，
Apple TV 写到 `Library/Caches/Game`）：

```sh
xcrun devicectl device copy to --device <设备> \
  --source data01000.arc --source ... \
  --destination Documents/Game \
  --domain-type appDataContainer --domain-identifier org.local.subahibi.<Team ID>
```

---

## 开始游戏

在启动页选择语言（检测到汉化补丁时可选「简体中文」），点「开始游戏」。
首次选择中文时会先解开汉化补丁（几秒钟）。

### 操作方式

| 输入 | 对应 |
|---|---|
| 轻点 | 鼠标左键（推进文字、选择） |
| 双指轻点 | 鼠标右键（游戏菜单 / 隐藏文字框） |
| 上下滑动 | 鼠标滚轮（向下滑 = 打开回想） |
| 长按 | 按住 Ctrl（快进），松手停止 |
| Mac / iPad 鼠标与触控板 | 与 PC 版相同：按下、拖动（如回想滚动条）、左右键、滚轮 |
| 硬件键盘 | Enter、空格、Esc、方向键、Ctrl（按住快进）、Page Up/Down 等 |
| Apple TV 遥控器 | 触控板移动指针，按下 = 左键，返回键 = 右键，上/下键 = 滚轮，播放/暂停 = 按住快进 |
| 游戏手柄 | 左摇杆移动指针，A = 左键，B = 右键，RT = 快进，十字键上/下 = 滚轮 |
| Vision Pro | 注视 + 捏合 = 轻点；其余同触控 |

游戏结束（标题选 EXIT）后应用会提示退出，再次游玩请重新打开应用。

---

## 存档

- 存档与游戏数据分开保存（iPhone/iPad/Vision Pro 在 Application Support，Mac 在 `~/Library/Application Support/Subahibi`，Apple TV 在缓存区），**日文版与中文版各自独立**
  （汉化版存档含 GBK 文本，原版作品同样不能混用）。
- 首次启动时会把游戏目录中的 `BGI.gdb`（全局设定、已读记录、CG 收集）复制一份作为起点，
  电脑上的进度因此可以带过来。
- 删除应用会同时删除存档。iPhone/iPad 可在访达中备份应用数据。

---

## 已知限制

- 免费 Apple ID 签名 7 天后过期，需要重新安装（不会删除已导入的数据和存档）。
- Apple TV：tvOS 只允许应用写入缓存区，游戏数据、汉化解包与**存档**都保存在那里；
  系统空间紧张时可能被清除（届时需重新传输数据，存档也会丢失），重要进度请留意。
- Vision Pro 的注视不会产生"悬停"，按钮的高亮会在点击时才出现。
- 0.1.7 已内置后日谈社区汉化；其余未汉化内容仍取决于玩家导入的本篇补丁。
- 游戏目录中的 `fcxzzd.arc` 是与游戏无关的文本文件，不需要也不会被传输。

---

## 开发与调试

| 路径 | 内容 |
|---|---|
| `engine/openbgi/` | OpenBGI 引擎（上游版本见 `engine/UPSTREAM_COMMIT`）及本项目修改 |
| `engine/openbgi/src/os/apple/` | Apple 平台后端 |
| `app/` | XcodeGen 工程定义、Swift 外壳、EVB 解包器 |
| `tools/re/` | 原版 BGI.exe 分析脚本（操作码表恢复、缺失指令普查等） |
| `tools/sim/` | 无头模拟构建与场景脚本 |
| `tools/apple/` | 工程生成、IPA 打包、图标生成 |
| `tools/check/` | 图像解码全量检查、截断模糊测试、EVB 比对 |

在 Mac 上不用模拟器即可运行同一套 Apple 后端：

```sh
cd engine/openbgi && make PLATFORM=apple CC=clang VORBIS=stb bin/openbgi-hosttest
bin/openbgi-hosttest /path/to/game --seconds=60 --script="wait 10 click 640 480 shot out.png"
bin/openbgi-hosttest /path/to/game --overlay=<解包后的汉化目录> --font=/path/to/game/font.ttf --chinese ...
```

真机/模拟器上可用环境变量做无人值守测试：`SUBAHIBI_AUTOSTART=ja|zh` 跳过启动页，
`SUBAHIBI_SCRIPT="wait 6 click 640 480 shot title"` 注入输入并把截图写到 `Documents/Debug/`。

引擎测试与大图加载回归测试：

```sh
cd engine/openbgi
make -j8 PLATFORM=apple CC=clang VORBIS=stb test
# 可选：用自己购买的游戏验证全部 10 张超过 32 MiB 的图片
BGI_GAME_DIR=/absolute/path/to/game bin/test_imageload
```

`tests/imageload.c` 默认生成临时图片，覆盖同步加载、等待加载和缓存命中，
不含游戏素材。内存检查可用 `make -B` 重建该测试，并设置
`OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"`；
完成后用默认编译选项重新构建，避免混用带检查器的目标文件。

本仓库忽略游戏目录、封包、凭证、签名文件、构建结果、`agents.md` / `AGENTS.md`
及 `IMPLEMENTATION_PLAN.md`。公开源码不包含本地调试报告和存档。

---

## 许可

- 本项目代码（含引擎修改与 Apple 后端、应用外壳）以 **GPL-2.0** 发布，见 `LICENSE`。
- 第三方组件（OpenBGI、pl_mpeg、stb_vorbis、参照 GARbro 的 CBG v2 算法）见 `THIRD_PARTY_NOTICES.md`。
- 《素晴らしき日々 ～不連続存在～》及其一切内容版权归 ケロQ 所有；中文补丁归【冥月·凌雪汉化组】所有，
  请遵守其声明（勿用于商业用途）。本项目与上述权利人无关，仅供已购买正版的玩家在自己的设备上游玩。
- 后日谈社区汉化致敬原作者 **笑笑竺斋**，出处为
  [素晴日后日谈笑笑汉化补丁](https://www.bilibili.com/video/BV19m4y1m7Us)。
  社区译文及字体等资源保留原署名，不因随项目发布而改变其原有权利归属。
