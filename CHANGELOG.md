# opengc 变更摘要

- ABI 3 增加可选 ALLOC_RUN_PAIR。marksweep 和 bump 可保留两个不同类型的固定对象分配块，补充保持首记录入口，统计与收集都会撤销两块未发出的预留。

- ABI 3 分配块从小批量预留起步，同一个类型与 key 连续用尽分配块才扩大批量。marksweep 按位图字登记和撤销连续槽位。

- ABI 3 增加可选的堆对象所有弱句柄，回收包装时释放其句柄，宿主弱句柄生命周期保持独立。

- 2026-11-20：ABI 3 增加延迟分配、固定对象分配块和栈帧根链能力。生成码检查点优先执行 nursery 回收，按晋升量调整预算。provider 在收集和 WALK 前回收预留块中尚未发出的单元。

- 2026-11-20：自动全量回收改为在「上次回收后的分配量」同时达到 8 MiB 与存活字节数时触发。固定 8 MiB 预算会在大活集合上每 8 MiB 重扫一遍，使总成本对活集合呈二次方（f77 map n=1e6：865 → 98 ms）。堆入口的属主检查改用线程指针/TCB 读取（Apple arm64 读 `tpidrro_el0`，其 `__builtin_thread_pointer` 在各线程相同），不再每次调用 `pthread_self`。

- 2026-11-19：新增能力位 `OGC_PORTABLE_PLAIN_READ`：声明的 provider 承诺 read_word 在属主线程上就是一次无副作用的对齐 8 字节读，消费者可自行读引用槽；读屏障、转发指针或移动式 provider 不得声明。marksweep 与 bump 预设都声明。roots_enter/roots_enter_typed 复用已退出帧的节点，不再每帧 malloc/free；节点随堆销毁释放。

- 2026-11-19：完整标记访问无引用布局时只更新块的存活 epoch。单强引用布局直接枚举槽，精确引用定位内联块缓存并为 16 字节步长使用移位；借用地址仍按对象内部偏移解析。

- 2026-11-19：引用写入一次判定槽类型和归属校验要求。块登记保留两个最近定位的块，块释放时清除对应缓存，减少目标和值分布在不同块时的页索引查找。marksweep 与 bump 共用路径，保留强引用校验和写屏障。 macOS arm64 同轮 A/B 的 54 组 GC 压测中，耗时比分别下降 9.0% 和 8.3%（几何平均）。

- 2026-11-19：marksweep 为非资源块维护保守完整标记 epoch 摘要，整块死亡在分配慢路径直接重置游标，首次复用批量清零。避免逐槽扫描死亡块，混合存活块/增量黑对象/资源终结语义保持；不扩大 GC 核心职责或修改公共 ABI。百万次短命分配同轮下降约 8.7%，与 bump 比值约 1.07，见 docs/audit/opengc_dead_block.md。

- 2026-11-18 重构 allocator：用类型/count 分块、页级定位及大块范围树替代逐对象前缀、地址哈希和登记链。marksweep 使用侧记录与块级懒清扫，NoGC 无 GC record；新槽推进 frontier。保留单调身份、额度、精确引用、资源延迟复用与无分配 WALK；ABI 3 和已编译产物合同不变。测量及限制见父仓库 docs/audit/opengc_allocator.md。

- 2026-11-18：将 GC 算法收敛为 policy.h 的 6 个入口，marksweep/NoGC 核心独立链接；allocator/registry 独占对象登记与释放，runtime 反向提供根/引用及生命周期服务。删除单体 heap3_impl.h，ABI 3 外观保留共享预设；NoGC 核心不再冒称负责 bump 分配。独立 C99 宿主验证无 runtime 链接依赖。

- 2026-11-18 优化 ABI 3 分配：懒清扫不重启已完成游标；小单元有界缓存及额度不足时释放；NoGC 按需补建索引，marksweep 仍在分配时建索引以保持暂停界限。类型归属直接查询 owner。短命分配同轮耗时下降 59%/68%，ABI 不变。

- 2026-11-18：marksweep/bump 原生重写到实验 heap ABI 3 / 模型 3，不调用 ABI 2。统一精确根、类型/身份、读写屏障、WALK、弱引用和 no-GC 窗口；默认无机器栈扫描。ABI 2 原实现保留为显式兼容库。Lisp 编译器/运行库/加载器/缓存已迁移，helper 8、AOT 文件 4；同一 Lisp 归档与运行库不重编分别链接两 provider。最终只链接一个 provider，移动式只规定协议。性能样本及平台限制见 VALIDATION 与父仓库 docs/audit/opengc_abi3.md。

- 2026-11-17 macOS 执行栈检查启用 _DARWIN_C_SOURCE，保留 _XOPEN_SOURCE 所需 ucontext 声明的同时公开 mmap 扩展。

- 2026-11-16 重复根 base 的撤销明确为后进先出，避免任务临时根撤销调用者此前登记的范围；根数组保序删除。

手写摘要：只记改了什么、为什么；提交明细：`git log -- opengc`。

- 2026-11-16：marksweep/bump 增加线程内 LIFO 执行栈范围登记。收集与 Walk 使用选定栈顶并检查当前帧，供独立任务栈接入；暂停调用栈仍须显式保活。真实三层切栈验证根保活、配对撤根和资源关闭，尚不提供任务或 trap 恢复。

- 2026-11-09 原生迁移与 GC 合同整合。
- 2026-11-15 D5：heap ABI 2——显式堆所有权、可选 RETAG/WALK、发布的 mutator 布局与编译器能力门控。
- 2026-11-15 D6：NoGC `bump` provider（内联分配、WALK、配额、frontier 统计、静止时销毁资源）；构建期在 marksweep/bump 间选择，不支持的 NoGC 操作显式拒绝而非假装回收。
- 2026-11-15 D2：默认语言堆禁用 RETAG；根遍历快照与 Walk 控制字初始化，避免填充字节伪造根。
