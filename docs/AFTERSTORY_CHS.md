# Apple 版后日谈增量中文字幕

此功能从 0.1.6 开始提供，用于原作标题界面的 **Knockin' on heaven's door** 后日谈。
资源来自用户自行取得的“后日谈补丁”，沿用其翻译及署名，不包含重新翻译。
开源仓库、IPA 和 Mac 应用均不附带这些资源或字体。

## 本地制作

需要匹配的原汉化游戏目录（原 `BGI-CHS.exe` 与 `font.ttf`）和“后日谈补丁”目录
（新 `BGI-CHS.exe` 及其子目录中的新 `font.ttf`）。本地制作工具需要 macOS 的 C 编译器、
Python 3.10 或更新版本及 fontTools。

```sh
python3 -m venv .venv
.venv/bin/python -m pip install fonttools
.venv/bin/python tools/patch/build_afterstory_patch.py \
  --game /path/to/original-chinese-game \
  --epilogue /path/to/后日谈补丁 \
  --output build/afterstory-local
```

输出目录必须尚不存在，以免覆盖已有资源。工具只读取输入，解开两份 EXE 后比较资源，
如果出现后日谈范围以外的增删或修改就停止。输出包含一个可复制的 `AfterstoryCHS` 文件夹
及同内容 ZIP。本包包含汉化脚本、图片和派生字体，须保留在本地，不要提交或上传。

## 安装与使用

1. 覆盖更新到 0.1.6 或更新版应用，保留原应用容器；先备份自己的存档。
2. 解压小包，将整个 `AfterstoryCHS` 文件夹放在实际游戏目录，与 `sysprg.arc` 同级。
   保留原来的封包、`BGI-CHS.exe`、`font.ttf` 和所有存档。
3. 完全退出并重新打开应用。启动器会显示检测到后日谈增量汉化；选择简体中文并开始游戏，
   应用随后校验全部文件。缺文件或不匹配时会提示具体错误。
4. 达到原作解锁条件后，在标题界面选择 `Knockin' on heaven's door`。
   `EXTRA` 是鉴赏菜单。未通关时，本补丁不会替玩家开放后日谈入口。

iPhone、iPad、Vision Pro 可用 Finder 文件共享或“文件”App 复制文件夹；
Mac 将文件夹放进启动器选择的游戏目录。

Apple TV 或其他设备可使用局域网传输：在应用启动器开启接收，将本地解压目录作为参数。
这里只传小补丁，不需要再次发送全部游戏。

```sh
python3 tools/push_data.py --host 192.168.1.23 --code 123456 \
  --patch-only /path/to/unzipped-parent
```

父目录下应直接有 `AfterstoryCHS/patch.json`。`--patch-only` 会重传补丁所有文件并最后写入清单，
避免把字节数相同的旧资源当成已完成。传输完成后关闭接收、完全退出并重启应用。

## 内容边界与移除

增量包只包含四个后日谈资源：`extra`、`8_af`、`8-1_extra_01-1`、`8-1_extra_01-2`，
以及独立的 `afterstory-font.ttf`、`patch.json` 和说明。
其中 `8_af` 是既有后日谈衔接脚本，其他三个是新增资源。
没有替换本篇脚本、封包或原字体文件。日文模式不加载该目录。

新旧字体在一个码位上分别绘制“县”和“龕”，直接换用后日谈字体会改变本篇的“神龕”。
工具保留全部旧字体映射，追加后日谈字形，并为后日谈三处“县”字保留一个未使用编码。
生成后重新打开字体，验证旧轮廓、横竖向度量、排版指标及新增字形均符合预期。

移除时退出应用，只删除或移走 `AfterstoryCHS` 文件夹，再重新启动即可。
存档不会被删除；后日谈中保存的存档应在补丁启用时使用。

## 验证范围

提供源代码检查：

```sh
.venv/bin/python tools/check/afterstory_test.py
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun swiftc \
  -module-cache-path build/swift-module-cache \
  app/Sources/App/AfterstoryPatch.swift tools/check/AfterstoryPatchTest.swift \
  -o build/afterstory-test
build/afterstory-test
```

2026-10-21 的本地资源验证覆盖旧／新 231 个场景、310,997 条字符串引用，
保留全部 3,912 个旧 cmap 映射，轮廓及度量均无变化，Core Text 注册成功。
最终增量包经 Apple 后端从原作入口运行，确认后日谈开头中文字幕、语音与推进。
iPhone 16 Pro Max 已覆盖安装 0.1.6、导入独立小包，从玩家现有进度的后日谈入口
确认中文字幕与推进；更新前后 10 个存档及进度文件逐字节一致。
未对后日谈全篇逐句通关验证，本次未在其他 Apple 设备上实机复测。
