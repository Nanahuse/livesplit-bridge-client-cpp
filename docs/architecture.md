# 内部アーキテクチャ

## スレッドと I/O

`client` は `client_impl` を `shared_ptr` で保持します。`client_impl` は 1 つの
`boost::asio::io_context`、1 本のワーカースレッド、RPC 用と Events 用の 2 つの
WebSocket 接続を持ちます。

```text
public methods ──post──> io_context worker ──async──> RPC WebSocket
       ^                       │
       └──── promise/future ───┴──async──> Events WebSocket
public next_event <── condition variable ── event queue
```

- 公開 API は standard C++ の型だけを返します。Boost の型は `src/detail/` に閉じています。
- Query / Control は operation queue に積まれ、ready の間だけ 1 件ずつ実行されます。
- RPC は 1 件ずつ送信し、`request_id` の照合と要求 method に対する結果型の検証を
  行います。Request ID は `uint32` で `4294967295` の次は `0` です。
- 対応する `async_write` が完了するまで Response の処理を確定しません。Write 完了前に
  Response が届いた場合は保留し、Write 完了後に処理します。これにより同じ WebSocket に
  対する Write の重複を防ぎます。
- Events は読み取りループで検証・sequence 管理を行い、`next_event()` 用のキューへ
  入れます。キューは上限 4096 件で、超過時は古いものから破棄し、
  `dropped_event_count()` を増やします。

## 状態

```text
connecting ──> synchronizing ──> ready
     ^               │             │
     │               v             v
     └──────── reconnecting <──────┘
```

- 初回は Events、RPC の順に接続し、その後に 5 つの Query を実行して同期します。
  `get_run_icons` はサイズが大きいため同期に含めず、要求時だけ取得します。
- `ready` の間は Query / Control / Event 受信が可能です。同期中も Event の検証・
  配送を継続します。
- 同期中に Event が届いた場合は変更された状態だけを再取得します。Timer Event は
  値を直接反映し、Run / Context / Reset は必要な Query を追加します。`sync_timeout`
  内に安定しない場合は `unstable_state` で中断して再接続します。
- 再接続中と同期中は `current_snapshot()` が `not_ready` になります。
- `close()` はワーカースレッドを停止し、待機中の future と `next_event()` を解放します。
  送信済み Control が残っている場合は `control_result_unknown` として終了します。

## 復旧

| トリガー | 動作 |
| --- | --- |
| RPC 接続の切断・読み書き失敗 | 両接続を閉じ、バックオフ付きで再接続して再同期 |
| Events 接続の切断 | 両接続を閉じ、再接続して再同期 |
| RPC と Events の Session 不一致 | 両接続を再接続 |
| Event の Session 変更 | 両接続を再接続 |
| Event の欠落・重複・逆順・不正 Event | 接続を維持して Query で再同期 |
| RPC Response の Session 変更 | 結果を返した後、両接続を再接続 |
| RPC Response のタイムアウト | RPC 接続を閉じ、再接続して再同期 |

再接続の初回遅延は `client_options::reconnect_initial_delay`、最大値は
`reconnect_max_delay` です。同期成功時にバックオフをリセットします。

Event の再同期では `event_sequence` の基準を解除し、再同期完了後の最初の Event を
新しい基準にします。再同期中も Event は破棄せず、検証・配送を継続します。同期中に
Event が届いた場合は、その Event を配送したうえで変更された状態だけを再取得します。
`sync_timeout` 内に同期が安定しない場合は `error_kind::unstable_state` で中断し、
バックオフ付きで再接続します。

## Control の結果不明

Control は Query と異なり、再送すると二重実行になります。次の場合は
`error_kind::control_result_unknown` を返し、要求を再送しません。

- Response のタイムアウト
- 送信開始後の RPC 切断
- Control に対する不正な Response
- 送信済み Control を残したまま `close()` した場合

呼び出し側は再接続・再同期後に `get_timer_state()` などで結果を確認します。
再接続自体は Control を再送しません。

## 公開 API の待機時間

- Query / Control / `synchronize` は同期版と `_async()` 版を持ちます。非同期版は
  operation queue へ積んで `std::future` を返します。
- `response_timeout`: RPC を送信してから Response を受信するまでの時間。
- `sync_timeout`: 1 回の同期処理全体の上限。Event が続いて安定しない場合は
  `error_kind::unstable_state` になります。
- `ready_timeout`: operation queue に入ってから実行開始までの時間。接続・再同期を
  待つ上限でもあります。
- `connect_timeout`: DNS 解決、TCP 接続、WebSocket handshake の上限。
- `next_event(timeout)`: Event がなければ `nullopt` を返します。復旧中も待機できます。

## モデルと検証

- 公式 Schema から生成した `protocol.hpp` が wire の必須・未知・重複プロパティ、
  enum、整数範囲、nullable、`const` を検証します。Schema 本体は Git に保存せず、
  固定 revision から生成時に取得します。
- JSON は 1 つの Text Message として送受信します。Response に 64 KiB の制限はなく、
  クライアントの読み取りバッファ上限は 64 MiB です。
- Event は `std::variant<timer_event, reset_event, changed_event>` で表現します。
