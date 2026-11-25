<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# OpenGC 验证记录

## 整块死亡快速复用（2026-11-19）

合并 main 后重验：两树完整构建/安装成功，marksweep 110/110，bump 106 通过、4 项按能力跳过，零失败；两树三配置语料各 517 项零回归。安装包与同一 Lisp 归档跨 GC 通过，知识快照 4786 源文件通过。集成性能及来源区别见下方验收记录。

保守完整标记摘要驱动非资源块游标重置，首次复用批量清零；混合叶存活块、增量期间出生黑对象与待终结资源覆盖检查通过。新旧 9 轮交错测量 marksweep 百万短命分配 24.520→22.380 ms，bump 20.920 ms，比值约 1.07；存活链表样本约慢 0.8%。两树完整构建/安装通过；marksweep 109/109，bump 105 通过、4 项按能力跳过，零失败；三配置语料各 510 项零回归，同一 Lisp 归档跨 GC 及独立安装/搬迁消费通过。完整验证及样本见 [验收](../docs/audit/opengc_dead_block.md)。

## 块登记 allocator（2026-11-18）

新槽 frontier、marksweep 侧记录与批量位图清扫、NoGC 无 GC record；宽 WALK 不动态分配。定向服务/GC 检查及完整构建/安装通过；marksweep 全量 109/109，bump 105 通过、4 项按能力跳过，零失败。两树三配置语料各 510 项零回归，同一 Lisp 归档跨 provider 通过；独立安装/搬迁 full/retain 消费各 7/7。知识生成/检查通过。性能样本、身份/额度/数组/资源边界及完整验证见父仓库 [allocator 验收](../docs/audit/opengc_allocator.md)。生成码内联分配未启用。

## 最小 policy 与反向服务（2026-11-18）

- 文档先修订，随后将单体拆为 policy.h/marksweep.cpp/nogc.cpp、heap3_allocator.cpp 和 heap3_runtime.cpp；独立 policy 只需 6 个算法入口。allocator 拥有唯一对象登记，出生/存活操作共享内联预设，不复制 GC 对象表。
- 相同 C99 宿主对象文件分别只链接 policy_marksweep/policy_nogc 核心；反向提供自己的根、引用、nursery 与退役，完整/代际/SATB/NoGC 判据通过。核心无对象分配或 runtime 符号依赖。
- 两树完整构建/安装通过；marksweep 全量 CTest 109/109，bump 105 通过、4 项按 NoGC 能力跳过，零失败。普通/无优化/压力各 510 语料零回归，同一 Lisp 归档跨默认 provider 消费通过。
- 独立安装/搬迁 full/retain 两套各 7/7；新核心目标也经安装消费。知识页生成与检查、diff 检查通过。
- 反向访问有成本：同轮短命分配增加约 1%/2%，存活链表 marksweep 增加约 26%。不把拆分当性能优化；样本和边界见 [职责拆分验收](../docs/audit/opengc_policy.md)。
- 移动式只保留可写边/偏移/固定/重定位合同，当前默认 allocator 与核心不声明 MOVING。Windows/Linux 本轮未执行。

## 原生统一 heap ABI 3（2026-11-18）

macOS arm64、Release，复用父仓库 build/release 与 build/bump。
原生 marksweep/bump 不调用 ABI 2、不扫描机器栈；默认 Lisp 编译/运行/加载/缓存已迁移。
移动式只规定协议，不实现移动 collector 或搬迁测试 provider。

- 两树完整 build-posix 构建与安装成功。
- 同一 C99 归档分别链接两个原生 provider；条件根、weak、no-GC、WALK CODE/标量区别及回调 busy 均有判据。
- lisp_gc_portable_package 已在两个构建执行：同一 Lisp AOT 对象/静态归档及 rt/link/runtime 三归档哈希不变，分别最终链接 marksweep/bump，结果均为 144；每个程序仅一个 provider。
- 独立安装/搬迁检查 `python3 opengc/scripts/check-package.py --build-dir build/release/opengc` 成功，full/retain 两套安装消费者各 5/5，其中包含 ABI 2 显式兼容消费与两个 ABI 3 原生消费。
- 最终完整 CTest：marksweep 107/107，bump 103 通过、4 项 NoGC 收集旋钮探针跳过，均零失败；两树普通/无优化/压力各 510 语料对 baseline 零回归。知识页生成/校验及 diff 检查通过。中间失败、重跑与性能样本见父仓库 [ABI 3 验收](../docs/audit/opengc_abi3.md)。历史适配阶段的结果不用于证明本次原生实现。
- nm 默认 lisp 只有 ogc_portable_query，没有 ogc_heap_*；ABI 2 原实现只保留在显式兼容目标。

