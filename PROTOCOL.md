<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# OpenGC 协议

§1–7 与 `include/opengc/protocol.h` 定义通用协议 ABI 1.0；§8 描述已实现的 heap ABI 2，包版本为 0.2.0。
§9 定义统一语言运行时预设；实验 heap ABI 3 / 对象模型 3 组合独立 policy 与 allocator/registry，
Lisp 编译器、运行库、Object/AOT 与缓存已接入。移动式收集器未实现，二进制 ABI 尚未冻结发布。
“必须”是实现要求。未列能力不得通过命名或默认降级声称支持。

## 1. 执行和所有权

一个对象域只有一个 owner 线程；创建线程必须活至域销毁。所有域操作必须在该线程串行执行。
错误线程的调用在接触对象、输出和 allocator 之前返回 `OGC_WRONG_OWNER`。
回调期间域的公共操作返回 `OGC_BUSY`。其他线程不得与 owner 同时调用或销毁域。

V1 只支持 64 位、稳定地址和显式维护调用。不扫描寄存器或未登记栈，不推断标量位形为引用，
不提供后台线程、隐式 GC、语言回调或失败后替换收集策略。
调用者只能在 mutator 完全暂停、临时引用已登记根后执行 `ogc_collect`。
分配返回的裸指针不是根；在下一次显式收集前必须建立根或接入已保活对象的引用图。

分层所有权：allocator 拥有存储；domain 拥有可信对象登记和根；collector 拥有本轮追踪元数据。
collector 只经 `retire(id)` 交回死亡对象，不直接释放 payload。
provider、allocator 的函数表被参考域复制，函数代码和 context 必须覆盖域及最后一次释放。
V1 不提供动态模块加载与引用计数；模块保活由组装层承担。

## 2. 版本和协商

query 必须给出可读的 `struct_size`、major、最低 minor、指针宽度和 required capabilities。
校验方必须先判断大小再读后续字段，major 必须相同，minor 必须满足最低值，指针宽度必须为 8。
未知必需能力返回 Unsupported；缺失函数返回 InvalidArgument；失败 query 清空 table 输出。
query 不分配、不扫描、不建立域。

表可在相同 major 下尾部追加，发送方不读取未知尾部。数组元素及按值记录的布局在同 major 下固定；
增删 `OgcId`、`OgcEdge`、`OgcObjectInfo` 等字段必须升 major。
表与配置均须按声明的结构实际分配；只有大小字段可读的短表会被拒绝。

| 能力 | 含义 | 参考实现 |
|---|---|---|
| StableAddresses | 存活对象始终保持 base | full、retain |
| ExplicitFull | 只在明确维护授权下同步追踪全堆 | full |
| Retain | collect 不扫描、不退休对象 | retain |

ABI 1.0 没有 BoundedWork 能力、有限 slice 或自动 advice，调用者不得据此授予实时窗口。

## 3. allocator 和预算

`allocate(context, bytes, alignment, out)` 接收非零字节数、非零 2 的幂对齐。
算术溢出、配置额度不足、真实系统分配失败分别返回 SizeOverflow、BudgetExceeded、AllocationFailed。
成功返回满足大小、对齐要求的独占稳定存储，`receipt.bytes/alignment` 必须等于请求；token 供来源
allocator 释放使用。失败不改 out、不留下存储。allocator 不收集、不修改域、不重入 mutator。

`deallocate(context, receipt)` 必须无失败、无异常，使用原始回条恰好归还一次。
回条不能混用于另一 allocator 或 context。实现不猜测修复违反合同的第三方 allocator。
data、metadata allocator 分别选择；所有对象配置、撤销、GC 退休、最终关闭都使用原选择。
参考系统 allocator 的 token 保存底层 malloc 起点，base 可因对齐不同于 token。

参考域三个额度彼此独立：`data_limit` 是请求 payload 总字节，`metadata_limit` 是域/对象/根及
复制扫描表字节，`scratch_limit` 是一次 collector scratch 请求字节。0 是零额度，UINT64_MAX
取消配置上限。每笔申请前检查加法和乘法，失败不增加已用字节。创建失败也不归还已发行的域身份。
scratch 使用选定 metadata allocator，但与长期 registry metadata 分开限额、分别释放。
这些额度是逻辑请求计数，**不是 RSS 或 allocator 物理保留字节的上限**；对齐 padding、系统分配
头和 C++ 线程设施不在账内。需要物理总额时由宿主所选 allocator 再施加共享预算。

## 4. 身份、布局和发布

`OgcId = {domain, serial}`。参考组装层在一个链接实例中用进程态原子单调发行 domain；同域
对象和根共用不回绕 serial。关闭、取消、内存地址复用均不复用身份。重复静态链接到多个独立
DSO 的副本不共享发行器，跨副本交换域/handle 不受支持。真正的跨模块部署应共用一份域库。
耗尽返回 IdExhausted，不回绕到零。身份不可持久化为跨进程对象地址。

