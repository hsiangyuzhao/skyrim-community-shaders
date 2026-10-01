# Batch 33 一次安装 A/B 测试

此压缩包是一个完整的 Community Shaders Data 根目录。只安装这一份包，不要叠装旧的 Core 或单独 feature 包。包含 batch33 的 SSGI Legacy/REBLUR、SSRT specular Full/Checkerboard，以及 DLSS NR 与 DLSS-G 的实验入口。SSRT 是否拆分及上游效率候选属于分析结论，没有对应运行时开关。

## 进入游戏前

1. 将包安装到实际使用的 Skyrim `Data` 目录或模组管理器的同等目录。先退出游戏。
2. 在 PowerShell 7 中运行：

   ```powershell
   pwsh -File "<Data>\Batch33-Test\Prepare-Batch33OneLaunch.ps1" -DataPath "<Data>"
   ```

   这个步骤只把 `SettingsUser.json` 中 `Upscaling.frameGenerationBackend` 设为 `1`（DLSS-G）、`Upscaling.upscaleMethod` 设为 `3`（DLSS）；其余用户设置原样保留。原文件先按字节备份，结果写入 `SKSE\Plugins\CommunityShaders\Batch33-Preflight.json`。若模组管理器将用户设置写在其他目录，加 `-SettingsDirectory "<实际设置目录>"`。DLSS-G backend 在启动时锁定，因此必须先做此步骤才能在同一次游戏会话里切换 NR 和 FG。
3. 此本机测试包显式加入用户已有的 `nvngx_dlssnr.dll`，版本 `310.8.0.0`；具体 SHA-256 见 `Batch33-Test/PackageManifest.json`。它仅供本机验证，不要提交、上传或转发含该 DLL 的压缩包。NR 仍可能因运行时初始化、资源约定或硬件条件失败，实际状态请看 `[B33AB]` 日志。DLSS-G 2×/3×/4× 依硬件与运行时支持情况启用。

## 一次游戏会话

1. 进入固定存档和固定场景，记下分辨率、画质档、驱动版本。**Batch33 A/B Lab** 会为两次采样自动开启 Performance Overlay 和 GPU rows。
2. 打开 Upscaling 设置里的 **Batch33 A/B Lab**，按 `Matrix.csv` 的 7 个实验逐项选择。按 **Run A** 后关闭设置菜单，保持游戏非暂停，在固定镜头与场景中运行至少 12 秒。重新打开菜单，确认 A 显示 `captured`，再按 **Run B**、关闭菜单并在同一场景运行至少 12 秒。B 的捕获与 A/B 比较会自动写入 `CommunityShaders.log` 的 `[B33AB] capture` 和 `[B33AB] compare` 行；无需另按捕获键。菜单打开或游戏暂停时 12 秒计时会重置。
3. 查看每组 `cleanPair`、`requestDiff`、实际 NR/FG/REBLUR/Checkerboard 状态、GPU bucket 与 `rawMeanMs`。`cleanPair=true` 要求 A/B 只有一个请求设置差异、两侧都有有效的 10 秒帧统计，并且目标功能确实运行；FG 侧还需要至少 8 个 Present 样本且实测平均呈现倍率接近请求值，NR 侧需要成功渲染帧。若 `cleanPair=false`、功能未实际启用或 MFG 被运行时降档，先记录原因，再调整条件重测；请求设置成功并不代表渲染路径实际运行。
4. 把 `Matrix.csv` 的结果、视觉瑕疵、崩溃及截图文件名填好。NR+FG 是实验组合；尤其检查首帧、开关切换、菜单、地图、分辨率改变后的画面和帧节奏。硬件不支持的 MFG 倍率标记 `SKIP`，不要视为通过。
5. 全部实验结束后、退出游戏前，务必按 Lab 的 **Restore original settings**，避免实验开关被保存进个人配置。退出游戏后，再运行下文预检脚本的 `-Restore`；这两步分别恢复游戏内实验设置和启动前的 backend/upscaleMethod 两个键。

Performance Overlay 的 GPU rows 不一定包含 Streamline 在 `Present` 内部队列执行的全部 FG 负载；记录原生 FPS、实际呈现 FPS 和视觉结果，比较帧节奏时优先使用能观察屏幕呈现时间的工具。

## 退出游戏后收集

```powershell
pwsh -File "<Data>\Batch33-Test\Collect-Batch33Diagnostics.ps1" -DataPath "<Data>"
```

脚本在“文档\Batch33-Diagnostics”输出**一个** ZIP，内含 `CommunityShaders.log`、`[B33AB]` 摘要、用户配置、测试矩阵、预检结果、安装 DLL 的版本与 SHA-256、包清单和 GPU 驱动版本。它只记录 `nvngx_dlssnr.dll` 的元数据，不复制该 DLL。若日志不在标准 `My Games\...\SKSE` 目录，传入 `-LogPath`；若用户设置在模组管理器覆盖目录，传入 `-SettingsDirectory`；若矩阵另存过，传入 `-MatrixPath`。分享 ZIP 前可先检查其中的日志和用户配置。

测试结束后按需恢复预检更改：

```powershell
pwsh -File "<Data>\Batch33-Test\Prepare-Batch33OneLaunch.ps1" -DataPath "<Data>" -Restore
```

恢复模式只恢复预检涉及的两个键；若原本没有用户配置且没有产生其他设置，会移除预检新建的空配置文件。原始逐字节备份会保留供手动恢复。
