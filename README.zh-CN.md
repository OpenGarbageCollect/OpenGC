<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# OpenGC

[English](README.md) · 简体中文 · [日本語](README.ja.md)

独立的垃圾回收协议及参考库，包版本 **0.2.0**，只依赖 64 位 C++20 与系统库，公共接口兼容 C99。

- **通用协议 ABI 1**：domain/allocator/full/retain 的显式组装接口，不隐式回收，不依赖 Lisp。
- **最小 GC policy**：policy.h 的 6 个核心入口，通过反向服务访问 allocator/runtime 的记录与引用。
  OpenGC::policy_marksweep / OpenGC::policy_nogc 不拥有对象登记、类型表、根表或 Handle，不链接语言 heap 外观。
- **默认 runtime 预设 ABI 3 / 模型 3**：portable.h 保留统一服务表，组合 marksweep 或 NoGC 核心与 allocator；
  预设承接精确可写根、引用屏障、类型/身份查询、WALK、Handle/pin、弱引用及 no-GC 窗口。
  两者均不扫描机器栈。Lisp 编译器、运行库、Object/AOT 与缓存使用此路径。
- **兼容 heap ABI 2**：heap.h 与 provider mutator 头，仅由显式 OpenGC::abi2_marksweep/bump 提供，
  保留原有内联布局与保守栈扫描合同。默认语言堆不包装或调用它。

最终程序只链接一个 provider；同一 provider 可建多个独立 owner 堆，不允许跨堆裸引用或活堆换算法。
同一份 Lisp lib 和运行库可在最终链接时选择 marksweep/bump，安装消费者以 OPENGC_HEAP_PROVIDER 设置 OpenGC::heap。
provider 私有布局不是产物 ABI。移动式只规定协议，不实现移动收集器，也不宣称已有搬迁执行证据。
详细合同见 [PROTOCOL §9](PROTOCOL.md#portable-heap)，验证与性能见 [VALIDATION](VALIDATION.md)。

## 构建和运行

要求 CMake 3.20+、Ninja、64 位 C++20 工具链，公共头及示例兼容 C99。

```sh
sh scripts/config-posix.sh -DCMAKE_BUILD_TYPE=Release
sh scripts/build-posix.sh
ctest --test-dir build --output-on-failure
build/opengc_graph
build/opengc_graph_cpp
cmake --install build --prefix /absolute/path/to/opengc-install
```

可用 `OPENGC_BUILD_TREE` 指定独立构建树，其他 CMake 选项通过配置脚本传入。
Windows CMake + Ninja + clang-cl 的验收记录见 [VALIDATION](VALIDATION.md)。

安装后消费：

```cmake
project(MyHost LANGUAGES C CXX)
find_package(OpenGC 0.2 CONFIG REQUIRED)
add_executable(my_host main.c)
target_link_libraries(my_host PRIVATE OpenGC::domain OpenGC::allocator OpenGC::full)
```

应用必须显式 query 并在 `OgcDomainConfig` 中选择 provider；导入 `domain` 不会选择收集器。
选择 retain 时链接 `OpenGC::retain`，调用 `ogc_query_retain`。纯 C ABI 客户端仍需由 C++ 链接器
链接参考实现，CMake 中同时启用 C、CXX 即可。

以上是通用协议 ABI 1 的组装示例。独立 policy 消费者链接 policy_marksweep 或 policy_nogc，反向提供自己的 allocator/runtime 服务；现有 Lisp 消费者链接 ABI 3 预设，最终选择 marksweep/bump，无须随默认 provider 重编产物。

| 目标 | 职责 | 依赖 |
|---|---|---|
| `OpenGC::protocol` | C99 表、版本协商、固定记录和错误码 | 仅标准 C 头 |
| `OpenGC::allocator` | 系统存储分配与回条释放 | protocol、C 标准库 |
| `OpenGC::domain` | 对象身份、隐藏构造、原子发布、根、检查、关闭 | protocol、C++ 线程身份 |
| `OpenGC::full` | 独立完整 mark/sweep provider | protocol、C 标准库 |
| `OpenGC::retain` | 独立 no-tracing provider | protocol |
| `OpenGC::policy_nogc` | 独立不追踪策略，不分配存储、不持有对象表 | protocol |
| `OpenGC::bump` | ABI 3 runtime 预设：arena allocator + NoGC policy，根/Handle/WALK 等归预设 | policy_nogc、标准库、Threads |
| `OpenGC::policy_marksweep` | 独立 epoch/SATB/分代决策，只操作 allocator 记录与反向服务 | protocol、标准库计时 |
| `OpenGC::marksweep` | ABI 3 runtime 预设：reclaiming allocator + marksweep policy | policy_marksweep、标准库、Threads |
| `OpenGC::abi2_marksweep/bump` | 显式 ABI 2 兼容实现，保留旧 mutator 布局与保守扫描 | protocol、标准库、Threads |

没有通配符源码清单，没有第三方下载或构建时生成源码。参考实现不使用 C++ 异常或 STL 容器。
示例见 [C / C++ Demo](DEMO.md)：建立互引对象、根保护、回收无根对象、撤根后回收整环。

## 文档

- [PROTOCOL.md](PROTOCOL.md)：规范性 ABI、服务和生命周期合同。
- [DESIGN.md](DESIGN.md)：设计来源、实现边界、成本和扩展路线。
- [VALIDATION.md](VALIDATION.md)：本次实际验证与平台限制。
- `include/opengc/policy.h`：最小核心与 allocator/runtime 反向服务合同。
- `include/opengc/portable.h`：ABI 3 共享 runtime 预设外观。
- `include/opengc/protocol.h`：既有通用 domain ABI 1 的 collector 表。
- `include/opengc/opengc.h`：可选的参考对象域与 allocator API。

## 授权

Copyright (C) 2026 OpenGC contributors.

本目录的原创协议文本、公共头、实现、测试、示例和构建文件均以 **GPL-3.0-or-later** 授权。
This program is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details. 完整条款见 [COPYING](COPYING)。
授权标记与许可证副本依据 [GNU 授权指南](https://www.gnu.org/licenses/gpl-howto.en.html)。

核心职责与独立 allocator 验证见 [PROTOCOL §9.9](PROTOCOL.md#99-收敛职责反向服务与共享预设)。bump 是默认分配预设的名称，核心 NoGC 不负责 arena 分配。

默认 allocator 已改为块登记、侧置记录和 frontier 新槽分配；NoGC 无逐对象 GC record。分配器合同见 [PROTOCOL §9.10](PROTOCOL.md#910-allocator-块级预设本轮重构)。生成码内联分配仍关闭。

marksweep 以保守完整标记 epoch 摘要加速整块死亡复用；混合块/资源仍走原有清扫/终结路径。实现边界与验证记录见 [VALIDATION](VALIDATION.md)。