布局为 payload 字节数、对齐和严格递增的物理引用偏移表。每槽是完整、对齐的 `void*`，必须在
逻辑 extent 内；引用对象的 base 对齐至少为指针对齐。偏移表配置时复制，不依赖调用方后续寿命。
原子数据对象用零个 reference slot，标量与 padding 永不扫描。

```text
预备元数据 → 来源 allocator 配置 → 清零 → Constructing
    → 初始化字段 → publish_batch 整组验证 → Published
    ↘ abort → 撤销记录 → 原回条配对释放
Published → 完整追踪判死 → retire(id) → 撤销记录 → 原回条配对释放
```

Constructing 不能登记精确根、被公开 inspect 或从 Published 对象指向；域有任意构造时拒绝收集和关闭。
Constructing 可以互指，只有同一个 batch 中的未发布目标才可通过发布验证。
batch 必须非空、无重复、全为本域未发布身份；先验证所有对象和边，再无失败地整组发布。
失败不发布任何成员；调用方可修复重试或逐笔 abort。Published 对象不能手动 abort。
未发布构造被撤销后，其他构造中的旧边需由调用方修复；发布验证会拒绝该失效边。

payload 标量可由调用方初始化和修改，声明引用槽通过 `ogc_store` 写入。store 先检查 owner 身份、
物理槽和目标归属，再一次提交。Published→Constructing、跨域、内部地址充当精确目标均拒绝。
完整停止世界收集不需要并发屏障；idle／retain 仍执行相同的引用准入。
裸地址引用不携带 allocation identity，释放后的裸指针必须立即弃用；地址重用后无法辨认它
是否来源于旧对象。带身份的 owner/root handle 则会返回 StaleHandle。

## 5. 根与安全查询

根保存一份 **edge 的值**，不借用外部栈槽；宿主移动栈或退出作用域不会隐式更新/移除根。
create 返回域限定 handle，set 在准入后替换，remove 后旧 handle 永远无效。
Exact 必须命中本域 Published 对象起点或为零；Interior 为保守候选，仅用于已暂停并已登记的
宿主边界，命中任意 payload 内字节即保活，非命中安全忽略。不修改候选地址。
不允许对象字段产生 Interior 边。没有自动栈扫描或任意宿主指针保活。

inspect 首先查询可信登记，绝不解引用候选地址；精确/内部查询不匹配时成功返回全零对象。
半开范围为 `[base, base + bytes)`，one-past 不属于该对象。构造对象只供内部构造准入查询。

## 6. Collector 反向服务

provider 只能使用 `OgcServices`，不读取 Object、Root、域内部链表或 allocator 私有布局。

| 服务 | 必须满足的合同 |
|---|---|
| begin | owner/构造/重入检查；成功冻结对象、根和 payload，返回 Published 数量 |
| end | 成功 begin 恰好一次配对；包括 scratch、根扫描、字段扫描和 sweep 的失败路径；无失败 |
| object_next | cursor 初值 0；一次输出一个不可变可信对象或 has=0；对象身份唯一、范围不重叠 |
| root_next | cursor 初值 0；输出所有已登记根，无隐藏前缀丢失 |
| trace_edge | 只对可信身份和 `[0, reference_count)` 下标读一个精确槽；错误原样返回 |
| retire | 再核对完整身份，撤销登记后配对释放，不执行语言 finalizer |

cursor 是服务自有的不可解释 cookie，只在本轮 lease 有效，provider 只保留原值传回，不伪造。
参考服务使用稳定元数据链节点作为 cookie，不保留 payload 内部扫描指针。
成功冻结之后直到 end，不允许外部注册、写入、重绑或构造。协议是可信同进程扩展边界，不是
对不受信任插件的内存隔离；自定义 provider 负责遵守 begin/end 和死亡证明顺序。

full provider 先完整建立并校验对象快照，再扫描所有根、迭代扫描可达图，最后只退休未标记对象。
扫描、额度、分配失败在 sweep 之前发生时，**不释放任何对象**。
sweep 中 retire 失败可保留已释放的死亡前缀，不能复活它们；结果报告已成功退休的数量和字节。
调用方按原 status 处理，可重试新一轮；没有持久的半轮 mark 状态或遗留 lease。
retain 同样获得和归还 lease，随后返回 Disabled，不请求 scratch、不扫描或退休对象。

## 7. 失败、结果和关闭

所有可失败入口返回 `OgcStatus`，不得抛异常、abort 或进入语言回调。
参考库不链接 `lang::Result`，语言适配层应保留 status 与操作位置再映射自己的 typed Result。
query 失败清空输出；其他普通失败不改调用者输出。collect 在 begin 成功后是例外：即使失败
也返回已完成的扫描/退休前缀。准入失败不改结果。只有 status=OK 时可解释 outcome。
计数器不回绕；边计数溢出、畸形服务快照、非法字段边会在 sweep 前失败。