迁移时一百万次短命分配的中位数：marksweep 由 2.906 ms 至 138.638 ms，bump 由 4.343 ms 至 119.133 ms。后续同轮优化对比为 132.340→54.007 ms、93.796→29.902 ms，见 [优化验收](../docs/audit/opengc_abi3_optimization.md)。
这是不同 allocator/metadata/精确根/屏障的总路径成本，不能全归于关闭内联后的函数调用；纯算术基本持平。
Windows/Linux 本轮未执行；无 build/linux/WSL，不创建临时本机树冒充 Linux。
任务创建的性能基线 worktree 已移除，长期构建树保留。

## 执行栈范围（2026-11-16）

Windows x64、clang-cl 19.1.5、Release：独立构建 CTest 9/9，零跳过；marksweep/bump 各执行 64 项栈 scope 判据。
三层真实 fiber 切栈验证活动栈与显式暂停根、collector/Walk 回调内 Busy、LIFO/外线程拒绝、返回后收集和资源释放。
full/retain 两套搬迁消费者各 3/3，C99 heap consumer 调用新增 scope 入口。POSIX ucontext 路径本轮未执行。
父仓库的双 provider 完整测试及 419 例语料三配置见 [执行栈验收](../docs/audit/task_stack_roots.md)。这不是任务 trap 恢复验收。


## D6 NoGC bump（2026-11-15）

Windows x64 clang-cl Release 独立构建 CTest 7/7、零跳过；marksweep 合同 120、bump 合同 55。full/retain 两套搬迁消费者各 3/3，包含分别只链接 marksweep/bump 的 C99 heap consumer；OpenGC 不依赖父仓库。父仓库两种 provider 各 41/41 CTest、303 项语料三配置，JIT/AOT 双向真实错配拒绝各 26 项。
这些记录不代表 Linux/macOS 或 sanitizer 本轮已执行。

## D2 完整根遍历扩展（2026-11-15）

新增 `ogc_heap_walk_roots`，不改 `OgcWalk` 布局；选择性遍历保持不变。Windows x64 clang-cl Release 独立构建 CTest 5/5，marksweep 合同 120 检查；full/retain 搬迁消费构建各 2/2，C99 heap consumer 实际调用完整根遍历。
父仓库默认语言堆禁用 RETAG；布局准入与 AST→MIR 的无 meta 目录由专门单元测试固定。详细记录见父仓库 `docs/audit/layout_updates.md`。

## 0.2.0 / heap ABI 2（2026-11-15）

Windows x64、clang-cl、Ninja、Release 独立构建；provider 保留 /W4 /WX /EHs-c-。CTest 5/5：通用协议、实例 marksweep、C99 heap consumer、C/C++ embedding。
`check-package.py` 安装后搬迁到新 prefix，full/retain 消费构建各 2/2（通用协议 + 只链 marksweep 的 C99 heap consumer），不引用父仓库库。
实例测试覆盖双堆隔离、外来 mutator/描述符、可选能力禁用、额度、Busy 关闭、大小对象 retag 和反复资源销毁；父仓库另测三目标能力门控及 Win64 原生 JIT/AOT 屏障绑定。
临时树 `build/d5-opengc-instance-20261115` 验收后删除。详细记录见父仓库 `docs/audit/heap_instances.md`；macOS/Linux 本修订未执行。

## 0.1.0 / 通用协议 ABI 1 历史记录

环境：2026-11-05，macOS arm64，AppleClang 21.0.0，Ninja，64 位静态库。
临时构建树均位于任务独占的 `/tmp/opengc-reference-20261105-*`，验收后删除，不作为交付依赖。

## 独立构建与合同

从父仓库根目录执行：

```sh
OPENGC_BUILD_TREE=/tmp/opengc-reference-20261105-build \
  opengc/scripts/config-posix.sh -DCMAKE_BUILD_TYPE=Release
OPENGC_BUILD_TREE=/tmp/opengc-reference-20261105-build \
  opengc/scripts/build-posix.sh -j 2
ctest --test-dir /tmp/opengc-reference-20261105-build --output-on-failure
python3 opengc/scripts/check-package.py --build-dir /tmp/opengc-reference-20261105-build
```

