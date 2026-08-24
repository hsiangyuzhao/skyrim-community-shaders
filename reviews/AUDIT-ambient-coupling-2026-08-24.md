# 环境光通道耦合审计 — IBL / VanillaFresnel / SSGI / SSRT

日期 2026-08-24。分支 `route/ssrt`。
目的：拆清四个特性在环境光上的相互影响，定下一个"最符合物理"的配置，并评估更便宜的近似替代品。

标注约定：**【证】** = 代码可验证的事实，附行号；**【推】** = 强推断，链条完整但需一次观察定案；**【测】** = 尚未验证，需要测量。

---

## 0. 一句话结论

用户报告的四个症状不是四个 bug，是**同一个结构性缺陷**的四个投影：

> **环境光的"移除"是一个全局无条件的常数乘法，而"补回"是一个逐像素有条件的屏幕空间通道。任何补不回来的像素，其环境光被直接删掉，没有任何回退。**

因此"最正确的配置"不是一组口味选择，而是被这个缺陷唯一确定的：**必须开 Ambient Reinjection**，因为它是唯一让移除与补回同源同权的模式。

---

## 1. 通道地图：谁在哪一步写/读什么

前向阶段（`Lighting.hlsl`，每个材质各跑一次）：

| 步骤 | 行 | 内容 |
|---|---|---|
| 组装 DALC | `:3217` | `directionalAmbientColor = Color::Ambient(DirectionalAmbient · N)` |
| IBL 缩放 DALC | `:3224` | `*= DALCAmount`（默认 0.33） |
| IBL 探针叠加 | `:3249-3252` | `+= Saturation(GetIBLColor(-N)) * DiffuseIBLScale` |
| **SSRT 全局归零** | `:3259` | `*= ssrtSettings.AmbientMult` ← **全局 uniform** |
| 加进画面 | `:3275` | `diffuseColor += directionalAmbientColor` |
| 乘 albedo | `:3636` | `directionalAmbientColor *= outputAlbedo` |
| 写 G-buffer | `:3894` | `psout.Masks.z = RGBToYCoCg(directionalAmbientColor).x` ← 只存**亮度** |

合成阶段（`DeferredCompositeCS.hlsl`，逐像素一次）：

| 步骤 | 行 | 内容 |
|---|---|---|
| 解析重建 A 的色度 | `:266-302` | 必须**逐字复刻**前向的构造，否则残留加性偏色 |
| 亮度从 G-buffer 取回 | `:304-307` | `YCoCg.x = Masks.z` |
| 决定保留比例 | `:346-353` | reinjection：`ambientKeep = 1 - conf * strength`；否则 `A = 0` |
| 减掉 | `:416` | `diffuseColor -= A` |
| 回加 | `:445` | `+= A' * multiBounceAO * ambientKeep` |
| SSGI IL | `:467` | SSRT diffuse 开着就跳过（避免重复计算） |
| SSGI DE | `:384-413` | SSRT diffuse 开着就跳过；只换**回加项**的色源 |

SSRT diffuse 的辐射注入（`ssrt_diffuse_composite.hlsl:355`，跑在合成之前）：

```hlsl
color = IrradianceToGamma(ssrtDiffuse.xyz * IrradianceToLinear(albedo.xyz) + IrradianceToLinear(originalColor.xyz));
```

---

## 2. 缺陷的证明

**【证】** `AmbientMult` 默认值为 **0**（`ScreenSpaceRayTracing.h:166`）。

**【证】** 它在 `Lighting.hlsl:3259` 作为**全局常量**乘到每个材质的 `directionalAmbientColor` 上。它不知道、也无法知道这个像素的替代光源是否会到达。

**【证】** 于是 fallback 模式（reinjection 关）下：
1. 全场景每个材质的前向环境光 → 0；
2. `Masks.z` → 0；
3. 合成端 `:351` 把 A 显式置 0，减法和回加都不发生；
4. 唯一把光放回去的通道是 `ssrt_diffuse_composite.hlsl:355` 的 `ssrtDiffuse × albedo`。

**这一步是逐像素的、依赖屏幕空间追踪成败与 G-buffer albedo 的。** 凡是它落不上的像素，环境光就是净损失——不是"变化不明显"，是**被删掉且无人补回**。

