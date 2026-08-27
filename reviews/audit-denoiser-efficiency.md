# 降噪器效率审计 — REBLUR 2.73 ms 能省在哪

只读调研，未修改任何项目文件，未构建。分支 `route/ssrt` @ `66055fb74`。
所有结论标注 **【实测】**（从代码/产物里直接读到）或 **【推断】**（模型/推理，需要 A/B 才算数）。

---

## 0. 前提

- **DLSS 光线重建整条作废。** 用户已排除，理由：模型旧、清晰度差、未启用。不再列为候选。
- 抓帧不可用。所有验证动作都设计成「拨一根已有滑条，看一处读数」。
- 用户取向：**愿意用「多打光线」换「少降噪」**。下面的排序照这个取向排。
- 一个必须先说清的分母问题：REBLUR 的**分配**是输出分辨率（`kMAIN` 的 desc，4K = 8.29 MP），**运行**是 `nrd::CommonSettings::rectSize` = `Util::ConvertToDynamic(screenSize)`，即渲染分辨率。你给的「diffuse unpack 约 133 MB/帧@4K」正好等于 16 B/px × 8.29 MP，说明你算的时候按渲染分辨率也是 4K（DLAA）。下面的 ms 估算都按这个走；如果实际是 DLSS Quality（1440p 渲染），所有 ms 数字乘 0.45。

---

## 1. 排序表

| # | 动作 | 省多少 ms @4K | 依据 | 风险 | 改动量 | 怎么验证 |
|---|---|---|---|---|---|---|
| A | **diffuse 链 `Max Stabilized Frames` 30 → 0**，整个 TEMPORAL_STABILIZATION pass 不再被派发 | **0.35 – 0.55** | 【推断】按显存读写量推 | 低－中：diffuse 少一层时域稳定，暗部可能出现轻微「呼吸」。几何类信号不受影响（这个 pass 只处理辐射度） | **0**（滑条已有） | 拨 SSRT → REBLUR Diffuse (advanced) → Max Stabilized Frames 到 0，等 5 秒让历史重建，读 overlay 的 SSRT REBLUR 行 |
| B | **A 之后**：`NRD::PrepareGuides` 里那次全资源 MV `CopyResource` 变成死代码，删掉并把游戏的 MV SRV 直接接到 `IN_MV` | 0.10 – 0.17 | 【推断】按显存读写量推（4 B/px 读 + 4 B/px 写 @ 4K = 66 MB/帧） | 极低 | 小（NRD.cpp 删 4 行、改 1 处绑定；`texNRDMV` 可整个删掉，省 33 MB 显存） | 看 overlay 的 **NRD Guides** 行（它是独立 bucket），不是 REBLUR 行 |
| C | **两条链的 `Min/Max Blur Radius` 压小**（例如 diffuse 30 → 8，specular 30 → 12；Min 1 → 0） | 0.4 – 0.9 | 【推断】8 抽 Poisson 的 cache 局部性，非线性 | 中：这是 REBLUR 唯一的空间降噪手段，压太狠会把噪点直接放到屏幕上——但这正是「多打光线换少降噪」要付的账 | **0**（滑条已有） | 极端点先测上限：Min = Max = 0（此时 8 个 tap 全落在中心像素，退化成 cache 命中）。这一读数直接告出 Blur+PostBlur+PrePass 在 2.73 ms 里占多少 |
| D | **specular 链 `Specular Pre-pass Radius` 50 → 0**，PREPASS pass 不再被派发 | 0.30 – 0.50 | 【推断】按 8 抽 Poisson 全屏 pass 推 | **中－高**：`UsePrepassOnlyForSpecularMotionEstimation = true`，这个 pass 现在只用来估虚拟运动。关了会让反射的重投影变差（拖影/抖动），这正是当初从 SVGF 换到 REBLUR 要修的缺陷 | **0**（滑条已有） | 拨 SSRT → REBLUR Specular (advanced) → Specular Pre-pass Radius 到 0，读 REBLUR 行；同时在有反射的水面/湿地上目视看拖影 |
| E | **吸收 diffuse unpack**：让 `ssrt_diffuse_composite.hlsl` 直接读 `texNRDPackOutput` 并内联 YCoCg 反变换 | 0.13 – 0.15 | 【推断】按显存读写量推（133 MB/帧 @4K） | 低 | 小－中（一个 shader permutation + 一处绑定切换 + Buffer Viewer 那条目会显示过期内容） | 读 REBLUR 行（unpack 在同一个 bucket 内）。需要 bump SSRT ini 到 1-3-1，否则跑的是旧字节码 |
| F | **diffuse 半分辨率降噪 + 双边上采样**（rectSize 减半 + 半分辨率 guide + 上采样代替 unpack） | 0.8 – 1.1 | 【推断】6 个 pass 的纹素数 ×1/4 | **高**：4K 渲染下半分辨率是 1080p 的 GI 细节；接触暗部会明显变糊。而且要动 MV 的缩放约定，弄错会静默地让重投影全错 | 大 | 只能靠游戏内 A/B 看图，没有离线手段能证明画质。**建议放在 A–E 都做完、还嫌贵的时候再考虑** |