域关闭先检查 owner/重入/所有根和构造；Busy 时保留域供撤根/abort 后重试。
成功关闭停止使用 provider，再逐笔撤销 Published 登记、归还 payload 和 metadata，最后释放域本体
并将 caller handle 清空。最后一个 metadata allocator 释放回调发生在域生命周期结束后，
该回调不得访问先前保存的域指针。allocator context 和代码必须仍然有效。
根 handle、对象 handle 不拥有域；域销毁后不得再传入已释放的 `OgcDomain*`。

## 8. 兼容堆接口（`opengc/heap.h`）

本节仅适用于 `OpenGC::abi2_marksweep` / `OpenGC::abi2_bump` 和旧 mutator 产物，
实现保存在 `heap_abi2_marksweep.cpp` / `heap_abi2_bump.cpp`。默认 provider 与 Lisp 使用 §9，
不调用 ABI 2 收集器，也不将其栈范围扫描带入 ABI 3。

ABI 1 的 provider 表之外的第二层：给要内联快路径的语言运行时用的**链接期**接口。一组固定的 C 函数（`ogc_heap_*`），
每个构建只链一个实现；没有函数表，所以慢路径的代价就是一次 C 调用。

- **实现方**：`OpenGC::abi2_marksweep`（只标记 + 懒清扫 + 分步 SATB + 分代卡表 + 保守根 + 回收钩子）。
  `src/heap_services.cpp` 已有经 OgcServices 适配 `full` / `retain` 的代码（无内联分配、无屏障），
  但尚未接入 CMake，不能作为当前可选 heap provider；其 step/minor 退成完整收集，不能声称有预算/分代能力。
- **mutator 头**：每个实现带一个 `opengc/mutator_<provider>.h`，只含常量与布局——尺寸类、`OgcRegion`/`OgcMutator`
  （语言上下文的偏移 0）、对象头、屏障种类（`OGC_MUTATOR_CARDS` / `OGC_MUTATOR_SATB`）与堆内标志、卡表的地址合同。编译器在构建时
  包含选中的那个并内联它描述的快路径；编译好的代码只在同一个构建里使用。
- **能力**：`ogc_heap_capabilities(heap)` 报告实现做得到的（`INCREMENTAL` · `GENERATIONAL` · `COLLECTS` · `RELEASE_HOOKS` ·
  `INLINE_ALLOC` · `RETAG` · `WALK`）；做不到的操作答 `OGC_HEAP_UNSUPPORTED` 或什么也不做，不得假装完成。
- **失败**：对象内存分配不到答空指针（是否成为语言的陷阱由调用方决定）；实现自身元数据增长失败可以终止进程，须在实现里写明。
- **线程**：单 owner 线程；根是调用线程的栈与寄存器（保守）、登记的区间（保守）、句柄表（精确）。

这层的替换与通用协议的 domain 配置不同：domain 创建时可注入合格的 collector/allocator，
语言 heap 则在构建/链接时选定实现和 mutator 描述，生成码必须匹配。两层都没有存活堆原地切换算法的接口。
ABI 2 产物仍须匹配原 provider 与布局；不允许把其身份直接改成 ABI 3 后加载。

### 8.1 第 2 版修订（marksweep/bump 已实现）

heap ABI 为 2，独立于上面的通用协议 ABI 1；OpenGC 包版本为 0.2.0。旧的进程全局函数不保留转发层：

- **堆实例**：慢路径函数取 `OgcHeap*`，`ogc_heap_create(const OgcHeapConfig*)` / `ogc_heap_destroy` 管理生命期；
  mutator 登记到一个堆，同一堆只由其 owner 线程操作。进程全局堆不再是接口的一部分。
- **布局宏是合同**：每个 mutator 头必须定义以下宏，消费者只读这些宏，不复制数字：
  `OGC_MUTATOR_ABI_VERSION`、`OGC_MUTATOR_CAPABILITIES`、`OGC_MUTATOR_HEADER_BYTES`、`OGC_MUTATOR_DESC_ID_OFFSET`（相对 payload，带符号）、`OGC_MUTATOR_DESC_ID_BITS`、
  `OGC_MUTATOR_ALIGN16_PAYLOAD_OFFSET`、`OGC_MUTATOR_REGION_OFFSET` / `OGC_MUTATOR_REGION_STRIDE`、
  `OGC_MUTATOR_CLASSES_INIT` / `OGC_MUTATOR_NUM_CLASSES`。provider 不能用描述符 id 识别的对象（大对象、
  16 字节对齐对象），在该偏移处必须是 0，不能是悬空值。
- **能力决定快路径**：`OGC_MUTATOR_INLINE_ALLOC` 与 `OGC_MUTATOR_CARDS` / `OGC_MUTATOR_SATB` 为 0 时，消费者
  不得生成对应快路径或屏障；能力位与宏必须一致，不一致是构建错误。