**【证】** reinjection 模式（`:346-349`）：`AmbientMult` 被钉到 1（UI 层 `ScreenSpaceRayTracing.cpp:154-162` 禁用该滑条并说明原因），前向环境光完整留在画面里；合成端只按 `ambientKeep = 1 - conf * strength` 扣除，`conf` 正是光线自己报告的覆盖率。恒等式（`:333-336` 的注释即此推导）：

```
MAIN = direct + A + Σ conf_i · L_i / N
out  = direct + (1-conf) · A + conf · L̄  =  direct + lerp(A, L̄, conf)
```

**移除与补回同源同权，没有方向被算两遍、也没有方向被丢掉。这是唯一在能量上自洽的模式。**

---

## 3. 对上用户的四个症状

### 症状 1：fallback 影响不到眼睛/头发，却能影响金属门框

**【证】** 机制不是"SSRT 影响不到它们"，而是：它们的环境光被 `AmbientMult = 0` 全局删掉了，而 `ssrtDiffuse × albedo` 这条补回通道没落到它们身上。金属门框是普通不透明 ENVMAP 材质，补回通道落得上，所以它会变。

**【测】** 补回通道对眼睛/头发具体败在哪一步（G-buffer albedo？屏幕空间追踪在凸面/alpha-test 抖动几何上失败？还是它们根本不在延迟窗口内 —— `Deferred.cpp:806` StartDeferred 到 `:827` EndDeferred 之间）尚未定案。**但这不影响结论**：无论败在哪一步，"全局删除 + 条件补回"这个结构必然产生这类漏洞，修法是让删除也变成条件性的，即 reinjection。

> 一个便宜的定案办法：Buffer Viewer 看眼睛像素的 Albedo 与 Masks.z 两个通道，在 VanillaFresnel 开/关下各看一次。

### 症状 2：reinjection 修好了亮度，但 IBL 的任何错误都会污染画面

**【证】** 这是直接的结构后果，不是巧合。reinjection 模式下留在画面里的是 `(1 - conf) · A`，而 `A = DALC × DALCAmount + IBL探针`（`DeferredCompositeCS.hlsl:275-296`）。开阔地的 `conf` 远达不到 1，所以 **IBL 的任何偏色都按 `(1-conf)` 的权重进入每一个像素**。

⇒ **IBL 的正确性从"锦上添花"变成了 SSRT 路径的前置条件。** 批次 5 的 env 减法修复、以及排队中的 sky/env 独立滑条与饱和度，都在关键路径上，不是可选项。

### 症状 2b：金属门框被原始 DDS 影响

**【证】** 见 `AUDIT` 前置分析：关掉 auto cubemap conversion 时，`Lighting.hlsl:2504` 走进 `if (!dynamicCubemap)`，按方向采样**原版烘焙的静态 cubemap**，并在 `:3568` 以 `envColor * IrradianceToLinear(diffuseColor)` 加入。三条物理错误：

1. 反射的是一张与所处位置无关的烘焙贴图；
2. 不随天气/时辰变化（**已由用户实测确认：蓝条夜里仍在**）；
3. `:3568` 把反射**乘上了接收面自身的漫反射光照**——暗房间里的镜子照样反射亮窗户，这个乘法没有物理意义。

⇒ auto cubemap conversion **必须开**。开启后 `:2446` 在 mip 15 取 cubemap 平均色当 `F0`（反射率是材质属性，角色正确），真正的反射交给动态 cubemap。

### 症状 3：VanillaFresnel 的 bug

**【证】** `Lighting.hlsl:3302-3306`：

```hlsl
#	if (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE))
#		if defined(VANILLA_FRESNEL)
	if (!enableVanillaFresnel)
#		endif
	reflectance *= envMask;
#	endif
```

**VanillaFresnel 一开，`reflectance *= envMask` 就被整条跳过。** 而 `envMask = EnvmapData.x * MaterialData.x`（再乘 envmask 贴图或 glossiness，`:2396-2402`）正是原版用来表达"这个材质该反射多少"的那个旋钮。跳过它 = 每个 ENVMAP/EYE 材质的环境反射按**满强度**输出。