**不成立的三条**（否证部分，见第 2 节）：棋盘格没传给 NRD（不是 bug，是刻意的，而且传了会更贵）；镜面链有个 stabilization pass 在白跑（没有，NRD 已经跳过了）；SVGF 那条路留着要钱（不要 ms，只要磁盘和源码）。

---

## 2. 逐条

### 第 2 条：棋盘格没告诉 NRD —— **不成立**（既不是 bug，也不是性能损失）

**【实测】** `ScreenSpaceRayTracing.cpp:3061` 确实硬编码 `nrd::CheckerboardMode::OFF`：

```
nrdSvc.ApplyReblurSettings(reblurNative, reblurUI, nrd::CheckerboardMode::OFF);
```

但这不是漏传，因为 **REBLUR 根本没见到棋盘格数据**：

**【实测】** 稀疏解析在降噪之前就把信号还原成满分辨率了。`ScreenSpaceRayTracing.cpp:3433-3464` 的 sparse resolve 派发在 `RunReblur` 之前（`3702`），把 `texSparseColor`（半宽）展开成满分辨率的 `texNRDPackInput`。`ssrt_common.hlsli:515-517` 明确写着这是设计意图：

> What is deliberately *not* affected: the denoisers. Both modes resolve back to full resolution before anything else in the chain runs.

**【实测】** 而且「NRD 在棋盘模式下内部会少做一部分工作」是反的。`extern/NRD/Source/Reblur.cpp:118-120`：

```
bool enableHitDistanceReconstruction = ... && settings.checkerboardMode == CheckerboardMode::OFF;
bool skipPrePass = (...) && (...) && settings.checkerboardMode == CheckerboardMode::OFF;
```

打开 checkerboard 会让 `skipPrePass` 变成 false，**给 diffuse 链多加一个全屏 8 抽 Poisson 的 PREPASS pass**（现在这个 pass 是跳过的，因为 `NRD.cpp:374` 把 `diffusePrepassBlurRadius` 钉死为 0）。TemporalAccumulation / HistoryFix / Blur / PostBlur 的 extent 一个都不变——它们全都按 `rectSize` 跑。

**【实测】** 这件事 batch 12 的作者已经查过并记录在 `ssrt_sparse_resolve.hlsl:6-31` 的文件头注释里，连 `Reblur.cpp:117/119` 的行号都引了。所以这不是遗漏，是有档的决定。

**布局上也是对得上的，可惜没用**：`texSparseColor` 是半宽全高的 RGBA16F（`ScreenSpaceRayTracing.cpp:1671, 1703`），而 NRD 要求棋盘输入「tightly packed to the LEFT HALF of the texture, the input pixel = 2x1 screen pixel」（`NRDSettings.h:42-44`）——正好一致。相位也是同一族：我们的是 `(x ^ y ^ (FrameCount&1)) & 1`（`ssrt_common.hlsli:569-586`），NRD 的是 `Sequence::CheckerBoard(pos, gFrameIndex)`。所以技术上可以接，但接了只会更贵。

