# DAS 编排架构语境（CONTEXT）

> 沉淀自 2026-08-18 MaaEnd pretask 排查及其后的一轮架构对齐讨论。
> 定位：后续会话理解 DAS 任务/编排模型的 primer。事实均以代码坐标锚定；
> 行号会漂移，以符号名为准。

---

## 1. 存储层：一切从任务文件开始

任务实例（含 graph 任务）统一落盘在 `settings/{profile}/taskIdNN.json`（当前 profile 恒为 0）：

```
settings/0/taskId25.json
├─ properties        acceptedProperties（用户选择，唯一事实源）
│   ├─ adapter / pi / ui / portDefinitions / portMap …
├─ authoring         revision（每次 apply +1）、sourceFingerprint、lastChangeKind
└─ nextExecutionTime / enabled / taskGuid / pluginGuid
```

- **envelope（编译产物）从不落盘**。执行时现编现用，生存期 = 一次编译到一次执行。
- `scheduler.json` 只有 `taskOrder`/`nextTaskId`；插件级 `settings/0/{pluginGuid}.json` 存插件全局设置（当前为空）。

## 2. 编译层：唯一编译器与 envelope

各任务类型有 authoring session（插件经 `IDasTaskAuthoringSessionFactory` 提供）。
`Compile({purpose:"execution"})` 把 acceptedProperties 翻译成执行产物 **executionInput（envelope）**：

- 用户选择是**引用**（controllerName="Win32-Front"、pretasks 名单），envelope 是**实物**
  （typed controller init params、resource 路径、物化的 pretask exec/args、schedule）。
- 翻译必须持有 catalog（interface.json 的 parse 结果），所以编译天然属于 authoring 层。
- DasMaaPi 的编译器：`MaapiAuthoringSession::Compile`；envelope 契约：`ExecutionEnvelope.h`
  （version + pluginGuid + taskTypeGuid 校验，为跨 IPC 演化设计）。

编译发生在三个调用点（同一个编译器）：
1. 调度器 OnTick 直跑任务（`SchedulerService.cpp` OnTick → `GetOrCreateAuthoringSession` → `session->Compile` → `ExtractProviderExecutionInput`）
2. UI apply/preview（校验 + 预览，产物不存）
3. graph 编译引用 repository 条目（`CompileRepositoryEntryProvider` → `RepositoryInvokeCompiler`）

## 3. 两条执行路径，一个运行时

```
入口 A：调度器直跑（runNow/定时）
  OnTick(主进程) → authoring Compile → envelope
    → IPC → host 进程 MaapiTask::Do（IDasTask）
    → ParseExecutionEnvelope → MaaRuntime::Run
    （+ schedule 语义：GetNextExecutionTime/daily 补跑，只在此路径）

入口 B：GraphTask（graph/formSequence）编排
  graph 任务 OnTick → graph authoring 编译（entryRef 引用在此展开，复用上面 ③）
    → CompiledGraphPlanDto → IPC → GraphRuntime
    → 逐节点 CreateTaskComponent + ApplySettingsChange + Do
```

`MaaRuntime::Run` 是唯一 MaaFW 执行器（resource 加载 → hash 校验 → attach 资源 →
**pretask** → 创建 controller → 连接 → agent runtime → PostTask 循环）。
pretask 在 CreateController **之前**执行（协议 v2.7.0）；选项值由编译器序列化为
单行 JSON 追加在 args 尾部（与 MXU `buildPretaskArgs` 同约定）。

## 4. GraphTask 编排模型

### 4.1 authoring 文档两种形态

- **graph 模式**：`GraphDocumentDto`（nodes + edges + 图级输入输出口），自由连线。
- **formSequence 模式**：`FormSequenceDto`（items 有序数组）。`FormSequenceProjector::Project`
  把 item → node，相邻 item 自动生成 **signal 边**（顺序即数组序，UI 不可见）。

边两种：`data`（输出口→输入口传值，编译成 `PortBindingPlanDto`）与 `signal`
（门控激活顺序，`signal_routes`/`back_edges` 支持循环）。

### 4.2 节点两种引用

`GraphNodeTargetDto.target_kind`：