**【推】** 眼睛尤其严重且夜里发光，两个放大器：
- 眼睛的 `F0` 被硬写成 0.027、`roughness` 硬写成 0.1（`:2496-2499`），是个近镜面球体，边缘 Fresnel → 1，满强度反射直接拉满；
- 夜间放大器在 `DeferredCompositeCS.hlsl:539-541`（及 `DynamicCubemaps.hlsli:195-197`）：cubemap 除以自身 mip15 亮度再乘 DALC 亮度，分母有 `max(…, 0.001)` 地板。夜里两者同趋零，比值可以爆掉。

定案观察：同一场景夜间，只切 VanillaFresnel 开/关，看眼睛反射是否恢复正常。

**注意**：`reflectance` 在 DEFERRED 下是 **[0,1] 的 split-sum BRDF 权重**（`DynamicCubemaps.hlsli:166` 返回 `horizon * (F0 * A + B)`，不是辐射亮度），所以 `:3616` 的 `outputAlbedo *= 1 - reflectance` 是**正确的能量守恒**，不是缺陷。但它继承了同一个未衰减的 `reflectance`，所以在眼睛边缘会把 albedo 压到接近 0。

### 症状 4：SSGI DE 也影响不到眼睛/头发

**【证】** DE 只替换回加项 `ambientReAddColor`（`:412`），同样 `× albedo × maxScale`；且被 `!(DiffuseMult > 0)`（`:386`）门掉，所以它与 SSRT diffuse 是**二选一**，从来不是补充。

**一处必须纠正的区分**：DE **没有** fallback 的"删了不补"病。SSRT 关闭时 `AmbientMult` 根本不参与，A 没被删除，而 DE 的 `:412` 是 `lerp(directionalAmbientColor, envColor * albedo, envWeight)`——没数据时**退化为原版环境光**而不是黑。所以 DE 在能量上是安全的，它的问题**纯粹是质量**（用户已判定不如 SSRT）。

---

## 4. "最正确"的配置

按上面的推导唯一确定，不是口味：

| 项 | 值 | 理由 |
|---|---|---|
| SSRT Diffuse | **开** | 唯一有真实可见性的漫反射来源 |
| **Ambient Reinjection** | **开，Strength = 1** | §2 已证：唯一移除与补回同源同权的模式。Strength 1 才守恒 |
| SSRT Specular | **开** | 见 §5 第 1 条，它是反射遮挡的正解 |
| IBL Diffuse | **开**，env/sky 分量正确 | §3 症状 2：reinjection 把 IBL 正确性变成前置条件 |
| VanillaFresnel | **开** | F0 作为材质属性是正确角色 |
| └ Auto Cubemap Conversion | **开** | §3 症状 2b 已证：关掉会反射一张与环境无关、且被漫反射光照乘过的烘焙贴图 |
| SSGI Contact AO | **开** | 从 `multiBounceAO`（`:425-427`）进入，不被 SSRT 门掉，且几何类信号靠同帧空间手段（批次 6） |
| SSGI IL | 无所谓 | SSRT diffuse 开着时被 `:467` 自动门掉 |
| SSGI Directional Env | 无所谓 | 被 `:386` 自动门掉；作为 SSRT 关闭时的低配档保留 |
| Skylighting | **开** | 反射与 IBL 的遮挡输入都靠它 |
| 降噪器 | NRD REBLUR | 出货降噪器 |
| 置信度空间滤波 | **开**（批次 6） | reinjection 的噪声只能在这里压 |

### 这个配置目前仍存在的物理缺陷（三条，均有行号）

1. **`reflectance *= envMask` 被跳过**（`Lighting.hlsl:3304`）—— §3 症状 3。**这是本配置的头号待修项**，因为配置要求 VanillaFresnel 开着，所以这个 bug 一定在生效。
2. **反射没有可用的局部遮挡。** `:3297` 传了 `skylightingSH` 进去，但按已有的 Skylighting 审计，0.57m 格子 + 多层稀释分辨不出一个城门洞。铁条正在反射一个它看不见的半球——这就是"平、偏亮"的真实成因。
3. **IBL 的天空/环境比例还不可调**（排队中），而 §3 症状 2 已证它按 `(1-conf)` 权重进入每个像素。

---

## 5. 更便宜的近似替代品与取舍