**Half Resolution 档同理**：NRD 那侧完全不知道，也不需要知道——它拿到的一样是满分辨率还原后的信号。

**顺手记一个不相关的隐患【实测】**：`SSRT_SparseCheckerPhase()` 读 `SharedData::FrameCount`，而 `State.cpp:791` 是 `data.FrameCount = frameCount * temporal`。关掉 AA/上采样时 `temporal = 0`，FrameCount 恒为 0，棋盘相位就冻住了，一半像素永远不追。你开着 DLSS 所以 `temporal = 1`，当前配置不受影响。

---

### 第 3 条：2.73 ms 的 pass 级构成

**【实测】这个 bucket 装的是什么**：`GpuTimers.h:33` —— `SSRTReblur` = 「NRD REBLUR dispatches + back-end unpack (diffuse and specular)」。也就是 2.73 ms 里包含：diffuse 的 NRD pass 组 + diffuse unpack + specular 的 NRD pass 组 + specular unpack。guide 那一趟是独立 bucket（`NRDGuides`），不在里面。

**【实测】创建了几个 denoiser**：两个独立实例，各自一个 `nrd::Instance`（`ScreenSpaceRayTracing.cpp:1821-1823`）：

```
nrdReblurDiffuse.Init(mainDesc.Width, mainDesc.Height, nrd::Denoiser::REBLUR_DIFFUSE, 0);
nrdReblurSpecular.Init(mainDesc.Width, mainDesc.Height, nrd::Denoiser::REBLUR_SPECULAR, 1);
```

**【实测】当前设置下实际派发的 pass**（从 `extern/NRD/Source/Reblur.cpp:112-190` 的调度逻辑，配合本仓库的实参逐条代入）：

| | diffuse 实例 | specular 实例 |
|---|---|---|
| CLASSIFY_TILES | 跑 | 跑 |
| HITDIST_RECONSTRUCTION | 跳（`HitDistanceReconstructionMode = 0`，`NRD.h:114`） | 跳 |
| PREPASS | **跳**（`NRD.cpp:374` 钉死 `diffusePrepassBlurRadius = 0`） | **跑**（`SpecularPrepassBlurRadius = 50`，`ScreenSpaceRayTracing.h:459`） |
| TEMPORAL_ACCUMULATION | 跑 | 跑 |
| HISTORY_FIX | 跑（但内部 20 抽循环基本不执行，见下） | 跑（同样，且更彻底） |
| BLUR | 跑 | 跑 |
| POST_BLUR | 跑 | 跑 |
| TEMPORAL_STABILIZATION | **跑** | **跳**（`MaxStabilizedFrameNum = 0`，`ScreenSpaceRayTracing.h:441`） |
| SPLIT_SCREEN / VALIDATION | 跳 | 跳 |
| 合计 | **6 个 dispatch + 1 unpack** | **6 个 dispatch + 1 unpack** |

#### 「镜面链 MaxStabilizedFrameNum = 0 是不是有 pass 在白跑」——**不成立**

**【实测】** `Reblur.cpp:118` 算出 `skipTemporalStabilization = (settings.maxStabilizedFrameNum == 0)`，`Reblur.cpp:174-179` 就据此整段不 `PushDispatch`。而且 `Reblur.cpp:167-172` 会切到 POST_BLUR 的另一个 permutation（`TEMPORAL_STABILIZATION = 0`），由 PostBlur 自己补写 `OUT_SPEC_RADIANCE_HITDIST` 和 `PREV_INTERNAL_DATA`（`Reblur_Specular.hpp:207-210`）。所以镜面链没有白跑的 pass。

**但有白占的显存【实测】**：`Reblur_Specular.hpp:25-26, 36-37` 无条件把 `SPEC_HISTORY_STABILIZED_PING/PONG`（各 R16_SFLOAT）加进 permanent pool，即使 stabilization 永不运行。4 B/px，4K 分配下 **33 MB 白占**。要回收得改 `extern/NRD`，不值当。

