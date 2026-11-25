<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# OpenGC 设计与实现范围

本文的 domain/full/retain 章节说明通用协议 ABI 1；语言堆的 marksweep 是另一条已接线实现，
见下文“语言堆与可替换边界”。通用协议未提供某项能力，不代表 marksweep 也未实现。
当前规范见 [PROTOCOL](PROTOCOL.md)，Lisp 产品目标见父仓库 `docs/MERGED_INTERFACE_DESIGN.md` §6。

## 设计来源

参考的是 2026-11-05 工作树中的源码合同，包含当时尚未提交的 GC 拆分及多策略工作。
父仓库设计说明有迁移前后的历史段落，发生分歧时以实际头和实现为准。
OpenGC 是新实现的独立参考库，没有直接搬运 Lisp runtime 或 collector 源码。

| 本项目设计证据（父仓库路径） | OpenGC 的选择 |
|---|---|
| `include/lisp_core/memory/allocator_protocol.h` | allocator 独立选择；带来源 token 的原始回条配对释放 |
| `include/lisp_core/memory/object_registry_protocol.h` | 可信 base/extent/identity，与 payload header 分离 |
| `include/lisp_core/memory/collector_protocol.h` | C 表版本协商；collector 经反向服务追踪和退休 |
| `src/lang/runtime_memory.cpp`、`src/lang/runtime_domain.cpp` | 组装层发行域身份、选择组件、保持关闭顺序 |
| `src/memory/full_collector.cpp`、`src/memory/retain_collector.cpp` | 显式 Full 与 Retain 分开声明能力 |
| `src/memory/low_latency_collector.cpp`、`src/memory/generational_collector.cpp` | 通用协议后续算法扩展的参考；不代表这些旧策略已移植为 OpenGC provider |
| `docs/invariants/01-lang.md`、`docs/invariants/03-native.md` | 隐藏构造、发布先准入、稳定地址、显式收集边界 |

父项目的 Lgc provider preview ABI 3、Runtime/Unit/helper 版本不作为 OpenGC 版本。
Ogc ABI 从 1.0 独立起步：不要求 TypeDesc、TaggedValue、ExecutionContext、NativeUnit 或 JIT offset。

## 程序结构

```text
宿主 / 语言适配器
  ├── OpenGC::domain ──────── OpenGC::protocol
  │     ├── copied roots + immutable registry metadata
  │     ├── selected data allocator → receipt → object bytes
  │     └── selected metadata allocator → registry / root metadata
  └── 显式选择 provider
        ├── OpenGC::full ─── OpenGC::protocol
        └── OpenGC::retain ─ OpenGC::protocol
              ↕ OgcServices：begin/end、enumerate、trace、retire
            同一对象域
```

`domain` 不引用 query_full/query_retain/system_allocator；`full`、`retain` 不链接 domain 或彼此。
宿主也可不用参考域，直接提供服务表让 full provider 收集自己的登记对象；测试中的服务 fixture
验证这条路径。反过来自定义 provider 也可注入参考域，无需编译 Lisp。

参考域用独占 metadata 节点形成链表，逻辑上区分 allocator 和 registry；不依赖某种 payload
块几何，不把 allocator 指针当可信对象，也不使用 STL/Core 容器的隐式 OOM 处理。
用户配置覆盖所有 payload、节点、扫描表及 collector scratch。allocator 以外的 callback 均不进入
业务代码；引用偏移表替代任意语言扫描回调，让参考实现能在扫描前界定完整物理槽。

## 完整追踪算法

1. 成功 begin 冻结所有 mutator 操作，复制 n 个可信对象描述。
2. 在同一次选定 metadata 申请中预备对象记录和 n 项标记队列。
3. 按 base 排序，拒绝非法/重叠范围。对根做二分查找，Exact 未命中失败，Interior 未命中忽略。
4. 每个对象首次标记恰好入队一次，迭代扫描引用，不递归、不改变 payload。
5. 所有可达边验证成功后退休未标记对象；释放 scratch，end 解除冻结。

collector scratch 是 `n * (sizeof(Entry) + sizeof(uint64_t))`；实现先检查乘法和额度。
参考域查询、身份查找和部分删除为线性复杂度，所以端到端最坏可能达到 O(n² + e·n)，
不能据此宣称吞吐或低延迟已经达到父项目水平。collector 自身快照排序和 edge 查找约为
O(n log n + e log n)，以宿主回调成本为外部成本；qsort/allocator 不提供硬实时上界。
本版选择便于检查的实现作为协议 oracle，后续索引优化必须保持同样的失败与身份合同。

## 通用协议参考实现的边界