已测基准（4K，RTX 4070 Ti Super，`PROFILE-2026-08-24.md`）：SSRT Trace **7.71ms** / 降噪 **6.85ms** / SSGI 2.00ms / Contact AO 1.01ms。

**先说结论：这个配置没有整体替代品，但有两处能在不牺牲物理正确性的前提下换更便宜的实现，还有一处是免费的质量升级。**

### 免费的质量升级（不是省钱，是白拿）

1. **把 ENVMAP 材质接到 SSRT specular 上。** 那个 pass 已经在跑了，它算的正是带真实遮挡的局部反射，而 ENVMAP 材质现在走的是 `GetDynamicCubemap` + Skylighting SH 这条粗糙路径。这同时解决 §4 缺陷 2 和"眼睛类材质由 IBL 决定"的老问题。**【测】** 可行性尚未读码验证 —— 需要确认 `psout.Reflectance` 通道与 SSRT specular 的合成顺序能否让前者被后者取代。

### 真正的省钱项，按性价比排序

| 项 | 收益 | 画质代价 | 判断 |
|---|---|---|---|
| **D. 干掉 NRD pack/unpack 集成税** | **-0.70~1.00ms** | **零** | 无脑做。纯粹是我们自己加的转换开销 |
| **A. 半分辨率追踪**（已批准） | 追踪 **≈-3.9ms**，降噪同步降，估计总 **-8~10ms** | 细节边缘 | 做。量级最大 |
| **B. 棋盘**（已批准） | 追踪 ≈-3.9ms，但降噪端**变贵**（PrePass 强制开、hitT 重建失效），净收益小 | 更少 | 做，但当对照组，预期不如 A |
| **E. 降 SPP** | 与射线数成正比 | 噪声 | 有效。降 MaxSteps 无效——Hi-Z 成本随射线数而非步数 |
| **C. 用 SSGI DE 换掉 SSRT diffuse** | -14.56ms，换上 2.00ms | **质量明显下降** | **否决为主线**，保留为低配档 |

### 取舍上的一句实话

C 是唯一能带来数量级性能变化的选项，也是唯一必须放弃画面上限的选项。用户已经拍板"SSRT 是主线"，所以真正的路是 **D + A**：在**不改变物理正确性**的前提下把主线成本压下来，而不是换一条更便宜的物理模型。

---

---

# 第二轮：上游调研结果（2026-08-24 追加）

用户提供的新数据：**VanillaFresnel 的几个勾选框里只有 auto cubemap conversion 会影响眼睛**，其余不会。并提示上游可能已修好眼睛问题。

## R1. 撤回上一轮的"头号未修 bug"

**【证】** 上一轮我把 `Lighting.hlsl:3304` 的 `reflectance *= envMask` 跳过列为头号待修项。**这是错的，它不是 bug。**

最新上游 `integration-0819:package/Shaders/Lighting.hlsl:2698-2700` 仍是同一形态：

```hlsl
if (!enableVanillaFresnel)
    indirectLobeWeights.specular *= envMask;
```

8 个月的上游演进里它一直保留。这是**有意设计**——VanillaFresnel 的卖点就是给没有 envmask 贴图的原版材质加反射，绕过 envMask 是实现该卖点的手段。上一轮我已经标注了这个可能性，现在定案：**不要动这一行。**

## R2. 眼睛问题的真实机制（由上游的修复反证）

上游有两个专门的提交，`route/ssrt` **都没有**（分岔点 `bedec8379`，2025-12-06，比这两个提交早半年）：

| 提交 | 日期 | 内容 |
|---|---|---|
| `de4e90c1b` | 2026-05-08 | 加 `EnableEyeSpecialHandling`（默认 true）与 `SpecularRoughnessBlend` |
| `498fa7d8a` | 2026-05-27 | **禁止在 GGX 关闭时开启 cubemap conversion** |
| `83a10637a` | 2026-05-28 | **加运行时 `IsEye` 标志，改写眼睛材质处理** |

**关键在 `83a10637a` 的 C++ 侧 `IsEyePass()`**，它用三个条件判定眼睛：

1. shader technique == `Eye`；
2. material feature == `kEye`；
3. **material feature == `kEnvironmentMap` 且 geometry 名字里含 "eye"** ← 决定性的一条