#### 反过来，diffuse 链的 stabilization 是**真在跑**，而这是最干净的一刀

**【实测】** `NRD.h:102` 的默认值是 `MaxStabilizedFrameNum = nrd::REBLUR_MAX_HISTORY_FRAME_NUM`（= 63，`NRDSettings.h:199`），`ScreenSpaceRayTracing.h:434` 的 `ReblurDiffuse` 用的就是这个默认。`NRD.cpp:369` 夹到 `min(63, MaxAccumulatedFrameNum = 30) = 30`。非零 → pass 派发。

**【实测】这个 pass 干了什么**（`Reblur_Diffuse.hpp:206-227`）：读 TILES / NR / PREV_VIEWZ / DATA1 / DATA2 / DIFF_HISTORY / STAB_PING（catrom 抽样），写 **IN_MV / PREV_INTERNAL_DATA / OUT_DIFF_RADIANCE_HITDIST / STAB_PONG**，即 4 B + 2 B + 8 B + 2 B = 16 B/px 的写 + 约 25 B/px 的读。

**【推断】** 4K 下这一个 pass 约 340 MB/帧的显存读写，按整个 bucket 的 DRAM 流量占比推，约 **0.35–0.55 ms**。关它只需要把滑条拨到 0，没有代码改动、没有 shader 重编、不用 bump ini。

**【实测】连带效果——MV 那次 CopyResource 变成死的**：`Reblur_Diffuse.hpp:223` 和 `Reblur_Specular.hpp:235` 是 `IN_MV` 唯一被当输出用的地方，两处都在 "Temporal stabilization" pass 里。`NRD.cpp:284` 之所以要

```
context->CopyResource(texNRDMV->resource.get(), motion.texture);
```

正是因为注释写的「Motion Vector is used as both SRV and UAV by ReBLUR」（`NRD.cpp:282-283`）。两条链的 stabilization 都关掉之后，**没有任何 dispatch 会把 IN_MV 声明成 STORAGE_TEXTURE**，`NRDReblurIntegration.cpp:241-261` 的绑定校验也就不会要求它——可以直接把游戏的 `motion.SRV` 接到 `IN_MV`，删掉 `texNRDMV` 和那次全资源拷贝。4 B/px 读 + 4 B/px 写 = **66 MB/帧**，外加 33 MB 显存。

#### HISTORY_FIX：**基本已经是免费的**，不用管

**【实测】** `REBLUR_HistoryFix.cs.hlsl:81` —— `stride *= float2( frameNum < gHistoryFixFrameNum )`，`:114` —— `if( diffStride != 0.0 )` 才进那个 20 抽的重建循环。

- diffuse：`NRD.cpp:370` 算出 `historyFixFrameNum = min(3, maxFast(6) - 1) = 3`。稳态下绝大多数像素 `frameNum >= 3`，循环不执行。
- specular：`maxFast = 1` → `min(3, 0) = 0` → `frameNum < 0` 恒假 → **循环永不执行**。

所以 HISTORY_FIX 剩下的就是 LDS 里的 fast-history clamp，不额外吃带宽。把 `HistoryFixFrameNum` 调 0 省的是零头。

#### anti-firefly：**关掉几乎不省**

**【实测】** `NRD.cpp:381` 硬编码 `out.enableAntiFirefly = true`（UI 里没有开关）。但 `REBLUR_HistoryFix.cs.hlsl:219-246` 那个累加循环是 `NRD_SUPPORTS_ANTIFIREFLY` 编译期决定的、**无条件**跑的；`gAntiFirefly` 只控制最后 4 行的一个 clamp（`:249-260`）。数据全部来自 LDS 里 `s_DiffLuma` 的预载 tile，没有额外纹理读。关它只省几条 ALU。

#### `REBLUR_PERFORMANCE_MODE`：**已经开了**