- **可选能力**：`objects_of` / `payload_room` / `retag` 归入 `OGC_HEAP_RETAG`，`walk` / `walk_roots` 归入 `OGC_HEAP_WALK`；
  未声明时这些函数答 `OGC_HEAP_UNSUPPORTED`。
- **NoGC provider `bump`**：内联 bump 分配、无屏障、不声明 `COLLECTS`；`collect`/`step`/`minor` 答 unsupported，
  统计照常；配置可给出堆上限，超限时分配答空指针。内联分配不登记单个对象；统计读取 run 的 mutator 前沿，注销时封存。头/id 偏移为 16/-16；支持 WALK 和 destroy 时一次性释放资源，不支持 RETAG。
- `heap_services.cpp`（full/retain 经服务层适配）按同一修订更新后，可作为可选 provider 接入；它不是 NoGC 的高性能实现。

实例创建核对 `OgcHeapConfig` 的精确 ABI/尺寸、required/disabled 能力；可选 RETAG/WALK 可单独禁用。
`max_bytes` 限制对象映射总量（含对齐过映射，不计卡表及元数据），对象内存不足答空；元数据增长仍采用本 provider 原有的 abort 策略。
`collect/minor` 返回 status；`step` 返回 `{status, finished}`，只在 OK 时解释 finished。
`ogc_heap_barrier(heap)` 提供寿命覆盖到 destroy 的 marking/cards 地址；无屏障 provider 应答空地址。
mutator 必须零初始化，登记后只在所属堆分配，注销清空 region 和归属；描述符不可跨堆使用。
句柄编号归实例，保存保守根字；把外堆对象放进本堆句柄不保活外堆对象。
根区间允许重复 base，remove_roots 撤销最后一次匹配登记，保留先前范围；用于任务临时根与调用者范围的嵌套。
destroy 在 mutator、根、句柄、周期钩子或回调未退出时返回 Busy 且不改实例；静止销毁对剩余资源执行一次 release 后释放映射和元数据。
销毁中的 release 不得操作本堆，允许重复 destroy 并获得 Busy；其它实例不受影响。

父仓库 Runtime JIT 已由各 Isolate 创建独立实例，Ctx 携带域身份；只在显式共享时使用同一实例。
低层独立 SDK/Image/AOT 仍可使用默认 fallback，不能将它当作 Runtime 的默认堆。安装时绑定目标实例屏障，无屏障 provider 的地址为空。
`heap_services.cpp`/`mutator_services.h` 仍为旧 heap API，未迁移、未编译；D6 的第二种 heap provider bump 已独立构建/搬迁验收。

`ogc_heap_walk` 只跟随调用者指定的根，不隐式加入登记区间，供代码退休选择保留集。
`ogc_heap_walk_roots` 具有相同回调合同，额外纳入完整 owner 根集合：线程栈/寄存器、句柄及所有登记区间，供布局更新准入。

marksweep 在进入收集/外部 walk 时保存调用者的栈边界和寄存器，cycle hook 内的嵌套 walk 复用这一边界；
收集器和 walk 的内部临时值不属于 mutator 根。未被 setjmp 写入的快照存储也清零，避免未初始化字被当成保守根。
两者都只遍历，不收集或 retag；无 WALK 能力时均返回 UNSUPPORTED 且不调用回调。此扩展新增函数，不修改 `OgcWalk` 的结构布局。
父仓库 D2 已移除就地迁移，默认语言堆禁用 RETAG；独立 marksweep 实例仍可按需启用该可选能力。

### 8.2 执行栈范围（marksweep/bump 已实现）

`ogc_heap_stack_current()` 返回本线程选定的 `OgcStackBounds {low, high}`；没有显式 scope 时查询 OS 线程/fiber。
两种当前目标的栈都向低地址增长；收集和带 stack 的 walk 从当前帧/保存的 mutator 帧扫描到 high。
`ogc_heap_stack_enter(&scope, bounds)` 必须在目标栈上、分配或遍历之前调用，范围须包含该调用帧。
scope 存储地址固定，进入后字段归实现私有，不能搬移或修改；不要求进入前零初始化。
每个线程各有一个 LIFO scope 链，影响该线程调用的所有堆；enter/leave 不分配、不开锁。
空范围、反向范围、范围不含当前帧、重复进入、非栈顶退出及外线程退出返回 `OGC_HEAP_INVALID_ARGUMENT`，不写 scope 或活动链。
collector/walker 的 mutator 快照未退出时，原本有效的 enter/leave 返回 `OGC_HEAP_BUSY`，同样不改状态；其回调不能切换执行栈。
`ogc_heap_stack_leave` 退出最内层 scope；在另一执行栈再次运行 GC/Walk 或释放 scope/栈存储前必须退出，不得扫描仍选着另一执行栈的线程。
扫描入口复核当前帧和保存的扫描起点都在选定范围；违反原生合同会终止，避免跨地址空洞读取。

