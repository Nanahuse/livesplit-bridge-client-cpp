# 通信モデル生成

## 正本

通信仕様の正本は LiveSplit.Bridge の公式 JSON Schema v4 です。次の公式リポジトリ、
確定コミット、Schema を `tools/protocol-source.json` に固定しています。

- Repository: <https://github.com/Nanahuse/LiveSplit.Bridge>
- Revision: `4c14c0161eacd50b05411c0d20f5eff523e9c155`
- Schema path:
  - `schemas/v4/rpc-request.schema.json`
  - `schemas/v4/rpc-response.schema.json`
  - `schemas/v4/bridge-event.schema.json`

Schema 本体はこのリポジトリに保存しません。`tools/protocol-source.json` に公式
リポジトリ、確定 revision、Schema パス、SHA-256 を固定し、生成時に固定 revision から
メモリへ取得します。SHA-256 と一致しない場合は生成を中止します。独自 Schema や
手動の通信 DTO はありません。

生成結果は `include/livesplit/bridge/protocol.hpp` にコミットしています。通常の
ビルド時に Python や生成ツールは不要です。

## quicktype の適合性確認

quicktype は第一候補として実際に公式 Schema で検証しましたが、次の理由で採用しませんでした。

1. ルートの `oneOf` 内の `$ref` を解決できません。

   ```text
   $ npx quicktype --lang c++ --src-lang schema rpc-response.schema.json
   Error: Could not fetch schema , referred to from rpc-response.schema.json#oneOf/0.
   ```

   `$defs` を `definitions` に変換し `$ref` をインライン展開しても、quicktype の
   C++ 出力は `oneOf` を判別 union にせず、全分岐のプロパティを 1 クラスの
   optional フィールドへ融合します。const や required の制約も失われます。

2. C++ 出力が `boost::optional` / `std::shared_ptr` を使い、`std::optional` と
   「必須だが null 許容」を区別できません。
3. 整数の `minimum` / `maximum` を型や変換コードに反映しません。
4. `additionalProperties: false`、未知プロパティ、重複プロパティ、`const` の検証を
   生成しません。
5. `oneOf` の分岐選択を検証せず、Request の method / result 型の対応も表現できません。

このため、Python 標準ライブラリだけで動く決定論的な生成器
`tools/generate_models.py` を使用します。生成器は公式 Schema の `$defs` / `$ref`、
各ルートの `oneOf`、nullable、`const`、enum、整数範囲、
`additionalProperties: false` を入力として扱います。

## 生成規則

- 3 つの Schema の `$defs` を統合し、同名定義が完全一致しない場合は失敗します。
- 各ルートの `oneOf` を `std::variant` にし、生成した厳格な `from_json` で
  「ちょうど 1 分岐だけが一致する」ことを検証します。
- JSON の必須プロパティ、未知プロパティ、重複プロパティを拒否します。
- `integer` は Schema の型と範囲から `std::uint32_t` / `std::int32_t` /
  `std::int64_t` を選択し、変換時に範囲を検証します。時間値は 100 ナノ秒単位の
  signed int64 で、負値も受理します。
- `const null` は `std::nullptr_t`、`const "..."` は既定値付き `std::string` と
  して生成し、値も検証します。
- 配列・辞書は `std::vector` / `std::map` にし、null 要素は `std::optional` にします。
- `RunIcons.segment_icons` の公式 `if` / `then` は「配列自体は null 不可、要素は
  null 可」を表します。生成器はこの既知の条件だけを検証し、配列の外側の null を
  型から除きます。条件の形が変わった場合は生成を失敗させます。
- `CompletedCount` は C++ の「クラス名と同名のメンバーはコンストラクタと解釈される」
  規則を避けるため、生成型名を `completed_count_value` にしています。wire 上の
  プロパティ名 `completed_count` は変更しません。
- 生成する構造体は `operator==` を default 定義し、テストと利用側での比較を可能に
  します。

## 実行

```powershell
python tools/generate_models.py
python tools/generate_models.py --check
# 固定コミットのローカル checkout を利用する場合（ネットワーク不要）
python tools/generate_models.py --source C:/path/to/LiveSplit.Bridge
```

`--check` はコミット済みヘッダーと生成結果を比較し、差分があれば失敗します。
デフォルトでは固定 revision から Schema を取得します。`--source` を指定した場合は
その checkout の `schemas/v4/` を読み、同じ SHA-256 検証を行います。Schema ファイルは
リポジトリにも一時ファイルにも保存しません。出力にタイムスタンプは含めず、同じ入力
から常に同じ結果を生成します。

## Schema 更新手順

1. `tools/protocol-source.json` の `revision`、各 Schema の `path` と `sha256` を
   対象の確定コミットに更新します。
2. `python tools/generate_models.py` を実行します。固定コミットから Schema を取得し、
   SHA-256 を検証して生成します。ローカル checkout を使う場合は `--source` を指定します。
3. 公開型名が変わった場合はクライアント API とテストを調整します。
4. `python tools/generate_models.py --check`、通常テスト、公式 TestHost 結合テストを
   実行し、生成結果をコミットします。

## Fixture

`tests/fixtures/csharp-serialized-cases.json` は公式 LiveSplit.Bridge の C# DTO から
出力されたシリアライズサンプルです。固定コミットの仕様に対する wire 互換性を確認する
ために使用します。fixture 自体は通信仕様の正本ではなく、公式 Schema と生成コードを
置き換えるものではありません。