**【实测】** `CMakeLists.txt:90` —— `set(REBLUR_PERFORMANCE_MODE ON CACHE BOOL "" FORCE)`；`build/ALL/CMakeCache.txt:429` —— `REBLUR_PERFORMANCE_MODE:BOOL=ON`；生成的 `extern/NRD/Shaders/NRDConfig.hlsli:9` —— `#define REBLUR_PERFORMANCE_MODE 1`。这根杠杆已经拉到底了，没有剩余空间。（顺带：它把 `REBLUR_ANTI_FIREFLY_FILTER_RADIUS` 从 4 降到 3，`REBLUR_Config.hlsli:246-247`，也就是 NRD_BORDER 小一圈、LDS tile 小一圈。）

#### 谁最贵：三个 8 抽 Poisson gather

**【实测】** `REBLUR_Common_SpatialFilter.hlsli:178-280` 的 tap 循环是 `[unroll]`、**无条件** `POISSON_SAMPLE_NUM = 8` 次（`REBLUR_Config.hlsli:70-73`），每 tap 读 3 张纹理：viewZ（R32，4 B）+ normal_roughness（R10G10B10A2，4 B）+ 辐射度（RGBA16F，8 B）= **16 B/tap**，即 128 B/px 的 gather。

这样的 pass 有三个：diffuse 的 BLUR + POST_BLUR，specular 的 PREPASS + BLUR + POST_BLUR（共五个）。半径还在放大：`REBLUR_POST_BLUR_RADIUS_SCALE = 2.0`（`REBLUR_Config.hlsli:85`），所以 PostBlur 的足迹是 `MaxBlurRadius × 2 = 60 px`。

**【推断】** 这五个 pass 合计占 2.73 ms 的 **50–60%，约 1.4–1.6 ms**。它们不是 DRAM 带宽瓶颈（相邻线程足迹重叠很多，真实 DRAM 流量远低于 128 B/px），而是**取样率 / L2 局部性**瓶颈——所以有效的杠杆是**缩小半径**（提高 cache 命中）或**减少 tap 数**，不是省字节。

**怎么把这个 50–60% 从推断变成实测（不用抓帧）**：把 diffuse 的 `Min Blur Radius` 和 `Max Blur Radius` 两根滑条都拨到 0。此时 `REBLUR_Common_SpatialFilter.hlsli:118-120` 算出 `blurRadius = 0`，`:156` 的 `skew = 0`，8 个 tap 全部落在中心像素同一个地址上 → 循环还在跑、pass 数不变、但 gather 全变 cache 命中。REBLUR 行的降幅就是 diffuse BLUR+POST_BLUR 的 gather 成本。specular 那三个同理再测一次。**这是整份报告里最值钱的一个 A/B**，因为它一次性把 pass 级归因锁死。

#### 创建了但没人消费的东西【实测】

- **pipeline**：`NRDReblurIntegration.cpp:423-430` 给 `instanceDesc.pipelinesNum` 里**每一个** permutation 都建 `ID3D11ComputeShader`。REBLUR_DIFFUSE 一共 22 个（1 + 4 + 2 + 8 + 1 + 1 + 2 + 1 + 1 + 1），实际只用 6 个。这是一次性的 CPU/驱动开销，不吃帧时间，不值得动。
- **pool 纹理**：permanent + transient 全建。diffuse 36 B/px、specular 45 B/px（按 `Reblur_Diffuse.hpp:29-49` / `Reblur_Specular.hpp:31-56` 的格式表逐条算），合计 **81 B/px**。加上 `texNRDPackInput/Output`（16 B/px）和三张 guide（12 B/px），REBLUR 这条路 4K 分配下约 **900 MB 显存**。其中确定白占的只有前面说的镜面 stabilized ping/pong 33 MB，加上关掉 diffuse stabilization 后可回收的 `texNRDMV` 33 MB。

---

### 第 4 条：降噪跑在什么分辨率上 —— **满分辨率，这就是用户观察的直接来源**