scope 只选择正在运行的栈；暂停的调用栈须由调度器保存寄存器，另以 add_roots 登记从保存帧到栈顶的可读范围，返回后配对撤销。
不能把整个 reserved stack 当作可读根：底部可能尚未提交或含 guard page。
本扩展增加 C 符号，不改 heap/mutator ABI 2 的既有布局，也不修改通用协议 ABI 1。
它不执行切栈、调度、析构或 trap 恢复；父仓库仍需实现任务边界的根/资源清理及实际退出路径。


<a id="portable-heap"></a>

## 9. 统一语言堆协议（实验 heap ABI 3）

`include/opengc/portable.h` 定义实验 heap ABI 3 / 对象模型 3。
`marksweep.cpp` / `nogc.cpp` 仅实现 policy.h；`heap3_allocator.cpp` 持有唯一对象登记，
`heap3_runtime.cpp` 提供 portable.h 的共享 runtime 预设。它们不调用 ABI 2 heap/mutator 实现。
最终只链接一个 provider，由它导出 `ogc_portable_query`；可在该 provider 内创建多个独立堆。
ABI 1 和显式命名的 ABI 2 兼容库保留独立接口。移动式仅规定协议，不实现移动收集器或搬迁测试 provider。

### 9.1 分发与绑定

- 默认发布的原生库只依赖统一 mutator ABI、语言对象模型、目标平台和 runtime helper ABI；
  不记录 provider 名称作为兼容性条件，不包含 provider 私有布局、分配算法或屏障常量。
- 宿主最终链接时选择一个 provider，由它实现统一入口；生成码通过所属执行上下文绑定堆实例。
  同一 provider 可以创建多个独立堆，不能把进程唯一 provider 误当成进程唯一堆。
- 满足同一对象模型及协议要求的移动式、非移动式和 NoGC provider 必须能执行同一份合规产物。
  provider 内部布局可以不同，对外的引用表示、类型描述、对象字段与操作语义必须一致。
- 不支持存活堆原地换算法。不同 provider 的堆之间不得直接保存对象引用；跨堆传输必须显式复制或序列化。
  不要求同一进程同时链接多个 provider；统一符号由最终选中的一个 provider 提供。

### 9.2 对象与类型模型

- 每个对象有稳定的逻辑身份；物理地址不是身份，搬迁不得改变语言可观察的身份、相等性或身份哈希。
- 统一对象模型固定 null/强引用的表示、payload 的可见布局、对齐规则和类型查询语义。
  类型描述必须准确标识强引用槽及引用数组；标量中恰好像地址的字不能被当成可改写引用。
- 类型标识由稳定语义的查询入口取得。provider 私有头、标记位、转发指针、尺寸类和 region 布局
  不属于可分发产物可直接读取的 ABI；实现可用私有前缀或侧表保存，不能覆盖语言字段。
- 类型登记结果归堆；产物保存可重定位的类型描述，加载时绑定本堆标识，不把另一堆的标识直接复用。
- 编译器不得根据地址不变消除跨 safepoint 的引用刷新，或把裸地址当作持久对象标识。

### 9.3 分配与引用访问

统一服务表提供下列基础操作；具体字段与签名见 `OgcPortableApi`，尚未冻结发布：

| 操作 | 必须满足的合同 |
|---|---|
| 分配 | 输入类型、大小与对齐；返回初始化规则明确的对象引用或失败；可能触发 GC，存活实参先纳入根 |
| 读取引用 | 返回可用的当前引用；provider 可执行读屏障，非移动式也必须承接同一入口 |
| 写入引用 | 接受对象引用、槽位置和新值；保留旧值及执行写入的次序由统一操作负责，覆盖 SATB/卡表等要求 |
| 初始化／批量复制引用 | 不能因对象“刚分配”或使用 memcpy 绕过屏障；单独操作必须规定初始化可见性与发布边界 |
| 查询类型／身份 | 不泄漏私有对象头，搬迁前后保持语义一致 |
| safepoint | 发布可更新的存活根，允许回收和搬迁，返回后读取更新后的引用 |

屏障操作的堆内执行阶段不得搬迁传入的对象；若操作包含 safepoint，必须先通过可更新根承接实参，
在 safepoint 后重新解析对象及槽位置，不能沿用旧地址。引用写入不能由“先裸写、后任意通知”代替。
对象字段内的引用访问、全局引用、宿主 Handle 和 runtime 内部引用均受同一合同约束。
原子引用访问或多 mutator 并发执行需要另行定义内存序；本版目标仍为每堆单 owner mutator，
不得把后台收集能力解释成允许多个线程任意读写对象。

统一入口是默认分发路径。内联优化只能依赖统一 ABI 明确冻结的快路径，或在加载时验证的运行时绑定；
不能重新引入“默认产物按 provider 编译”。允许另行发行明确标注且严格拒绝错配的特化产物。