- 无自动阈值和 poll：所有收集均是明确维护，不把分配失败变成任意时刻的 GC。
- 无增量/分代/treadmill：还未定义 active-cycle 出生 credit、old/new 屏障、根 dirty cursor 和取消。
- 无弱引用/退休回调：先完成完整图及身份测试，避免引入对象复活和代码页生命周期歧义。
- 无移动：没有可重写根、pin、内部指针 fixup 或重定位服务。
- 无 JIT ABI：宿主编译器仍通过公开 allocator/domain 接口；不嵌入私有对象布局 offset。
- 无动态 provider loader：配置中持有的代码/context 由宿主保活，不能提前卸载。
- 通用 full/retain 尚未成为可选的 Lisp heap provider；父仓库新语言已链接 OpenGC marksweep，旧 Runtime 仍使用 Lgc。

## 后续扩展顺序

先补 Linux/Windows 64 位、故障注入和跨语言 ABI；再优化 registry 索引，保持 full 作为对照。
增量协议需要新的协商能力和 size-prefixed 扩展表：request/step/cancel、有界扫描进度、实际工作量、
出生两阶段 credit、物理槽 old/new 准入、根变更和 weak retirement。不能只增加 phase 字段或把
一次完整扫描记作一个 work unit。不同 provider 只声明实际通过合同与压力测试的能力。

移动和并发会改变根与读写合同，需单独设计并升 ABI major，不能静默塞进通用协议 ABI 1。
语言适配层负责将协议状态映射到自身 typed Result，并分别验证 native/root/fiber 生命周期，
不要求 OpenGC 依赖旧 `lang::Result` 或其它语言的错误类型。

## 语言堆与可替换边界

marksweep.cpp/nogc.cpp 只依赖 policy.h，提供独立核心库；默认语言堆组合 heap3_allocator.cpp 与 heap3_runtime.cpp 的共享预设，不调用 ABI 2。
统一表定义对象/类型查询、可更新精确根、引用屏障、WALK、弱引用、no-GC 窗口和资源终结。
marksweep 核心使用 epoch 标记、SATB 与 remembered 工作集；allocator 提供 nursery 枚举及懒清扫。非资源块使用保守完整标记 epoch 摘要确认整块死亡，在分配慢路径重置 frontier，首次复用批量清零；混合块和资源仍保留逐槽/终结路径。NoGC 核心不追踪；bump 是 arena allocator 加 NoGC 的默认预设。
allocator 使用按类型/count 分组的页对齐块；页表和大块范围树只登记块，无逐对象前缀或地址哈希。marksweep 的 80 字节记录在侧表，NoGC 不创建 GC record；NoGC 身份和无分配 WALK 工作链接使用不可见 padding 或块侧表。同一 C/Lisp 归档跨预设消费，客户端不得读取这些布局。

编译器只知道统一对象模型和 helper ABI。Ctx 在运行时持有服务表与所属 heap，生成码不内联 provider 分配区或卡表。
精确帧描述调用点活跃引用、条件 tag 与 CODE 位置；globals/闭包/对象携带同一 trace program。
任务切栈不扫描机器栈，只维持已发布根帧及显式宿主租约；退休 WALK 读取生成帧 PC 和类型已知的代码引用。
C++ 宿主跨收集保存值须显式根/Handle/pin；原生借用及内部地址重建合同见 PROTOCOL §9.4。

最终链接选一个 provider，不支持活堆热切。Object/AOT/缓存以模型、布局、统一 ABI 和目标判兼容，不比较 provider 名。
同一份 Lisp 静态归档与运行库不重编，在两个独立消费工程分别最终链接 marksweep/bump 已有自动化证据。
移动式只要求协议，不实现移动收集器或搬迁测试 provider；不能把这两种非移动实现的消费结果称为搬迁执行。

heap.h ABI 2 保留于显式 OpenGC::abi2_marksweep/bump，原实现移到 heap_abi2_*.cpp，原保守扫描与快路径合同仍属兼容库。
heap_services.cpp/mutator_services.h 为未接线的 ABI 1→旧 heap 适配候选，不属于默认 ABI 3。

## 最小 policy 与反向登记（本轮重构）

按 PROTOCOL §9.9 拆分：allocator/registry 是唯一对象登记所有者，runtime 预设提供根及类型引用枚举；GC 只经反向服务追踪 policy record 并作存活决策。原 portable.h 的大表属于共享 runtime 预设，不应成为每个算法的实现负担。独立 policy 库已由外部 C99 allocator 驱动，不链接 heap3 外观；现有 Lisp/C99 消费保留统一外观迁移，不能以删除功能实现“最小化”。
