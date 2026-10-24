# 手持ちジッターの A/B 引き継ぎ (2026-10-24)

`~/libsurvive/docs/libsurvive-handheld-jitter-handoff-20261024.md` を反映。
依存コピーを `llfa-main` の `9f3919cf3f1dd494833e6d6b6150493678721690` へ更新した。
`612629c` の native writer 排他修正と `f8886a2` の加速度逆算修正を含む。
`~/libsurvive` の作業ツリーは変更していない。
Monado の座標変換・履歴・予測・clamp の計算も変更していない。
更新先は `build-dashboard/_deps/libsurvive-handheld/libsurvive-llfa-main-install`。

排他修正は記録の破損を直すもので、手持ちジッターが直ったという意味ではない。
B の再生結果は細かな変動が小さいが、実際の微動の減衰や live の遅れは未測定。
既定値を変更する根拠にはしていない。

## 比較の選択

記録を有効にした libsurvive 起動時のみ、環境変数
`WIVRN_SURVIVE_ANGULAR_NOISE_COMPARISON=A` または `B` を解釈する。

| 設定 | A | B |
|---|---:|---:|
| process-weight-ang-vel | 60 | 0.6 |
| kalman-minimize-state-space | 0 | 0 |
| process-weight-gyro-bias | 1e-8 | 1e-8 |
| process-weight-acc-bias | 1e-8 | 1e-8 |

選択を外すと試験引数は渡さない。記録無効時も渡さない。
旧 `WIVRN_SURVIVE_BIAS_COMPARISON` との併用、不正な選択値、
上記引数を上書きする競合した `SURVIVE_*` 環境変数は初期化エラーとする。
選択値・実引数は `survive-settings.json` と manifest の `survive_settings`、
native OPTION の実効値は `native-options.json` と `trial_effective_options` に保存する。
実効値が条件と異なる場合、または設定 clamp が 0 ms でなければ記録を abnormal にする。
実際の各出力の `effective_prediction_ns` も引き続き記録・validate で検査する。
ほかの環境変数・ダッシュボード設定・ハードウェア条件は A/B 間で同一に保つ。

## 固定した自己較正と起動方法

今回は **20:37 記録の final snapshot** を使う。19:54 記録の初期値や live config は使わない。

- 原本: `/home/reina/LH-Dump/2026-10-24_203705_libsurvive_907114938_814822/calibration.final.json`
- 固定コピー: `build-dashboard/diagnostics/handheld-angular-comparison-20261024/baseline.json`
- SHA-256: `c0fd8d7485628f0471b3001e13786cf092bfb06ce66bd384d0740188087dd9da`
- 来歴: 同ディレクトリの `baseline-provenance.json`。固定コピーは読み取り専用。

ユーザーが正常追跡を確認した libsurvive 自身の較正であり、SteamVR の基地局姿勢は使わない。
各起動前に dashboard/server を終了し、WiVRn ディレクトリで以下を実行する。
ダッシュボードで libsurvive・記録有効・予測 clamp 0 ms を選ぶ。
毎回新しい書き込み可能なコピーを使う。下記の A を B に変えて比較する。

```sh
baseline="$PWD/build-dashboard/diagnostics/handheld-angular-comparison-20261024/baseline.json"
trial_dir="$(mktemp -d /tmp/wivrn-survive-angular.XXXXXX)"
install -m 600 "$baseline" "$trial_dir/config.json"
env -u WIVRN_SURVIVE_BIAS_COMPARISON -u SURVIVE_INIT_CONFIGFILE \
    WIVRN_SURVIVE_ANGULAR_NOISE_COMPARISON=A \
    SURVIVE_CONFIGFILE="$trial_dir/config.json" \
    ./build-dashboard/server/wivrn-dashboard
```

記録された `calibration.initial.json` の SHA が baseline と一致すること、
両試行の source/build・ロードされた core/plugin のパスと SHA が同じことを確認する。
新規較正からの確認は、良い条件が見つかった後に別試行として行う。

## 実測区間

A→B だけでなく B→A でも繰り返す。同じ BS、記録量、置き方で比較する。
各開始・終了を別ターミナルから記録する。session は今記録しているディレクトリを指定。

```sh
tools/lh-recorder/lh-recorder mark "$session" tracker-desk-start
# 10 秒以上静置後
tools/lh-recorder/lh-recorder mark "$session" tracker-desk-end
```

1. Tracker を台に固定して 10 秒以上静止。
2. 手持ちの自然な小さな動きで 10 秒以上観察。
3. 数 cm の遅い並進、数度の回転、動き始め・停止・逆転を各 5 秒ほど。
4. Index 高速回転 → 台に置く → 静置。その後 Tracker を持ち上げて 2、3 を再実施。

`tracker-held-start/end`、`slow-translation-start/end`、`slow-rotation-start/end`、
`index-fast-start/end`、`index-down`、`tracker-pickup` などのラベルを使う。
体感評価も自由なラベルで残せる。ジッター、始動遅れ、微動の消失、停止後の滑り、
速い運動からの復帰を個別に記載する。マーカーは CLOCK_MONOTONIC で記録される。
native receive-clock や動画 wall-clock との単純な数値比較はしない。

終了後に `tools/lh-recorder/lh-recorder validate "$session"` を実行する。
実機での A/B 実測はまだ行っていない。

## 記録の検査と残る調査

seal 時と offline validate に、NUL、ヘッダー、CONFIG JSON、既知の IMU/光学/pose 系
数値レコードの列数・数値構文の検査を追加した。未知の診断本文は検査しない部分文法であり、
全 playback 文法の保証ではない。gzip は EOF/CRC まで読む。
NUL があっても文字列を途中で切らずに検査し、件数と先頭 8 件の行番号・理由を保存する。
native 出力ロスを計測できたという意味でもない。

20:37 原記録には約 37.113 秒で 2 行の NUL 混入がある。CRC は通るが記録整合性検査は失敗する。
原記録は変更していない。監査結果は固定コピーと同じ診断ディレクトリの
`source-native-integrity.json` に保存した。
右 Index 高速回転 84.37–87.37 秒、Tracker 持ち上げ約 87.77 秒という説明は引き継いだが、
その後の手持ち区間を IMU の静かさだけで自動分類しない。

Tracker の工場補正後の加速度ノルム約 0.9404 g は、学習済み residual bias と分けて扱う。
単一姿勢からの再正規化は加えていない。
simple API の lighthouse getter と共有 BSD 状態の排他は別の未解決調査であり、
ジッター原因とは断定しない。poll mutex 内に ctx lock を追加してはいけない。

## 検証

- 更新した libsurvive: 関連 16 テスト成功。
- Monado adapter / native lifecycle / offline validator: 18 テスト成功。
  実 native API で A/B の OPTION 実効値、記録無効時の試験設定無効化、
  固定 config コピー、NUL と列数破損の検出を確認した。
- SteamVR 記録の回帰検査: 20 成功、6 skip（外部実測等が必要なケース）。
- 20:37 原記録: 1,976,002 行を読み、NUL 混入 2 行を検出。原本は変更なし。
- Monado patch 0031 は 0030 適用済みの親ソースに clean apply を確認。
- 実機の手持ちジッター・始動遅れ・微動保持の比較は未実施。