### 9.4 精确根、搬迁与原生借用

- 生成码必须在每个可能搬迁的 safepoint 提供精确、可更新的存活引用位置，包括寄存器保存位置、
  栈槽、全局、闭包、暂停任务及 runtime 保存的引用；可采用根帧或 stack map，不限定具体编码。
- GC 更新根及对象引用槽后才允许 mutator 恢复。派生内部指针用 BORROWED 类型登记，provider 必须识别所属对象及 base+offset 并同步重建，
  或处于有效 pin 中；不能把内部地址误当成精确 payload。非堆借用地址保持原值。
- 保守根区间不能被任意改写。若兼容路径允许保守根，provider 必须固定其可能引用的对象，
  或在执行前明确拒绝该路径；默认可分发代码不得要求保守根能力。
- Handle 的存储位置／token 稳定、归属堆明确，内部引用可由 GC 更新；取得的裸地址仅在明确的借用窗口内有效。
- pin 必须在可能 GC 前保活输入，返回地址与配对释放的租约；释放后地址不可继续使用。
  provider 不支持 pin 时必须拒绝借用准入，调用者只能选择已有的复制／Handle 路径，不得静默返回可移动地址。
- no-GC 借用窗口必须阻止本堆搬迁；分配、重入、回调或切栈若可能破坏此保证，须在进入前拒绝，
  不能仅禁止显式 collect。借用不能无条件跨 safepoint、任务暂停或原生回调延长。
- Walk、回收及资源回调不得泄漏可长期保存的裸对象地址；暂停任务的根与 pin 在取消和异常退出路径也须配对解除。
  代码退休需单独枚举代码引用，不能把可执行地址作为可搬迁的对象引用修正。

### 9.5 能力、协商与失败

- ABI 查询先验证 major、对象模型、平台和必需表前缀；size/minor 只用于明确的兼容扩展。
  未识别的必需能力或缺失入口必须在执行／初始化前拒绝，不用空指针调用或默认猜测布局。
- 统一引用访问、精确可更新根、Handle 和 safepoint 是消费者的基础合同。
  “会移动”描述 provider 行为，不能成为默认产物是否生成根更新／屏障的编译开关。
- collect、minor、有限预算 step、WALK、pin、弱引用和终结分别协商。
  NoGC 仍实现基础操作，不支持的回收操作返回 unsupported，销毁负责释放所拥有存储与资源。
- 并发收集器必须满足同样的读写、发布和根握手语义；未定义的弱引用、复活或终结次序不得由 provider 自行赋予语言语义。
- 基础入口的失败边界见 §9.7；后续扩展也必须逐入口明确，建堆／绑定失败不发布半初始化实例。

### 9.6 验收

`opengc_portable_client` 是只包含 `portable.h` 的 C99 静态库，只编译一次，分别链接 marksweep 和 bump。
两个可执行文件各只链接一个 provider，覆盖统一布局、身份、根、Handle、pin、数组／重叠复制、回收、
代际写屏障、回调重入拒绝、能力拒绝及 OOM。安装消费者同样使用一份 C 归档分别链接两者。
同一份预编译对象／静态库须不经重编在不同 provider 下加载、分配、调用并校验身份与字段，
并验证 ABI／对象模型错配拒绝、缺能力拒绝、屏障次序、OOM、资源及代码退休。
移动兼容的验收是协议合规而不是搬迁执行：消费者只经统一入口读写引用，跨 safepoint 的引用全部在精确可写根、
对象引用槽或 Handle 中并在其后重新读取，派生地址只在 pin 内使用，不依赖保守根或地址即身份。
强制搬迁 provider 与搬迁执行用例不是交付物；仅给非移动式 provider 换名或共用布局不构成移动兼容的执行证据，
文档不得把协议合规写成已在移动式堆上执行过。ABI 3 的 Object/AOT 比较模型、上下文布局、heap/mutator/helper/格式与目标，
不比较 provider 名。ABI 2 兼容库的原身份检查仍保持独立。


### 9.7 C ABI 3 的实现合同

- query 校验 ABI 3、模型 3 与所需表尺寸；create 校验完整 config、reserved 与必需能力。
  不支持返回 UNSUPPORTED，版本/结构不合法返回 INVALID，跨 owner 返回 WRONG_OWNER，回调重入返回 BUSY。
  失败不修改输出；调用者负责 heap/type/token 的有效生命周期，不承诺失效指针可安全探测。
- OgcRef 为 null 或本堆精确 payload；identity 单调、非零、堆内唯一，地址不是身份。
  默认 allocator 已去除逐对象前缀；调用者只能通过 type_of/identity/extent 查询，不得读取块或侧表。
