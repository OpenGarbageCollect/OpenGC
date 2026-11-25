<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# OpenGC

[English](README.md) · [简体中文](README.zh-CN.md) · 日本語

OpenGC は、単体で利用できるガベージコレクションのプロトコルと参照実装です。バージョン **0.2.0** のビルドには 64 ビットの C++20 ツールチェーンとシステムライブラリを使用します。公開 API は C99 に対応しています。

## プロトコルとプロバイダー

- **汎用プロトコル（ABI 1）**。アプリケーションがドメイン、アロケーター、プロバイダーを明示的に組み合わせます。`full` はマーク＆スイープ方式で回収し、`retain` は参照をたどらずにオブジェクトを保持します。プロトコルは Lisp から独立しています。
- **GC ポリシー**。`policy.h` の 6 つのエントリーポイントは、アロケーターとランタイムが提供するコールバックを使って GC 用のレコードにアクセスし、参照を列挙します。オブジェクトの登録情報はアロケーターが管理し、ルート、ハンドル、型メタデータはランタイムが管理します。
- **ランタイムプリセット（ABI 3、オブジェクトモデル 3）**。`portable.h` のサービステーブルは、書き換え可能な正確なルート、参照バリア、型と識別子の照会、ヒープの走査、ハンドルとピン留め、弱参照、GC を抑止するスコープを提供します。両プリセットは正確なルートを使用し、マシンスタックを走査しません。プロトコルはオブジェクトを移動する GC の要件も定めています。付属の marksweep と bump はオブジェクトを移動しません。
- **互換ヒープ（ABI 2）**。ABI 2 専用ターゲットは、従来のミューテーターのレイアウトと保守的なスタック走査を維持します。

最終リンク時にヒーププロバイダーを 1 つ選択します。同じプロバイダーから、所有者が独立した複数のヒープを作成できます。生の参照は元のヒープ内でのみ有効で、稼働中のヒープは作成時のプロバイダーを使い続けます。インストール済みパッケージでは、`OPENGC_HEAP_PROVIDER` に `marksweep` または `bump` を指定して `OpenGC::heap` のプロバイダーを選択できます。

## ビルドと実行

CMake 3.20 以降、Ninja、64 ビットの C++20 ツールチェーンを使用します。POSIX 環境では、リポジトリのルートから次のコマンドを実行します。

```sh
sh scripts/config-posix.sh -DCMAKE_BUILD_TYPE=Release
sh scripts/build-posix.sh
ctest --test-dir build --output-on-failure
build/opengc_graph
build/opengc_graph_cpp
cmake --install build --prefix /absolute/path/to/opengc-install
```

ビルド先を変えるには `OPENGC_BUILD_TREE` を設定します。CMake の追加オプションは設定スクリプトに渡せます。Windows で CMake、Ninja、clang-cl を使った検証結果は [検証記録](VALIDATION.md) を参照してください。

## インストール済みパッケージを使う

次の例では、C のホストアプリケーションに ABI 1 のマーク＆スイーププロバイダーをリンクします。ホストを構成するときに、インストール先を `CMAKE_PREFIX_PATH` に追加します。

```cmake
project(MyHost LANGUAGES C CXX)
find_package(OpenGC 0.2 CONFIG REQUIRED)
add_executable(my_host main.c)
target_link_libraries(my_host PRIVATE OpenGC::domain OpenGC::allocator OpenGC::full)
```

`ogc_query_full` で取得したプロバイダーを `OgcDomainConfig` に渡します。`retain` を使う場合は `OpenGC::retain` をリンクし、`ogc_query_retain` を呼び出します。C++ で実装された参照ライブラリをリンクするため、ホスト側では C と C++ の両方を有効にします。[C と C++ のデモ](DEMO.md) では、ルートによるオブジェクトの保護と、ルートを外した後の循環参照の回収を確認できます。

| CMake ターゲット | 用途 |
|---|---|
| `OpenGC::protocol` | C99 のプロトコルテーブル、バージョン交渉、レコード、エラーコード |
| `OpenGC::domain` | オブジェクトの識別、構築と公開、ルート、検査、終了処理 |
| `OpenGC::allocator` | システムメモリの確保と解放 |
| `OpenGC::full`、`OpenGC::retain` | ABI 1 のマーク＆スイープ方式と、参照をたどらない保持方式 |
| `OpenGC::policy_marksweep`、`OpenGC::policy_nogc` | アロケーターとランタイムのコールバックを利用する独立ポリシー |
| `OpenGC::marksweep`、`OpenGC::bump` | 領域を回収するアロケーターと、GC を行わないアリーナをそれぞれ使う ABI 3 プリセット |
| `OpenGC::heap` | 選択した ABI 3 プリセット |
| `OpenGC::abi2_marksweep`、`OpenGC::abi2_bump` | ABI 2 専用の互換プロバイダー |

## ドキュメント

- [プロトコル仕様](PROTOCOL.md) と [ABI 3 のポータブルヒープ仕様](PROTOCOL.md#portable-heap)
- [設計資料](DESIGN.md)
- [C と C++ のデモ](DEMO.md)
- [検証記録とプラットフォーム対応](VALIDATION.md)

## ライセンス

Copyright (C) 2026 OpenGC contributors.

OpenGC は **GPL-3.0-or-later** で提供されます。ライセンスの全文は [COPYING](COPYING) を参照してください。
