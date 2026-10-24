# Lighthouse Reference Dataset Recorder

ビルド、Dashboard/CLI の使用法、schema、時刻同期、再生、既知の制約:
[仕様と導入手順](../../docs/lighthouse-recorder.md)

- 記録は server 内蔵。Dashboard の ON/OFF・出力先だけで起動し、Monado 終了時に停止・確定。
- `lh-recorder`: 任意のオフライン validator / aligner / public exporter / 一括圧縮・展開ツール（記録コマンドなし）。
- `build-libsurvive.sh`: pin に対する任意のオフライン replay 用互換修正のビルド。
- `tests/`: Pose、時刻変換、設定 snapshot、終了処理の自動テスト。
- `samples/synthetic-native-session/`: 内蔵 writer が生成した合成データ（sensor data / Pose solve の検証ではありません）。
- `samples/synthetic-session/`: 初版の互換 fixture。
- [テスト結果](TEST_RESULTS.md)

記録中の増大する JSONL は gzip に保存します。保存済みセッションの圧縮・展開:

```sh
tools/lh-recorder/lh-recorder compress ~/LH-Dump --output ~/LH-Archives
tools/lh-recorder/lh-recorder decompress ~/LH-Archives --output ~/LH-Restored
```

標準 `.tar.xz` (LZMA2 preset 9e)。元データを保持し、同名の出力は上書きしません。

gzipの内容を展開してからtar.xzに最高圧縮で格納します。
展開ツールはJSONL／pcapをgzip level 6に揃え、manifestのSHA-256を再計算します。
元セッションは変更せず、JSONL原文とUSB payloadを保持します。

libsurvive も同じ Dashboard 設定で記録できます。セッション名の `_libsurvive_`、
manifest の `source: libsurvive`、各 pose 行の `source` / `stream` / `stage` で識別します。
usbmon の権限は不要です。詳しくは [libsurvive の記録・比較手順](../../docs/libsurvive-recording.md)。