- layout 以 struct_size 校验，登记复制名字、引用偏移和 trace program。固定对象 size 为可见大小；
  数组为 prefix_bytes 加 count 个 size 字节元素，payload 对齐 8/16，允许字节元素。
  引用偏移有序、唯一、8 字节对齐且不能跨元素；引用数组的 stride 必须保持槽对齐。分配清零可见字节。
- trace program 支持 STRONG、BORROWED、TAGGED、IF_U8/IF_U64 与 END；CODE 仅供退休 WALK，GC 不追踪。
  PINNED_AMBIGUOUS 只供显式无 tag union 或主动登记的 opaque host buffer，不能改写其可能为标量的位形。
  默认生成的帧、globals、对象与闭包均提供类型描述，机器栈与寄存器区间不扫描。
- roots_enter 登记连续可写精确槽；roots_enter_typed 登记地址/条件/种类的稀疏程序。
  roots_leave 严格 LIFO，roots_remove 用于独立生命周期的模块或宿主根注销。槽内更新在 safepoint 前发布。
  STACK_FRAMES 提供堆属主独占的链表头；生成码把栈上的节点和槽描述链接后发布根，
  返回后重新载入仍活跃的值。栈暂停时节点保持有效，栈终止时先撤销废弃帧。
  C++ 宿主借用在跨调用/收集时须使用 Handle、显式根或 pin；裸 C++ 局部不是自动根。
- DEFERRED_ALLOC 的 alloc_deferred 不执行收集或回调。超过预算后设置属主读的 poll 字节；
  检查点只在字节非零时发布活跃根并进入 safepoint。宿主 alloc 维持原有可收集合同。
  ALLOC_RUN 适用于最大 256 字节的无前缀、无析构固定对象。refill_run 返回首个对象，
  同时发布已登记并清零的连续单元。匹配 key 后，属主通过推进 next 取出单元。
  回收、WALK 和 no-GC 窗口先关闭分配块；尚未发出的单元不参加对象查询和统计。
  可选 ALLOC_RUN_PAIR 允许 allocation_run 返回固定地址的两个连续记录。消费者在确认能力后可从任一记录取单元，
  只写该记录的 next。refill_run 仍从首记录返回对象，可交换两条记录或撤销任一预留。调用后重读字段。
  关闭分配块的操作同时关闭两条记录。单记录消费者继续使用首记录。
- load_ref/store_ref/copy_refs 与 read_word/write_word/copy_memory 不触发 safepoint；
  store/copy 在覆盖旧引用前完成 SATB，并记住 old→young 边。copy_refs 为已验证连续引用槽的快照复制。
  copy_memory 为类型已由编译器验证的值复制，不允许调用者借此绕过对象模型。标量可以直接访问。
  PLAIN_READ 允许生成码直接读取已登记或本地对象的引用槽。ALLOC_RUN 的已发出范围允许直接写入
  经类型验证的本地引用槽，范围内对象尚未晋升且增量标记未开始。其它引用写入使用屏障。
- marksweep policy 实现 epoch 标记、增量 SATB 与 remembered 集；allocator 维护 nursery 登记并实际懒清扫。
  自动检查点优先使用 nursery 回收，按晋升量调整预算，并周期性全量回收。
  runtime 在周期完成后处理资源队列、weak 与通知。NoGC policy 不追踪/回收；默认 bump 预设组合 arena allocator 与 NoGC，维护入口返回 UNSUPPORTED。
  步进/终结预算为尽力限制的微秒，最大 UINT32_MAX，不承诺硬实时或并发 mutator。
- WALK 不触发收集，按精确 refs、显式 words 或选择的已登记根遍历，按对象去重；
  只报告已描述的引用/CODE 字段，不扫描普通标量。registered_roots 掩码 1 选全部根，2 选 Handle/pin，4 选生成栈帧；
  选择性代码退休排除未证实存活的退休 globals，显式纳入生成帧 PC、CODE 槽与宿主代码租约。
  回调期间分配、收集与递归 WALK 返回 BUSY；周期结束 hook 可发起 WALK，只有此时允许撤销退休模块根。
- weak 不保活，marksweep 在判定死亡后清空；bump 在销毁前保留。无复活语义。
  no_gc_enter/leave 为 LIFO 窗口，窗口内 alloc/collect/minor/step 返回 BUSY；pin 保活并固定对象，不能冒充 no-GC 窗口。
- destroy 在根、Handle、pin、weak、窗口或 hook 未撤时返回 BUSY；成功后释放全部对象、元数据和存储，资源只释放一次。
  release 只读本对象标量，user 保持至销毁，禁止重入本堆；堆销毁前必须保持回调代码有效。
  max_bytes 限制 provider 存储，stats.heap_bytes 报物理存储，live_bytes 为逻辑存活负载；统计不等于可见 payload 总和。

### 9.8 Lisp 接线