| | componentRef | entryRef |
|---|---|---|
| 指向 | 组件类型（manifest 注册表 guid） | repository 条目 = 某任务实例的存档 |
| 配置归属 | **图**（node.settings 内联，值语义） | **被引用任务**（引用语义，只存 entryId+expectedRevision+指纹） |
| 编译 | 注册表解析端口 → settings 原样拷入快照（直通） | RepositoryInvokeCompiler：load entry → 校验 → 对方 authoring session Compile → envelope |
| 要求 | 无 | 对方任务类型必须有 authoring capability + 声明 executionComponent |
| 典型 | FlowControl 原语（delay/branch/for）、规划中的「启动进程」 | maapi 任务等有完整配置界面的任务 |

**判断规则**：这个配置配不配拥有自己的家（独立文件+配置界面+版本）？
配 → entryRef；不配（参数属于使用点）→ componentRef 内联。
因果方向：参数属于使用点 → 不需要 authoring → 内联（authoring 的有无是**结论**不是原因；
反向推导会得出「给每个组件补 authoring 就不用内联」的错误结论）。

### 4.3 编译期归一化：运行时零区分

entryRef 节点在编译产物里**不存在**——被改写成普通的 RepositoryInvoke 组件节点
（FlowControl 注册），快照（component_guid + execution_input=envelope）经
`compiledSnapshot` 输入口交付（ABI 由 TaskComponentContractTest 钉死）。
GraphRuntime 只认一种节点：`CreateTaskComponent + ApplySettingsChange + Do`。

## 5. 参数模型

### 5.1 二分公理

- **settings 通道**：authoring 期常量（编译期定死）
- **ports 通道**：运行期值（上游产出，PortFrame/binding_plan 投递）

节点 = 一次函数调用；内联 settings = 调用点实参。所有编排系统同构
（Airflow params/XCom、n8n 节点配置/上游数据）。

### 5.2 envelope 参数的两条通道

入参物理终点：`MaaTaskerPostTask(entry, pipeline_override)`。

- **通道 A（配置期，在用）**：UI 选项值 → PiCompiler 烧进 envelope 的 `pipeline_override`。
- **通道 B（运行期，两头断）**：graph 上游值 → 输入口 + portMap（口名→PI参数名）
  → `MergePortMapInputs` 合进 override。机制已实现有测试，但：
  ① 只接在孤儿组件路径（读 settings.portMap）；
  ② RepositoryInvoke 包装只转发 executionInput 一个口，参数口不透传。
  接通属 formSequence 后续工作，最小闭环（启动游戏→跑 MaaEnd）不需要。

### 5.3 端口可选是默认

`GraphPortDefinitionDto.is_required = false` 默认；`MergePortMapInputs` 对无值口直接跳过。
语义 = **覆盖式**：不传走配置值（A），传了才覆盖（B）。三级兜底：上游值 > 端口默认值 > 不覆盖。
端口暴露面收敛：只有项目显式声明可接参的口才成为端口（MaaEnd 仅 2 个），
controller/pretask/大量 option 不占端口（走存档）。

### 5.4 schema 的一张表、三个产出时机

UI/编译器只消费一种东西：端口定义表（id/类型/label/默认值，`GraphPortDefinitionDto`）。
表的来源决定「档位」：

1. **构建期手写** → manifest `config`（delay_ms 等；组件作者拍脑袋的常量，无数据源可解析）
2. **authoring 期解析** → portDefinitions/dynamic_ports（从项目数据派生，换项目即换口集；
   依赖 entryRef 侧的 authoring 会话）
3. **不产出** → `object` 口直传（executionInput：编译产物，graph 只是邮差，
   给它产出 schema = 把 PI 协议搬进 graph 层）

## 6. 配置漂移与一致性（revision 体系）

图不存储动态口，只存指针+版本钉；**副作用靠每次编译重新解析传播，不靠通知**。

三道闸（已实现）：
1. apply → 存储层 revision+1
2. session 缓存失效：`ResolveCachedAuthoringSession` 按 `creation_revision != current`
   重建，上下文携带最新 acceptedProperties
3. 编译期双校验：expectedRevision 不匹配 → `repository-revision-mismatch`；
   连线对最新解析的端口集校验（连了消失的口 → 结构化编译错误）