**【实测】**
- 实例分配：`ScreenSpaceRayTracing.cpp:1821-1823`，用 `kMAIN` 的 `mainDesc.Width/Height`，即输出分辨率。
- 运行 extent：`NRD.cpp:314-315` 把 `commonSettings.rectSize` 设成 `floor(ConvertToDynamic(screenSize))`，即渲染分辨率。REBLUR 的每个 pass 都按 `rectSize` 派发。
- unpack 派发：`ScreenSpaceRayTracing.cpp:3003-3004`，同样是渲染分辨率。
- 追踪那一侧：Checkerboard 档只追一半密度（`ScreenSpaceRayTracing.cpp:3215-3218`），但 `3433-3464` 的 sparse resolve 在降噪之前就还原成满分辨率了。

所以**追踪可以稀疏，降噪一直是满的**——这正是「降噪的边际成本超过多打光线」的机制。定量一下：diffuse SPP 2 → 4 大约给「SSRT 追踪合计 1.10 ms」再加 0.5–0.6 ms【推断】，而降噪那边一分不省。REBLUR 是 2.73 ms。用户的判断是对的。

**半分辨率降噪的可行性【推断】**：技术上不需要第二套 guide 分配——NRD 的 `resourceSize` / `rectSize` 就是动态分辨率机制，而且 `NRD_SUPPORTS_VIEWPORT_OFFSET = 0`（`NRDConfig.hlsli:4`）意味着 rect origin 恒为 0，所以「半分辨率数据写在现有满尺寸 guide 纹理的左上角」是合法的，不用新建纹理。要动的是四处：

1. `NRD::PrepareGuides`（`NRD.cpp:216-290`）改成写半分辨率的 viewZ / normal-roughness 到左上角（顺便这一趟自己也变便宜）。
2. `NRD::GetCommonSettings`（`NRD.cpp:314-317`）的 `rectSize` / `rectSizePrev` 减半。
3. `commonSettings.motionVectorScale`（`NRD.cpp:331-333`，现在是 `{1,1,0}`）——**这是最容易静默弄错的一处**。MV 的单位约定改了 rect 之后必须跟着改，弄错的话重投影会全错而画面看起来「只是有点糊」。
4. `ssrt_nrd_unpack.hlsl` 从 1:1 反变换改成深度/法线引导的双边上采样。

代价：diffuse 六个 pass 的纹素数 ×1/4。**只对 diffuse 做**——specular 半分辨率降噪会直接毁掉反射锐度，和 `ssrt_common.hlsli:521-523` 已经写死的「sparse specular 不做」同一个理由。

**这条我建议排在最后**：没有任何离线手段能证明它的画质，验证成本全落在游戏内 A/B 上，而 A–E 加起来的收益已经和它同一个量级、风险低得多。

---

### 第 5 条：diffuse unpack 的吸收

**【实测】现状**：`RunReblur` 的 unpack（`ScreenSpaceRayTracing.cpp:3113-3122`）是一个全屏 dispatch，`ssrt_nrd_unpack.hlsl` 读 RGBA16F 写 RGBA16F，16 B/px = **133 MB/帧 @4K**，和你给的数字一致。

**【实测】唯一的 REBLUR 侧消费者**：`ScreenSpaceRayTracing.cpp:3785` —— diffuse composite 的 t0。而 `ssrt_diffuse_composite.hlsl:336, 354` 只用 `[dispatchID.xy]` 的 `.rgb`，`.w` 明确不读。其余引用 `texSSRTDiffuseColor` 的地方（`3501, 3517, 3552, 3645-3684`）全在 SVGF 分支里；`3707` 那次 `CopyDynamicRegion` 被 `SvgfHistoryNeeded(false)` 挡住（`ScreenSpaceRayTracing.h:1317`），REBLUR 下不执行。

**具体改法（最小改动）**：

