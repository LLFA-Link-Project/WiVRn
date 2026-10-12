# Lighthouse Reference Dataset Recorder

ビルド、Dashboard/CLI の使用法、schema、時刻同期、再生、既知の制約:
[仕様と導入手順](../../docs/lighthouse-recorder.md)

- `lh-recorder`: Python 標準ライブラリだけの session supervisor / validator / aligner。
- `build-libsurvive.sh`: pin に対する受動 USB capture 修正のビルド。
- `tests/`: Pose、時刻変換、設定 snapshot、終了処理の自動テスト。
- `samples/synthetic-session/`: 明記した合成データ（solve 用の実測 sensor data ではありません）。
- [テスト結果](TEST_RESULTS.md)
