# Lighthouse Reference Dataset Recorder

ビルド、Dashboard/CLI の使用法、schema、時刻同期、再生、既知の制約:
[仕様と導入手順](../../docs/lighthouse-recorder.md)

- 記録は server 内蔵。Dashboard の ON/OFF・出力先だけで起動し、Monado 終了時に停止・確定。
- `lh-recorder`: 任意のオフライン validator / aligner / public exporter（記録コマンドなし）。
- `build-libsurvive.sh`: pin に対する任意のオフライン replay 用互換修正のビルド。
- `tests/`: Pose、時刻変換、設定 snapshot、終了処理の自動テスト。
- `samples/synthetic-native-session/`: 内蔵 writer が生成した合成データ（sensor data / Pose solve の検証ではありません）。
- `samples/synthetic-session/`: 初版の互換 fixture。
- [テスト結果](TEST_RESULTS.md)