Ctx 保存 api、heap、services 和当前堆的检查点字节、分配块及生成帧链表头地址。
内联访问使用 portable.h 的公共布局，编译器不依赖 provider 的对象头、region 或卡表。
AOT 文件版本 4；Object 身份 format 2 采用模型 3、heap/mutator ABI 3、helper 13 与目标，缓存格式 2、编译期包格式 16。
安装的 OpenGC::heap 通过 OPENGC_HEAP_PROVIDER 选最终 provider，不必重编 compiler、runtime 或 Lisp lib。
`lisp_gc_portable_package` 编译/归档一次 Lisp AOT，在移除编译器归档的安装目录内分别链接两 provider，
校验结果 144，并核对 Lisp 对象/归档与三个运行库归档哈希未改变。
移动式仅有协议合同，当前执行证据限于非移动 marksweep 与 NoGC bump；不宣称已执行搬迁。

### 9.9 收敛职责：反向服务与共享预设

GC policy 的核心合同收敛为追踪、标记/代际状态和回收决策。allocator/registry 拥有唯一对象登记、地址定位及实际释放；runtime 拥有根、类型引用枚举、Handle、弱引用/终结调度、任务与代码退休。

核心 policy 不包含 portable.h，不创建对象/类型/根/Handle 表，不扫描语言 tag 或代码字，不直接调用 malloc/free 释放对象。allocator 提供地址稳定的公共 policy record，policy 只使用其中的算法状态和工作链接，不复制一套对象登记。建立堆时反向提供根枚举、引用枚举、nursery 枚举和周期完成回调；完成回调由 allocator 根据存活判断退役对象，再由 runtime 处理 weak/资源及周期通知。

`policy.h` 为这一小型 C99 合同；`portable.h` 保留为现有消费者的共享 runtime 预设外观，不再视为每个收集算法必须逐项实现的 provider 表。默认 marksweep/bump 外观分别组合共享 runtime 预设与一个核心 policy，独立 policy 目标不链接该外观或 Lisp。统一 ABI 3 产物与单 provider 最终链接合同保持，现有根/Handle 等入口由预设承接。

分配由 allocator 登记；出生与存活判定采用共享预设的内联 record 操作，不逐对象调用 policy 的登记接口，也不转移对象所有权。反向服务不要求逐对象新增动态登记，也不承诺 cursor 快路径适用于全部 policy。移动式仍只规定协议：反向边携带可写槽、base 偏移与固定标志，allocator 可提供 relocate 服务并保持记录稳定，GC 在恢复 mutator 前更新精确边。当前两个 policy 和默认 allocator 预设均非移动，不声明 MOVING；缺重定位服务时必须拒绝移动 policy，不假装已实现搬迁。

独立目标为 OpenGC::policy_marksweep / OpenGC::policy_nogc；默认 OpenGC::marksweep / OpenGC::bump 是预设组合。独立 C99 宿主使用一份对象文件分别链接两个核心，只提供自己的登记、根、引用和退役回调。核心未定义符号不包含 portable/runtime 服务，也不调用对象分配器；完整验证与成本见父仓库 docs/audit/opengc_policy.md。

### 9.10 allocator 块级预设（本轮重构）

allocator 按类型和尺寸组织块，以页级定位表作为唯一地址登记。类型/尺寸/count/对齐属于块；不在每个对象前放置私有 Node，不逐对象插入地址哈希或所有权链。NoGC 以块 frontier 表示分配，无逐对象 GC record；追踪预设将稳定 policy record 放在侧表。根/引用枚举得到的 record 仍由 allocator 唯一拥有，GC 核心职责不扩展。

回收通过槽位图和块级懒清扫，在分配慢路径复用；资源等待 finalize 时保持物理槽占用，不能提前复用。身份在复用后重新发放，跨类型/块仍按实际分配次序单调递增。块补充时有界清扫其他尺寸，额度不足时释放空块；完整收集不遍历普通死对象。WALK 使用块访问位图与预留工作链接，不在遍历期间分配。marksweep 链接放在侧记录；NoGC 利用不可见尾部 padding，不足时由块预留侧表，身份空间与工作链接互不覆盖。页表定位必须验证真实 payload 范围、已分配槽和 padding，不能靠解引用任意地址前缀猜对象。

marksweep 预设在完整标记枚举（包括无出边叶对象）及分配出生时更新块 live_epoch。摘要早于 completed_epoch 时，可确认非资源块整块死亡：在分配慢路径重置占用位图/frontier，首次复用批量清零旧占用范围，侧记录继续保持地址稳定。摘要保守覆盖增量期间出生的黑对象；minor/WALK 不把摘要推进到新的完整周期。混合存活块仍逐槽判定，资源块仍由 pending/finalize 管理，不能进入整块重置。此优化不在完整收集结束时新增死对象/块遍历。

本轮保持公开 ABI 3 和编译产物合同，重构 allocator 内部；不宣称已重新启用生成码内联分配。实际内联需要后续明确的公共快路径合同，不能让编译器偷读块布局。