1. `ssrt_diffuse_composite.hlsl` 加一个编译期 permutation（例如 `SSRT_NRD_PACKED`），在 `:336` 之后插一句 `REBLUR_BackEnd_UnpackRadianceAndNormHitDist`。用 define 而不是 cbuffer 字段，就不动任何现有 permutation 的 reflection chunk。
2. `ScreenSpaceRayTracing.cpp:3785` 的 t0 改成绑 `RunReblur` 的返回值决定的那张：成功绑 `texNRDPackOutput`，失败绑 `texNRDPackInput`。这一步不需要 shader 改动——`RunReblur` 现在已经在做同样的选择（`:3114`），只是把选择结果往下传一层。
3. 删掉 `RunReblur` 里 diffuse 分支的 unpack dispatch。specular 分支保持不动。
4. composite permutation 从 2 个（`diffuseCompositeCS` / `diffuseCompositeExternalConfCS`）变 4 个。
5. **必须 bump `features/Screen Space Ray Tracing/Shaders/Features/ScreenSpaceRayTracing.ini` 到 1-3-1**，否则磁盘缓存不失效。

**代价**：`texSSRTDiffuseColor` 在 REBLUR 下不再被写，Buffer Viewer 那一条（`ScreenSpaceRayTracing.cpp:840`）显示过期内容。这是你已经接受的。顺带：REBLUR-only 的会话里这张 66 MB 的 RGBA16F 也可以延迟分配，但它是 SVGF 链的输出目标，改动面会大一圈，不建议一起做。

**specular unpack 确认堵死【实测】**：`Deferred.cpp:576` 在固定 SRV 数组的 t16 位置直接绑 `ssrt.texSSRColor->srv.get()`，`DeferredCompositeCS` 是跨特性的共享 shader。要吸收就得在那个 shader 里加分支，牵动的 permutation 面太大。（你笔记里写的是 `Deferred.cpp:524`，当前树上是 **576**。）

---

### 第 6 条：SVGF 那条路还值不值得留 —— **留着，但不是因为它便宜，而是因为它已经不要钱**

**【实测】** `ResolveDenoisers`（`ScreenSpaceRayTracing.cpp:1928-1930`）只在「用户选了 SVGF」或「选了 REBLUR 但某条链起不来」时才 `EnsureSvgfResources()`。batch 11 item C1 已经把 `|| bufferViewerActive` 这一项删了，注释（`:1923-1927`）记着原来那十张缓冲是 68 B/px、4K 下 537.9 MiB。

所以在你的配置（REBLUR 正常工作）下，SVGF 的成本是：

- **GPU 帧时间：0**（一个 dispatch 都不派发）
- **显存：0**（一张都不分配）
- 唯一的实际成本：`ScreenSpaceRayTracing.cpp:2026-2029` 无条件编译那六个 SVGF compute shader，占的是启动时间和磁盘缓存。

而它是 VR / NRD 未安装 / NRD 被关 / NRD 实例起不来这四种情况唯一的兜底（`:1948-1959`）——按项目「任何设置组合必须独立工作」的规则，退役它就得接受这些情况下屏幕上是生噪。

**结论：退役 SVGF 一毫秒都不省。不成立。**

---

## 3. 用户想走的路线：REBLUR 调到最轻 + spp 提上去

**【实测】** `DiffuseSPP` 默认 2，滑条 1–16（`ScreenSpaceRayTracing.cpp:141`），是编译期宏 `DIFFUSE_SPP`（`:2068`），改它会触发重编（`:2337` 比对 `compiledDiffuseSPP`）。

**具体该动哪几个参数**，按「改一处、测一处」的顺序：

**第 1 步 — 免费的两刀（纯滑条，无重编）**

| 参数 | 位置 | 从 → 到 | 效果 |
|---|---|---|---|
| Max Stabilized Frames（diffuse） | SSRT → REBLUR Diffuse (advanced) | 30 → **0** | 整个 TEMPORAL_STABILIZATION pass 不派发 |
| Max Blur Radius（diffuse） | 同上 | 30 → **8**（先用 0 测上限） | BLUR / POST_BLUR 的 gather 足迹缩小，cache 命中大幅上升 |
| Min Blur Radius（diffuse） | 同上 | 1 → **0** | 让收敛像素的 blurRadius 真的能到 0 |

注意：`ScreenSpaceRayTracing.cpp:493-496` 里任何 REBLUR 参数变动都会同时置 `resetReblurDiffuse` 和 `resetReblurSpecular`，历史全清。**每次拨完等 5 秒再读数**，否则读到的是重建期。