第 3 条说明：**Skyrim 里相当多的眼睛材质根本不是 Eye technique，而是普通的 `kEnvironmentMap` 材质。** 编译期的 `EYE` 宏识别不到它们，所以上游必须改用运行时标志（`ExtraShaderDescriptors::IsEye = 1 << 6`，通过 hook `BSLightingShader::SetupGeometry` vfunc 0x6 逐 draw 设置）。

**【证】** 在我们的树上，这类材质因此走通用 ENVMAP 分支，拿到 `Lighting.hlsl:2454`：

```hlsl
F0 = saturate(Color::GammaToLinear(envColorBase.rgb) * Math::PI * CubemapToF0Multiplier);
```

那个 **× π** 让明亮 cubemap 的平均色直接饱和到 ≈1.0 ⇒ **F0 ≈ 1 = 镜面金属**。而 `:2496` 的 `F0 = 0.027` 覆写挂在 `#if defined(EYE)` 上，对这类材质**根本不编译进去**。

**这条路径只在 conversion 打开时可达**（`:2442` 强制 `dynamicCubemap = true`，`:2453-2454` 是 conversion 专属的 else 分支）。**与用户"只有 conversion 影响眼睛"的观察精确吻合。**

**【证】** 我们树上还有一处独立缺陷，上游同一提交也一并重构掉了：`:2374-2385` 用的是 `#if defined(SPECULAR) … #elif defined(EYE)`。**`#elif` 意味着同时带 SPECULAR 和 EYE 的排列永远拿不到眼睛处理。** 上游把 SPECULAR 分支改成 `&& !isEyeMaterial`，并把眼睛处理挪成一个独立的、非 elif 的 `if`。

## R3. 这推翻了我上一轮的配置建议

**【证】** `498fa7d8a` 让上游在 `EnableGGX` 关闭时**强制清零** `EnableDynamicCubemapsConversion` 并禁用该勾选框。我们的树**没有这个门控**（`VanillaFresnel.cpp:30-42` 四个 Checkbox 平铺，无 gate），而 `EnableGGX` 默认是 **false**。

所以「conversion 开 + GGX 关」是**上游明确禁止的组合**，而这很可能正是用户当前的测试配置。原因是自洽性：conversion 把 `F0` 与 `roughness` 设成喂给 split-sum GGX 的形状，而 GGX 关闭时 `:3540` 仍然跑原版 phong specular，`:3543` 又把 GGX 用的 `F0` 混进 phong 的 `SpecularColor` —— 两套模型串味。

⇒ **§4 配置表更正：VanillaFresnel 的 `Enable Phong to GGX` 必须与 auto cubemap conversion 一同开启。** 上一轮只写了 conversion 开，是不完整的。

## R4. 顺带可拿的第三项改进

上游的 roughness 推导（`83a10637a`）比我们的更细：

```hlsl
float roughnessFromSpecular  = (1.0 - glossiness) * (1.0 - glossiness);
float roughnessFromShininess = ShininessToRoughness(shininess);   // 与我们的公式相同
roughness = lerp(roughnessFromShininess, roughnessFromSpecular,
                 SpecularRoughnessBlend * (1.0 - glossiness));
```

我们只有 `roughness = pow(2.0/(shininess+2.0), 0.25)`（`:2378`），等于上式的一个端点。**roughness 直接决定 cubemap 的 mip 级别，也就是反射的锐利程度**——这与"金属显得平"直接相关。

## R5. 上游对眼睛/头发的**环境光**问题没有答案

**【证】** `git ls-tree -r integration-0819 | grep ScreenSpaceRayTracing` 为空 —— **上游根本没有 SSRT 这个特性**。因此 `AmbientMult` 全局归零、以及 Ambient Reinjection，都是我们自己的设计（`966237eec`，2026-08-21）。

⇒ **§2 的结构缺陷结论不变，并且完全是我们自己的问题、也完全由我们自己修。** 上游只回答了 VanillaFresnel 的**反射**那一半。

## R6. 结论：这是两个被混为一谈的问题

| 症状 | 归属 | 状态 |
|---|---|---|
| 眼睛**反射**拉满、夜里发光 | VanillaFresnel 材质识别缺陷 | **上游已修**，可移植（R2/R3） |
| 眼睛/头发**漫反射亮度**与环境不匹配 | 我们自己的环境光移除/补回不对称 | 我们自己修 = 开 Reinjection（§2） |