UI 是懒发现（pull：进入视野即 Get；编译失败引导重解析）；push（SSE revision 事件）
只是把 pull 时机提前的体验增强，一致性永远由 pull + 编译墙保证。
**input/output 随 authoring 变化的组件必须走 entryRef 模式**——动态口不可能进静态 manifest。

## 7. HTTP 与存储门面

```
/scheduler/0/tasks/{taskId}/authoring/* 、runNow、enable…   ← 调度器门面
/scheduler/0/repository/entries/{entryId}/authoring/* 、…   ← 仓库门面（graph 引用侧）
        ↓
DasSchedulerController → SchedulerService → TaskRepositoryStore（仓库语义）
        → SettingsManager → settings/0/taskIdNN.json
```

**taskId 与 entryId 同一空间同一文件**——任务实例天生双重身份（可被调度 / 可被图引用）。
这是「同一份配置两个入口行为一致」的存储基础。
authoring 会话分 `task_authoring_sessions_`/`repository_authoring_sessions_` 两池，
session 类型与编译语义相同。

actionCatalog（组件目录）由 Core 从 IDasTaskComponent 注册表枚举注入 graph authoring
上下文；从组件目录选 → componentRef，从任务列表选 → entryRef（目录来源决定 kind）。

## 8. 已知缺口与既定方向（2026-08-18 定）

### 8.1 组件路径修复（「⑤」，formSequence 前置）

`MaapiRunTaskComponent` 的 manifest 契约是 `settings: []` + 输入口 `executionInput(object)`，
但实现消费自造的 piPath/taskName settings 键（生产无人写）+ `BuildMinimalSettings`
第二套有损编译（丢 controller/pretask 选择）。修法：

- Do 改为纯消费：读 executionInput 口 → `ParseExecutionEnvelope`
  → `MergePortMapInputs`（挪进共享内核，空表即空操作）→ `MaaRuntime::Run`
- 两个 Do 统一到共享内核 `ExecuteEnvelope(envelope, stop_token, port_map, inputs)`；
  Task 保留 schedule 语义，Component 保留 port I/O（接口职责不同，ABI 不可合并）
- 退役：`MaaPiExecutionEngine`、`BuildMinimalSettings`、`EngineInput`、组件 settings 键、
  `ApplySettingsChange`；复活死 helper `MakeInputPortMap` 为正式测试
- 验收：同一份 acceptedProperties 走调度器与走 graph，envelope 逐字节等价

### 8.2 formSequence 跑起来 + 启动进程组件

用户方向：官方插件提供「启动进程」组件（componentRef；config 声明
exePath/args/workingDirectory/waitWindow{classRegex,windowRegex,timeoutMs}），
formSequence 组合 `启动进程（等窗口） → 跑 MaaEnd（entryRef）`。
关键缺口即窗口出现延迟——等窗口参数必须做进启动组件。

