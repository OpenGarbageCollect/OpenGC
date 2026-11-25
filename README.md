<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# OpenGC

English · [简体中文](README.zh-CN.md) · [日本語](README.ja.md)

OpenGC is a standalone garbage collection protocol and reference library. Version **0.2.0** builds with a 64-bit C++20 toolchain and system libraries. The public API is compatible with C99.

## Protocols and providers

- **General protocol (ABI 1).** Applications explicitly assemble a domain, an allocator, and a provider. The `full` provider performs mark-and-sweep collection, and `retain` retains objects without tracing. The protocol is independent of Lisp.
- **Collector policies.** The six entry points in `policy.h` use callbacks supplied by the allocator and runtime to access collector records and enumerate references. The allocator owns the object registry. The runtime owns roots, handles, and type metadata.
- **Runtime presets (ABI 3, object model 3).** The service table in `portable.h` provides precise writable roots, reference barriers, type and identity queries, heap walking, handles and pinning, weak references, and no-GC scopes. Both presets use precise roots and leave the machine stack unscanned. The protocol specifies contracts for moving collectors. The supplied marksweep and bump presets are nonmoving.
- **Compatibility heap (ABI 2).** The explicit ABI 2 targets preserve the older mutator layouts and conservative stack scanning behavior.

A program selects one heap provider at final link. A provider can create multiple independently owned heaps. A raw reference belongs to its original heap, and each live heap keeps its provider for its lifetime. Consumers of an installed package can set `OPENGC_HEAP_PROVIDER` to `marksweep` or `bump` to select the provider behind `OpenGC::heap`.

## Build and run

Use CMake 3.20+, Ninja, and a 64-bit C++20 toolchain. Run these commands from the repository root on a POSIX system.

```sh
sh scripts/config-posix.sh -DCMAKE_BUILD_TYPE=Release
sh scripts/build-posix.sh
ctest --test-dir build --output-on-failure
build/opengc_graph
build/opengc_graph_cpp
cmake --install build --prefix /absolute/path/to/opengc-install
```

Set `OPENGC_BUILD_TREE` to use a different build directory and pass additional CMake options to the configuration script. The [validation notes](VALIDATION.md) also cover Windows with CMake, Ninja, and clang-cl.

## Use an installed package

This example links the ABI 1 mark-and-sweep provider into a C host. Add the installation prefix to `CMAKE_PREFIX_PATH` when configuring the host.

```cmake
project(MyHost LANGUAGES C CXX)
find_package(OpenGC 0.2 CONFIG REQUIRED)
add_executable(my_host main.c)
target_link_libraries(my_host PRIVATE OpenGC::domain OpenGC::allocator OpenGC::full)
```

Call `ogc_query_full` and pass the resulting provider in `OgcDomainConfig`. To use `retain`, link `OpenGC::retain` instead and call `ogc_query_retain`. Enable both C and C++ so the host can link the C++ reference libraries. The [C and C++ demos](DEMO.md) show how to protect objects with a root and reclaim a cycle after removing the root.

| CMake target | Purpose |
|---|---|
| `OpenGC::protocol` | C99 protocol tables, version negotiation, records, and error codes |
| `OpenGC::domain` | Object identity, construction and publication, roots, inspection, and shutdown |
| `OpenGC::allocator` | System storage allocation and release |
| `OpenGC::full`, `OpenGC::retain` | ABI 1 mark-and-sweep and non-tracing providers |
| `OpenGC::policy_marksweep`, `OpenGC::policy_nogc` | Standalone policies driven by allocator and runtime callbacks |
| `OpenGC::marksweep`, `OpenGC::bump` | ABI 3 presets with a reclaiming allocator or a NoGC arena, respectively |
| `OpenGC::heap` | Selected ABI 3 preset |
| `OpenGC::abi2_marksweep`, `OpenGC::abi2_bump` | Explicit ABI 2 compatibility providers |

## Documentation

- [Protocol specification](PROTOCOL.md) and the [ABI 3 portable heap contract](PROTOCOL.md#portable-heap)
- [Design notes](DESIGN.md)
- [C and C++ demos](DEMO.md)
- [Validation and platform coverage](VALIDATION.md)

## License

Copyright (C) 2026 OpenGC contributors.

OpenGC is licensed under **GPL-3.0-or-later**. See [COPYING](COPYING) for the full terms.