**第 2 步 — 用省下来的预算买光线**

`DiffuseSPP` 2 → 4。这会触发一次 shader 重编（有停顿，正常）。追踪那边约 +0.5–0.6 ms【推断】，第 1 步省下来的应该够付。

**第 3 步 — 判断噪点是否可接受，再决定要不要继续压**

如果 SPP 4 + 轻降噪的画面比 SPP 2 + 重降噪更干净（这是有可能的：REBLUR 的 Blur/PostBlur 是在拿分辨率换方差，而 SPP 是直接降方差），就继续：Max Blur Radius 再往 4 压，或者 specular 那边也做同一组（Max Stabilized 已经是 0 了，只需要动 blur 半径和 Pre-pass Radius）。

**明确不要动的**：

- `Max Accumulated Frames` / `Max Fast Accumulated Frames`：**这两个不省任何 ms**。pass 数和 extent 都不变，它们只改 cbuffer 里的两个浮点。压小它们只会让噪点更多而不省钱——这是纯亏。【实测：`Reblur.cpp:112-190` 的调度逻辑不读这两个值；`REBLUR_HistoryFix.cs.hlsl:123` 处 `gMaxAccumulatedFrameNum` 只参与权重计算】
- `History Fix Frame Num` / `History Fix Pixel Stride`：见上，那个循环稳态下本来就不跑。
- anti-firefly：UI 里没有，而且关了也几乎不省。
- `Hit Distance Reconstruction`：现在是 OFF，**开它会加一个全屏 pass**（`Reblur.cpp:135-142`）。别碰。

**一个反直觉的提醒**：用户的取向是「多打光线换少降噪」，但在这套代码里，「少降噪」能省钱的只有**空间**部分（blur 半径、pass 数），**时域**部分（累积帧数）省不了钱。所以路线是「砍空间滤波 + 提 spp」，不是「砍历史 + 提 spp」。砍历史纯亏。

---

## 4. 时间估算的方法说明

- 第 A / B / E 项：**按显存读写量推**。逐 pass 从 `Reblur_Diffuse.hpp` / `Reblur_Specular.hpp` 的 PushInput/PushOutput 列表和 `Reblur.cpp` 的格式宏算出 B/px，乘 8.29 MP，再按整个 bucket 的 DRAM 流量占比线性分摊 2.73 ms。这个模型对「点对点读写」的 pass（stabilization、unpack、MV copy）比较可靠。
- 第 C / D / F 项：**不是纯带宽模型**。8 抽 Poisson 的真实 DRAM 流量远低于 128 B/px（相邻线程足迹重叠），瓶颈在取样率和 L2 局部性，非线性。所以给的是区间，而且明确标了要靠 Min/Max Blur Radius = 0 那个 A/B 才能定标。
- 全部按渲染分辨率 = 4K（8.29 MP）算。这个分母来自你给的「diffuse unpack 133 MB/帧」正好等于 16 B/px × 8.29 MP。**如果实际是 DLSS Quality（1440p 渲染），所有 ms 数字乘 0.45**——这个只能查 ini 确认，不能从帧数据反推。

---

## 5. 如果只能做一件事

**把 diffuse 的 Min Blur Radius 和 Max Blur Radius 都拨到 0，读一次 SSRT REBLUR 行。**

不是因为这是最终配置（0 半径不是能出货的画质），而是因为这一个动作同时完成两件事：它是**免费的**（滑条已有，无代码、无重编、无 ini bump），而且它把「2.73 ms 里有多少是五个 8 抽 Poisson gather」这个问题从推断变成实测。上面 A–F 六项里有四项的价值排序都取决于这个读数——它要是只降 0.3 ms，那第 C / D / F 项就都该往后放，第 A 项（关 diffuse stabilization）就是唯一值得做的；它要是降 1.2 ms 以上，那「砍空间滤波 + 提 spp」这条路线整个成立，可以直接照第 3 节走下去。

一根滑条，一个读数，锁死后面所有决定。
