# 通信モデル生成

## 正本

通信仕様の正本は LiveSplit.Bridge の公式 JSON Schema v4 です。次の確定コミットを
`tools/protocol-source.json` と `schemas/v4/SOURCE.json` に固定しています。

- Repository: <https://github.com/Nanahuse/LiveSplit.Bridge>
- Revision: `4c14c0161eacd50b05411c0d20f5eff523e9c155`
- Schema:
  - `schemas/v4/rpc-request.schema.json`
  - `schemas/v4/rpc-response.schema.json`
  - `schemas/v4/bridge-event.schema.json`

`schemas/v4/` のファイルは公式ファイルの無改変コピーです。`SOURCE.json` の SHA-256 と
一致しない場合は生成を中止します。独自 Schema や手動の通信 DTO はありません。

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
```

`--check` はコミット済みヘッダーと生成結果を比較し、差分があれば失敗します。
出力にタイムスタンプは含めず、同じ入力から常に同じ結果を生成します。

## Schema 更新手順

1. `tools/protocol-source.json` と `schemas/v4/SOURCE.json` の `revision` を対象の
   確定コミット SHA に更新します。
2. 固定コミットの 3 つの Schema を無改変で `schemas/v4/` に取得し、`SOURCE.json` の
   SHA-256 を更新します。
3. `python tools/generate_models.py` を実行します。
4. 公開型名が変わった場合はクライアント API とテストを調整します。
5. `python tools/generate_models.py --check`、通常テスト、公式 TestHost 結合テストを
   実行し、生成結果をコミットします。

## Fixture

`tests/fixtures/csharp-serialized-cases.json` は公式 LiveSplit.Bridge の C# DTO から
出力されたシリアライズサンプルです。固定コミットの仕様に対する wire 互換性を確認する
ために使用します。fixture 自体は通信仕様の正本ではなく、公式 Schema と生成コードを
置き換えるものではありません。
