<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 用 C / C++ 写一个 OpenGC 小示例

两个版本都通过真正的 OpenGC 库创建三个对象：A 与 B 互相引用，orphan 没有入边。
给 A 登记根后第一次 GC 保留 A/B、回收 orphan；撤销根后第二次 GC 回收整个 A/B 环。
这说明循环引用不妨碍追踪式 GC，普通局部指针变量则不会自动成为根。

- C99：[examples/graph.c](examples/graph.c)，显式布局与整组发布。
- C++20：[examples/graph.cpp](examples/graph.cpp)，标准布局 `Node`、placement new 和 typed helper。

在 `opengc/` 目录执行：

```sh
sh scripts/config-posix.sh -DCMAKE_BUILD_TYPE=Release
sh scripts/build-posix.sh
./build/opengc_graph
./build/opengc_graph_cpp
ctest --test-dir build --output-on-failure
```

典型 64 位输出（C++ 版多打印一行对象值）：

```text
C before collection    objects=3 roots=1 data_bytes=48
C root keeps A <-> B   objects=2 roots=1 data_bytes=32
C after removing root  objects=0 roots=0 data_bytes=0

C++ before collection    objects=3 roots=1 data_bytes=48
C++ root keeps A <-> B   objects=2 roots=1 data_bytes=32
C++ rooted values: 10 + 20 = 30
C++ after removing root  objects=0 roots=0 data_bytes=0
```

字节数为请求的 Node payload，不包含 GC 元数据或 allocator 开销。每个示例不仅打印，还检查
实际保活/回收数量和保活对象的值；结果不符时进程返回非零。

需要关注的接口顺序：

1. `ogc_query_full` 选择 collector，`ogc_domain_create` 显式组合 data/meta allocator。
2. `ogc_construct` 建立隐藏对象；`OgcLayout.reference_offsets` 只声明 `next`，数值字段不扫描。
3. 初始化数值，经 `ogc_store` 建立引用，再 `ogc_publish_batch` 发布。C++ helper 先分别发布空引用
   节点，再连成环；C 版展示未发布的互引对象如何整组发布。
4. `ogc_root_create` 保活 A；`ogc_collect` 仅在这里显式运行。创建和写入从不偷偷收集。
5. `ogc_root_remove` 撤根，再显式收集，最后 `ogc_domain_destroy` 配对释放其余元数据。

C++ 对象必须可平凡析构，本例用 static_assert 检查；OpenGC V1 不调用 C++ 析构函数。
不要把 `std::string`、文件句柄或其他需要析构的资源直接放进这个示例的受管 payload。
所有受管内存都不能自行 `free`/`delete`；对象被回收后，旧 base 和 Node 指针必须停止使用。
