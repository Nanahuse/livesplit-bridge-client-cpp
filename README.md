# livesplit-bridge-client-cpp

[LiveSplit.Bridge](https://github.com/Nanahuse/LiveSplit.Bridge) の JSON Protocol v4
に対応する、単体で利用できる C++23 クライアントライブラリです。GUI フレームワークや
特定のアプリケーションには依存しません。

- RPC と Events の 2 接続を内部で管理
- Query 6 種類、Timer Control 7 種類、Game Time 4 種類
- 全 8 種類の Event と `session_id` / `event_sequence` の連続性検証
- 切断後の自動再接続、Session 変更・Event 欠落時の自動再同期
- Control の結果不明時に自動再送しない
- 同期 API と `std::future` ベースの非同期 API
- 公開 API に Boost.Beast / Boost.Asio の型を出さない
- 公式 JSON Schema から生成した通信モデルを同梱

設計と復旧の詳細は [docs/architecture.md](docs/architecture.md) を参照してください。

## 必要環境

| 用途 | 要件 |
| --- | --- |
| 言語 | C++23 |
| ビルド | CMake 3.25 以上 |
| 依存管理 | vcpkg manifest |
| 依存ライブラリ | Boost.Asio / Boost.Beast / nlohmann/json |
| テスト | Catch2 3、CTest |
| モデル生成 | Python 3 |
| TestHost 結合テスト | Python 3 |

## ビルド

`VCPKG_ROOT` を設定した Developer PowerShell で次を実行します。

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -E testhost
```

`LIVESPLIT_BRIDGE_TESTHOST` に公式 TestHost の実行ファイルを設定している場合は、
`-E testhost` を外すと結合テストも実行されます（後述）。

ライブラリのみをビルドする場合:

```powershell
cmake --preset user
cmake --build --preset user
```

## 他の CMake プロジェクトからの利用

インストール後、`find_package` で導入できます。

```powershell
cmake --install build/release --prefix C:/livesplit-bridge-client
```

```cmake
find_package(livesplit_bridge CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE livesplit_bridge::livesplit_bridge)
```

`add_subdirectory` / `FetchContent` で組み込むこともできます。

```cmake
add_subdirectory(external/livesplit-bridge-client-cpp)
target_link_libraries(my_app PRIVATE livesplit_bridge::livesplit_bridge)
```

## 使い方

```cpp
#include <chrono>
#include <iostream>

#include <livesplit/bridge/client.hpp>
#include <livesplit/bridge/icons.hpp>

int main() {
    using namespace std::chrono_literals;
    using namespace livesplit::bridge;

    client_options options;
    options.port = 54000;  // クライアントインスタンスごとに指定
    client bridge(options);

    // 接続・Events 購読・初回同期が完了するまで待機する。
    bridge.wait_until_ready(10s);

    const snapshot state = bridge.current_snapshot();
    std::cout << state.run.game_name << '\n';

    if (bridge.get_timer_state().phase == timer_phase::NOT_RUNNING) {
        bridge.start();
    }

    while (auto event = bridge.next_event(250ms)) {
        const bool reset = std::holds_alternative<reset_event>(*event);
        if (reset) {
            const auto completed = bridge.get_completed_count().completed_count;
            std::cout << "completed: " << completed << '\n';
        }
    }
}
```

非同期 API は送信キューへ積んで即座に戻ります。

```cpp
auto run_future = bridge.get_run_async();
auto icons_future = bridge.get_run_icons_async();
const run_state run = run_future.get();
const run_icons icons = icons_future.get();
```

アイコンは Base64 文字列として取得し、必要なら PNG bytes に変換できます。

```cpp
const run_icons wire = bridge.get_run_icons();
if (wire.game_icon) {
    const auto png = decode_base64(*wire.game_icon);
    // png は PNG ファイルの bytes
}
const decoded_run_icons decoded = decode_run_icons(wire);
```

### 同期と状態

- コンストラクタはワーカースレッドを開始し、自動的に接続・同期します。
- `wait_until_ready()` は初回同期、または自動再接続後の再同期完了まで待機します。
- `synchronize()` はアイコンを除く 5 種類の Query を同一 Session で実行し、Snapshot を
  更新します。同期中の Event に応じて必要な Query を追加します。全 Query が同一時点の
  値である保証はありません。
- `current_snapshot()` は最後に同期した Snapshot を返します。Timer State は受信した
  Timer Event でも更新されます。`not_ready` の間は例外になります。
- `get_run()` と `get_run_icons()` は別要求です。間に Run が切り替わる可能性があります。
- 各 Query / Control / `synchronize` には `_async()` 版があり、`std::future` を返して
  呼び出し元をブロックしません。同期版はその便利ラッパーです。
- Event は同期中も検証・配送を継続します。同期中に Event が届いた場合は変更された
  状態だけを再取得し、古い Snapshot で Ready にならないようにします。
- `sync_timeout` 内に状態が安定しない場合は `unstable_state` として中断し、再接続します。

### 再接続・再同期

- RPC / Events の切断後はバックオフ付きで自動再接続し、両接続が揃ってから再同期します。
- Event の `session_id` 変更、RPC と Events の Session 不一致では両接続を再接続します。
- Event の Session 不一致、`event_sequence` の欠落・重複・逆順、不正 Event では
  接続を維持したまま Query による再同期を行います。再同期中も Event の検証・配送を
  継続し、変更通知が届いた場合は変更された状態だけを再取得します。`sync_timeout` 内に
  安定しない場合は `unstable_state` として中断し、再接続します。
- 内部の Event キューは 4,096 件で上限です。利用側が消費しないまま溢れた場合、
  `dropped_event_count()` が増加します。値が増えたら Query で現在値を再取得してください。
- Events はベストエフォートです。履歴再送はないため、Event は状態再取得のきっかけとして
  扱ってください。
- Control がタイムアウト、送信後に通信異常になった場合、または送信済みのまま
  `close()` した場合は、`error_kind::control_result_unknown` を返し、自動再送しません。
  再接続・再同期後に Query で結果を確認してください。

### エラー

同期 API は `livesplit::bridge::error` を投げ、非同期 API は `std::future` に同じ例外を
設定します。`kind()` で分類できます。

| `error_kind` | 内容 |
| --- | --- |
| `closed` | クライアントが閉じられている |
| `timeout` | 応答待ち、または操作受付待ちのタイムアウト |
| `not_ready` | 同期済み Snapshot がない |
| `connection_lost` | 通信切断（自動再接続の対象） |
| `protocol_error` | 不正な frame / JSON / Request ID / 結果型 |
| `remote_error` | Bridge が返した構造化エラー。`remote()` に code / message |
| `control_result_unknown` | Control の実行結果が不明。再送しない |
| `unstable_state` | 同期中に状態が変わり続け、`sync_timeout` 内に安定しなかった |
| `internal_error` | クライアント内部の不整合 |

### Origin

既定では Origin ヘッダーを送らないため、ローカルクライアントとして Control を利用できます。
ブラウザーからの接続を模擬する場合だけ `client_options::origin` を設定してください。
Bridge は localhost 以外の Origin からの Control を `permission_denied` で拒否します。
Query と Events は Origin に関係なく利用できます。

### スレッド

内部の通信スレッドは 1 本です。公開 API は複数スレッドから呼び出せますが、1 つの
`client` インスタンスに対する操作は内部で直列化されます。非同期 API は送信キューへ
積んで即座に戻ります。`next_event()` と Query を同じスレッドで交互に呼ぶ必要は
ありません。

## CLI

`livesplit_bridge_cli` はライブラリ本体から独立した確認用ターゲットです。

```text
livesplit_bridge_cli [--host HOST] [--port PORT] [--timeout MS] [--ready-timeout MS] COMMAND

sync | timer | run | icons [--output DIR] | attempt | context | completed
start | split | skip | undo | reset | pause | resume
game-init | game-set TICKS | game-pause | game-resume
events [--count N]
```

例:

```powershell
build\dev\livesplit_bridge_cli.exe --port 54000 sync
build\dev\livesplit_bridge_cli.exe --port 54000 icons --output .\icons
build\dev\livesplit_bridge_cli.exe --port 54000 events --count 10
```

## モデル生成

通信モデルは固定した公式 Schema から Python 標準ライブラリのみで生成し、
`include/livesplit/bridge/protocol.hpp` として Git 管理しています。Schema 本体は
リポジトリに保存せず、生成時に固定コミットから取得してメモリ上で検証します。
通常のビルドに Python は不要です。

```powershell
python tools/generate_models.py
python tools/generate_models.py --check
# 固定コミットのローカル checkout を使う場合
python tools/generate_models.py --source C:\path\to\LiveSplit.Bridge
```

quicktype の適合性確認結果と生成規則は
[docs/model-generation.md](docs/model-generation.md) を参照してください。

## テスト

通常のテストはネットワーク外部へ接続しません。

- 公式 C# シリアライズ fixture の往復
- 必須・未知・重複プロパティ、enum、整数範囲、nullable、64bit ticks の検証
- Query / Control / Event / 再接続 / 再同期 / タイムアウト / 結果不明
- 64 KiB 超の Response
- 小規模な模擬 WebSocket サーバーによる異常系

公式 TestHost による結合テストは、固定コミットの LiveSplit.Bridge をビルドし、
`LIVESPLIT_BRIDGE_TESTHOST` を設定して実行します。

```powershell
$env:LIVESPLIT_BRIDGE_TESTHOST = "C:\path\to\LiveSplit.Bridge.TestHost.exe"
cmake --preset debug
ctest --preset debug -R testhost --output-on-failure
```

`testhost_integration` と `testhost_integration_icons` の 2 ケースがあります。テストの登録には、設定時の `LIVESPLIT_BRIDGE_TESTHOST` と Python 3 が必要です。
TestHost が未設定の場合はテストは登録されません。

## 制限

- Bridge は `127.0.0.1` のみで待ち受けるため、接続先はローカル PC です。
- 現時点では `ws://` のみ対応し、TLS は使用しません。
- 同期 API と `std::future` ベースの非同期 API を提供します。非同期操作も 1 本の
  ワーカースレッドで直列に処理されます。
- 1 クライアントインスタンスは 1 つの Bridge ポートに接続します。複数接続先は
  インスタンスを分けてください。
- Run Icons は同期 Snapshot に含みません。必要時に `get_run_icons()` を呼び出します。

## ライセンス

MIT License. 通信仕様の出典と fixture の帰属については
[docs/model-generation.md](docs/model-generation.md) を参照してください。