**组件开发套件（2026-08-18 讨论定稿）**：现状每个组件作者手抄五类样板
（JSON↔IDasJson 桥接 ×3 份、PortMap 读写 helper ×2 生产+1 测试、settings 逐键
解析、ApplySettingsChange wire 各解读一半、guid/工厂四件套），~300 行。
定稿方向：
- **固定协议 schema 的 IDL 化组装 builder 家族**（2026-08-18 二次修正）：凡编译期
  定死的 wire/协议文档结构（组件 settings-change wire 的
  settings/payload/acceptedSettings/setValue、authoring 变更信封 {kind,payload}、
  maapi authoring doc 的 FieldDescriptor 固定字段、HTTP 外壳 code/msg/data），
  字段名进协议 builder 接口方法名——ABI 强约束 + 跨语言同名（拼错协议字段
  编译期报错）。**通用组装层零新增**：IDasJson 已是 stringly 键+类型化值的
  可变 JSON 文档（DasJson.idl Set*ByName/Set*ByIndex + CreateEmptyDasJson
  工厂，跨语言绑定现成）——manifest 派生键直接用它；否决新增通用 builder
  （IDasJsonBuilder 与 IDasJson 全面重复，用户指出；旧名 IDasTaskComponentSettings
  退役）。协议 builder 内部组合 IDasJson，导出即 IDasJson。JSON 桥接样板
  （WrapJson/CloneJson 序列化往返三份拷贝）优先以 CreateEmptyDasJson +
  Set*ByName 直装替代；PortMap helper 公共化照旧。**实现落位 das/Core/Utils/**
  （helper 性质，与 DasJsonImpl/DasPortMapImpl 同层同构）
- **PortMap/Json helper 公共化**：FlowControlTaskComponents.cpp 匿名 ns 与
  DasMaaPi PluginUtils 的三份拷贝合并进公共头（das/PluginCommon 或 das/Utils）
- **C++ 组件基类由 IDL 生成**：含 yyjson default_caster DTO 反射糖
  （cast<SettingsDto>），manifest config schema 与 DTO 靠 field_name_rule 对齐
- 判断标准沉淀：IDasXxx 的料 = 边界另一侧有宿主实现的跨语言契约；
  纯插件侧语法糖 = 公共头/静态库（先例：cpp_yyjson caster、DasPtr、fmt 包装）

另需补：graph authoring UI 的 entryRef 选人面板、actionCatalog label 友好化
（现为 guid 兜底）、（可选）RepositoryInvoke 参数口透传接通通道 B、（可选）SSE 漂移提醒。

### 8.3 起源：MaaEnd pretask 排查定论（历史）

「pretask 没跑」是误判：链路一直正常（envelope 携带、执行、选项 args 尾部 JSON、
注册表画质写入 0x500×0x2d0 全验证）。失败根因 = 游戏未启动，Win32 控制器找不到
UnityWndClass/Endfield 窗口，属预期行为。GameSetting pretask（go-service）是
HKCU\Software\Hypergryph\Endfield 的画质注册表写入器，不含启动游戏逻辑
（MaaEnd 项目也无任何启动游戏的 pretask/任务）。i18n 文案已加「请先启动目标程序」提示。

---

## 9. IPC 传输与远端引用协议（2026-08-26 性能定案沉淀）

> 起源：`IpcPerformanceTest.RemoteProxy_IsSupported_FirstCall` 反映性能下降，
> 对照老仓库 DuskAutoScript@47122b98（迁移前基线）同机实测定位。本节沉淀
> 传输层拓扑、远端引用协议语义与已定稿的修复方案，供 Phase 108 及后续会话使用。

### 9.1 出站/入站路径拓扑（代码坐标）

- **出站**：`IpcRunLoop::PostSend/PostSendWithTransport`（登记在途计数 →
  `co_spawn`）→ `SendToSessionCoroutine/DoSendCoroutine` →
  `PrepareOutboundFrame`（捎带总闸 `piggyback_enabled_`：取 pending rider、
  判定拼装/standalone）→ `AnyTransport::SendCoroutine/SendScatter`
  （传输层 `send_mutex` 异步锁 + `async_write`）。
- **入站**：读循环逐帧解析（32B 自描述 header）→ `RouteIncomingMessage` →
  `RESERVED_AS_SIZE` 置位时先剥离扩展段（内嵌帧**内联递归派发，先于宿主**），
  再路由宿主帧。
- **两种合帧方式并存**（`SendRidersThenHostStandalone`，即 D-20）：
  - pipe/AF_UNIX（`CAN_BATCH_FRAMES=true`）：多完整帧**连写**一次 SendScatter，
    帧内容不变，接收循环天然按序逐帧处理；
  - Http（`CAN_BATCH_FRAMES=false`）：**扩展段拼装**——改写外层 header 置
    `RESERVED_AS_SIZE`/reserved/body_size，rider 嵌入宿主 body 前缀。

### 9.2 远端引用协议与「每调用物化抖动」

- **物化**：`ProxyFactory::GetOrCreateProxy` 弱缓存（`TryAddRefForCache` CAS
  复活），`RegisterRemoteObject` 条目 0→1 时申报 CONTROL_PLANE
  `RemoteAddRef`(0x82)；proxy 引用归零死亡 → 条目移除时发
  `RemoteRelease`(0x84)（`DasProxyBase` 析构，fire-and-forget）。
- **in-param 抖动**：stub 反序列化接口参数（如 Dispatch 的 variant vector）
  每调用物化→死亡。老协议（47122b98）此抖动为纯本地 `++ref_count` **零消息**；
  新协议每次抖动 = 一对 ADD_REF/RELEASE 消息 + 两侧 DOM/holder 账本 + main 侧
  final-release 的 BT 桥接投递。
- **owner 侧对象身份**：条目拆除后重注册产生**新 oid**（`next_local_id_` 单调
  递增、generation 恒 1），oid 永不重用——顺序安全性由「引用互斥」保证：
  RELEASE(x) 与引用 x 的业务帧在同一发送方不可能共存。

### 9.3 根因定案（打点实测数据）

Compute 稳态（10000 次调用，帧身分直方图）：

| 指标 | 老(47122b98) no-ASAN | 新 no-ASAN | 短路模拟(免申报) |
|---|---|---|---|
| Compute 往返 | 158us | 524us（闸 OFF 202us） | **197us** |
| host 单帧直通写 | — | **95us**（main 同路径 29us，锁排队放大） | 21.8us |
| main 每调用入站帧 | 1 | 3（响应+ADD_REF+RELEASE 各 10100 条实测） | 1 |

三因叠加：① 每调用 2 条控制面消息（~330us 主体，短路模拟实证）；② 搭车结构性
失效——stub 里 `p_arguments_owner` 析构先于响应 post，RELEASE 抢走刚入队的
ADD_REF rider，而 RELEASE 帧被 `IsRemoteReleaseEvent` 判定不可携 rider →
standalone 拆发、**响应空车**（拼装段稳态零执行，assemble 计数铁证）；③ host
每调用 2 个发送协程抢 `send_mutex`。

### 9.4 修复方案定稿（Phase 108 实施目标）

**出站 FIFO + drain 合写**（用户裁决的语义基准）：

- 每 target 一条出站 FIFO，**所有出站帧统一入队**（业务+ADD_REF+RELEASE），
  替代 pending-rider 队列与抢车交互（`MarkOutboundSendInFlight`/周期末豁免/
  双汇点/`IsRemoteReleaseEvent` 载体排除全部收敛删除）；
- drain 时机 = post 即 drain（带走已入队帧）+ dispatch 周期末兜底
  （`AddRefDispatchGuard` 挂点保留、语义简化为清空 FIFO）；
- pipe/AF_UNIX 走 D-20 连写（提为主路径），Http 走扩展段拼装（现状保留）；
- `DasProxyBase` 析构的 RELEASE 改为入队（延迟上界 = 周期末，无害）。

**验收断言**：稳态每调用 host→main **一次物理写**，帧序 = 入队序 =
`[ADD_REF][RELEASE][Response]`，对端引用计数平衡（位置无关，先建后拆即正确）；
Compute 预期恢复 ~200us。

**明确不做**（讨论中否决/降级的方向）：关总闸（用户否决，捎带是重要功能）；
生成器 owner 析构后移把 Response 提到中间（正确性不依赖排列，仅微秒级唤醒
顺序差异，降级为可选优化）；in-param 瞬态视图免申报（协议级改动，留作远期）。

**测量纪律**：ASAN 乘数 1.4~2.6 波动大，性能 A/B 必须 no-ASAN；热路径诊断
禁用 fprintf（每帧 stderr 写 +50~100us 会污染测量），只用 atomic 计数 + 析构
汇总；cdb 采 MinGW DWARF 栈：`llvm-nm` 导出符号地址 + `sym+off` 还原 VA +
`llvm-symbolizer` 解名。

## 10. 图/线性模式语义法则（2026-09-19 用户裁定）

**线性（formSequence）是图（graph）模式的真子集。**

- **图文档永远是图**：一旦文档是图模式，永不转换为线性模式。不存在
  graph→formSequence 的模式翻转——该方向必丢信息（超集降子集），
  旧 `convertView` 的 best-effort 翻转（往 `document.tags` 塞
  `LINEAR_TAG`，AuthoringDocContract.cpp convertView 分支，D-84-29
  "拓扑单链不变量归 Provider 责任"的设计）**整个退役**。
- **允许的方向只有升级**：线性文档可单向升级为图（单链是图的特例，
  无损）；formSequence→graph 合法，反向不存在。
- **线性变更只作用于原生线性文档**：`addFormSequenceItem`/线性
  `setValue`/move/remove 等线性 change kinds 对图文档一律拒绝——不是
  给「线性重建丢分区」打补丁（111 code review CR-01 的快照/回放修法
  由此作废），而是图文档根本不进线性变更路径。
- **对图文档的 formSequence 投影至多是只读视图**（如未来需要展示），
  绝不成为变更通道。
- CR-01 的正解：移除/拒绝 graph→linear 的 convertView 分支 + 线性
  change kinds 加图文档守卫；viewData.positions 等图侧数据由此天然不可
  破坏（动态口面不再落图文档——归各文档自己的内容承载，见下方最终形态）。

**推论（动态口架构最终形态，五轮裁定收敛后随本 phase 收尾定稿）**：

1. **动态口=authoring 文档机制的能力（opt-in）**——需要动态口的组件/
   任务类型实现会话工厂即得；纯 manifest 组件不接入（口面=manifest
   静态声明，无动态口编辑也不需要；Core 通用 manifest 会话方案否决，
   不给不需要的人强加机制）。MaaPI 为首期消费组件（PI 表单暴露
   toggle+ports section 纯投影推导，勾选状态存 acceptedProperties）。
2. **图文档 ports 分区整体退役、零口数据**——存储只留引用+拓扑+配置
   （112-F1 原则零例外，begin/end 不豁免）；在途旧分区由会话创建一轮
   兼容迁移等价迁入 begin/end 节点 settings。
3. **编辑=文档**——双击节点进入该任务类型自己的 authoring 模式
   （MaaPI→PI 表单；图→图编辑器；begin/end→其边界文档表单），UI 业务
   中立只渲染文档；不存在也不允许任何图侧通用「暴露/端口面板」，口的
   声明与编辑=各文档自己的内容（ports/types section、暴露 toggle 等
   字段，经 setValue 等通用变更承载）。
4. **begin/end 文档化**——与 MaaPI 同构：authoring session 实现于 Core
   GraphRuntime 并经 DAS_C_API create 出口暴露（CreateGraphRuntime
   同款先例，零新机制类），状态域=begin/end 节点 settings；调用节点
   签名物化存储副本随之退役（口面经 entryFaces 直取被调条目文档）。
5. **faces 注入=零新导出入口（context 键注入）**——SchedulerService
   在图会话 context 装配时（管线外圈、锁外）以预备 pass 现算
   entryFaces/nodeFaces 快照随 context 注入，Core 会话在既有解析点
   自解析消费（Setter 兄弟入口否决，actionCatalog 键同形态）；编译期
   零活查询（编译宿主够不着条目存储/PluginManager 且处于管线持锁期）；
   faces 与文档快照同 freshness（宿主+绑条目混合 revision 戳作会话
   缓存失效键）。
6. **编译口面=声明面∪文档口∪图结构**（全部按节点；provider 退守纯
   声明面——guid 聚合段删除）；重名不拒绝、文档侧胜出+WARN；失效边
   编译期自动断开+WARN 并随 Apply 写回（纯 Get 零修剪；新画无效线仍
   编辑期当场拒绝）；类型注册表随图视图投影（内建九词∪文档 types[]）。
7. **addNodePort/removeNodePort/renameNodePort=「文档编辑口列表」的
   标准变更词表**——不绑图文档（角色门废除；方向门保留 begin/end
   专属），begin/end 文档与未来任何组件文档编辑口列表都用它。

运行期法则：GetDocument 实例级（动态口=实例口，不进共享 catalog）；
执行期零改动（binding 口坐标编译时冻结、执行重放）；无跨文档陈旧
（图在打开状态才能编辑图内条目——条目编辑必经图会话，context 必重建，
编译必新鲜）。

**第 10 节补遗（收尾校准，2026-09-20）**：上述最终形态已全量落地——
文档协议立法（ports/types section+formSequence chrome 三键，Core
validator/schema/C# 三层 lockstep）、预备 pass 注入、编译口面按节点
合并、编译期自动断线写回、类型注册表随投影、UI 文档驱动渲染与双击
路由最终形态；执行链零改动、线性⊂图守卫可执行（图文档线性变更
结构性不可达）。完整决策与否决方案清单：
`.planning/phases/111.1-dynamic-port-authoring-generalization/111.1-CONTEXT.md`。