结果：库与 C++ 测试编译启用 `-Wall -Wextra -Wpedantic -Werror -fno-exceptions`；CTest 3/3 通过。
其中 `opengc.contract` 的 9 组合同全部通过，`opengc.c_embedding` 验证真正 C99 客户端，
`opengc.cpp_embedding` 验证 C++20 Node、显式根与循环回收。两份 Demo 的实际输出与
[DEMO.md](DEMO.md) 一致，对象数为 3 → 2 → 0，payload 字节数为 48 → 32 → 0。
包安装后搬迁到新目录，Full／Retain 两个客户端分别只链接所选 provider，2/2 消费者通过。

| 合同组 | 关键 oracle |
|---|---|
| ABI / allocator | 短表、未知能力、major/minor、32 位宽度拒绝；对齐、溢出、失败输出保留 |
| 原子发布与身份 | 半组互引发布失败不泄露成员；整组发布；撤根后整环死亡；旧 ID 不复用 |
| 域 / 布局 / 根 | 外域和内部地址充当精确边拒绝；Interior 保活与非命中；重复槽、越界槽拒绝 |
| 压力与重入 | data/meta/scratch 失败分别传播；回滚字节配平；回调 Busy；失败收集可重试 |
| retain / owner | no-GC 不扫描不退休；依旧检查写入；错误线程不改输出、不销毁域 |
| 独立 provider | 外部服务的扫描失败、非法根、错误数量；begin/end 配平；退休失败报告前缀 |
| 损坏图 | 外来写入破坏一个可达槽后全轮零退休；修复后正常回收不可达对象 |
| 长图 | 3000 个对象迭代追踪，撤根后全部退休，无递归依赖 |
| 图可达性对照 | 12 个种子、每图 128 节点；独立整数邻接表 BFS 与每个实际对象的保留结果逐项一致 |

参考 data/meta allocator 故障注入还检查正常回收与关闭后请求字节归零，验证原来源配对。
这不是操作系统级 leak checker 的替代。

## Sanitizer

```sh
OPENGC_BUILD_TREE=/tmp/opengc-reference-20261105-asan \
  opengc/scripts/config-posix.sh -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
OPENGC_BUILD_TREE=/tmp/opengc-reference-20261105-asan \
  opengc/scripts/build-posix.sh -j 2
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/opengc-reference-20261105-asan --output-on-failure
```

结果：ASan + UBSan，3/3 通过，无报告。
最初以 `detect_leaks=1` 启动时，本机 Apple ASan 报 `detect_leaks is not supported on this platform`
并在测试执行前中止；改用 0 后完成验证。**LeakSanitizer 未验收**。

## 链接边界

`nm -u` 检查 Release archive：domain 只有 bzero/memcpy/pthread_self 外部引用，full 只有 qsort。
没有具体 allocator/provider 工厂、Lisp、Runtime、Foundation 或编译器符号依赖。
安装消费未使用父仓库源头或构建产物。

## 父仓库基线

父仓库原本存在大量未提交代码与文档。开始本任务时已有其他构建进程使用 `build/release`，
本任务没有配置、构建或修改该共享树；在其构建结束后，仅使用现有二进制运行以下只读回归。
这些结果描述当时的工作树构建，**不能视为本任务重新构建了整个 Lisp 项目**。

```sh
python3 tools/corpus.py --case build/release/test/lisp_corpus_case \
  --baseline test/corpus_baseline.json --jobs 2
python3 tools/gtest_each.py build/release/test/lisp_core_tests \
  --baseline test/gtest_baseline.json --jobs 2
python3 tools/knowledge_base.py
python3 tools/knowledge_base.py --check
```

- Corpus：1074 项，OK 472、FAIL 602；相对基线 47 fixed、**0 regressed**。
- GoogleTest：2457 项，OK 2135、FAIL 86、CRASH 236；相对基线 20 fixed、**0 regressed**。
- 已有失败/崩溃不能描述为全绿；本次没有修改基线。
- 知识生成器增加 OpenGC 扫描根和独立模块 notes，生成文件不可手改。

所用现有二进制的 SHA-256：

```text
lisp_core_tests   2f108f64c7e7bbda17ed767b2bea457d07943254f8ee0be4e016f601ef477cb5
lisp_corpus_case  8baa68a22c8350cc45b7463a057848e791a2ef0e8a97ab52905dfe16bb078dc1
```

## 未执行

Linux/Windows、x64、共享库与动态装卸、ThreadSanitizer、LeakSanitizer、真实 RSS/吞吐/暂停时间
基准、生产 Lisp Runtime/native/fiber 接线尚未验收。增量/分代/并发/移动不属于本实现，不能列为
通过。未执行父仓库全量重新构建；没有创建 commit、push 或发布包。