## R7. 移植可行性

前置条件在我们树上**全部具备**：

- `Feature::PostPostLoad()` 虚函数存在（`src/Feature.h:91`）
- `globals::rtti::BSLightingShaderPropertyRTTI` 存在（`src/Globals.cpp:190`）
- 描述符 **bit 6 在两侧都空闲**（我们的 `State.h:187-195` 与 `Permutation.hlsli:57-65` 都只用到 bit 5，且 bit 3/4/5 的语义与上游不同，所以只能加 bit 6，不能整块搬）

**不能直接 cherry-pick**：上游 `Lighting.hlsl` 已重构为 `material` 结构体（`material.F0` / `material.Roughness` / `material.BaseColor`），我们没有。着色器侧四处改动必须按我们的行结构**重新表达**（`:2374-2388`、`:2494-2502`，以及 conversion 分支里对眼睛的排除）。C++ 侧可以近乎照搬。

---

# 第三轮更正：fallback ↔ reinjection 的变化落在哪里

**我在对话里说过"reinjection 改的是开阔、没遮挡的部分,封闭部分基本不变"。这是错的**,用户用白漫城门的两张对照图证伪:阴影门洞的色调差异非常明显。

## 错误的原因

我把"封闭处周围有几何"等同于"射线容易打中"。**但决定命中的是那些方向上的几何有没有出现在深度缓冲里。** SSRT diffuse 在**法线**周围的余弦半球采样,而门面正对相机 ⇒ 它的半球正对观察者 ⇒ 那些方向构造上出不了结果,只能算打空。

⇒ **凹进去、正对相机的表面是全画面打空率最高的一类,不是最低的。**

## 正确的判据

**变化幅度由"射线打空的比例"决定,与"开阔 / 封闭"无关。**

- 打空率高（⇒ 两模式差异大）：正对相机的表面、凹处、屏幕边缘、周围几何不在深度缓冲里的位置
- 打空率低（⇒ 两模式接近）：相对相机成大角度、且周围有大量在屏几何的表面,例如前景地面

## 更强的事实：这是整体换源,不是部分混合【证】

`ssrt_raymarch.hlsl:1224-1225`（只在 `UseDynamicCubemapsAsFallback` 下执行,而 reinjection 强制关闭它,见 `ScreenSpaceRayTracing.cpp:2790`）：

```hlsl
sampleColor.xyz = lerp(envColor, sampleColor.xyz, confidence);
confidence = 1;
```

| | 打空的射线拿到什么 | 报告置信度 |
|---|---|---|
| fallback | cubemap 辐射亮度,**满权重** | **强设为 1** |
| reinjection | 什么都不拿 | 真实值（低） |

所以在打空率接近满的表面上,环境光在一种模式下 100% 来自 cubemap、另一种下 100% 来自 DALC + IBL。**两种模式在射线层面就不同,不只是合成端填充策略不同。**

## 对决策的影响

换源发生在**所有正对视线的阴影凹处**（门洞、屋檐下、巷子、室内朝向玩家的墙面）——游玩视线里占比很大。因此"cubemap 还是 IBL"决定的是每一块正面阴影表面的整体色调,不是轻微偏色。

⇒ **§6 的顺序据此加强：IBL 必须先修完再做 fallback 退役的对比测试**,否则是拿一个只有天光的残缺 IBL 去和 cubemap 比,结论会假。

---

## 6. 建议的执行顺序

（第二轮后更新。上一轮排在第 1 位的 `reflectance *= envMask` 已撤回 —— 见 R1，它不是 bug。）

1. **移植上游的眼睛修复三件套** — 运行时 `IsEye` 标志 + GGX 门控 conversion + roughness 混合（R2/R3/R4）。这是唯一有**现成正确答案**的项，且直接消掉用户最在意的眼睛问题的反射那一半。
2. **IBL sky/env 独立滑条 + 饱和度**（已批准排队）— §3 症状 2 证明它在关键路径上。
3. **NRD 集成税** — 零画质代价的 0.7-1.0ms。
4. **半分辨率 + 棋盘双档** — 量级最大。
5. **调研：ENVMAP 材质接 SSRT specular** — 免费的质量升级，同时关掉两个老问题。
