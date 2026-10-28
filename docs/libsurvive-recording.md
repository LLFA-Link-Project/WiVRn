# libsurvive の記録と微動比較

較正の失敗を調べる場合は [固定初期値と新規較正の比較手順](libsurvive-calibration-diagnostics-20261024.md) を先に参照してください。

ダッシュボードで libsurvive を選択し、「Lighthouse Recording」の記録を有効にします。
ヘッドセットを再接続して内部Monadoを起動すると、指定した出力先の
`日時_libsurvive_…` に保存します。通常の終了でnative記録を閉じ、非同期writerを
stop/join/drainし、manifestの完全性・SHA-256を確定します。
SteamVRの記録は従来のusbmon方式で、同時に別プロセスを起動してUSBを取り合いません。
libsurviveの記録にusbmon権限は必要ありません。

## 記録内容

- `manifest.json`: `source` / `pose_backend` は `libsurvive`。終了状態、キュー統計、
  診断タグの有無、ファイルハッシュ、実際にロードされたライブラリとpluginのパス・SHA-256。
- `build-provenance.json`: ビルド時のWiVRn/Monado/libsurviveのrevision・追跡差分、
  パッチSHA-256。システム提供のlibsurvive等でソースrevisionを取得できなければnull。
  オフライン検証は不明revisionを明示的に失敗扱いにします。
- `native.rec.gz`: 実際に利用中のコンテキストから記録したraw光学・raw/calibrated IMU・
  pose/velocity。quaternionはnativeでは **wxyz**。現行 `~/libsurvive` はPOSE_TIMING・
  POSE_STATE・POSE_BIAS類を出力しません。これらは過去のlibsurvive拡張で記録した項目です。
- `poses.jsonl.gz`: Monado側の非同期キューで記録。全行が `source: libsurvive`、
  `stream: libsurvive_pose`。quaternionは **xyzw**。
  `stage` は `backend_received`（単純APIが渡した予測前の値）、`history_input`
  （world変換後、IMU原点で履歴へ渡した値と投入結果）、`app_output`
  （要求されたinteraction profileへ予測・オフセット合成後）、`hand_output`（手首）。
- `pose_status.json`: capacity、attempted/enqueued/written/dropped/unwritten、writerの終了・エラー。
  満杯ではcallbackをディスクI/O待ちにせずdropを記録。dropや書込失敗は不完全な記録として扱います。
- `survive-settings.json`: 実際の初期化引数、使用する設定ファイル、比較モード。
  `calibration.initial.json`（既存設定があれば）、`calibration.final.json`（終了後の設定）、
  `native-devices.json`（native CONFIGのcodename→工場設定・serial・role）。
- `clock_sync.jsonl.gz`: realtime/monotonic/rawの時刻対応標本。
- `scene-markers.jsonl`: 手動で指定した動作区間。以下の `mark` で追記します。

各poseには実行中のserial・role、callbackのmonotonic時刻、座標系・参照点、位置m、
線速度m/s、角速度rad/s、validity、要求時刻・解決時刻・実効追加予測時間を保存します。
IMU→head、head→grip、grip→palm、要求inputのoffset、手首のpalmに対する変換と
左右のrollも保存します。現行libsurviveではnative floor offset・センサーtick・bias状態を取得できません。
Monadoのworld回転はnativeの (x,y,z) → (x,z,-y) です。

単純APIのイベントにはセンサーtick/Hzと独立したvelocity時刻がないため、
これらは **null** です。callback時刻をセンサー時刻に見せません。
姿勢のmonotonic時刻は単純APIのepoch時刻からアダプターが変換した推定です。
アプリ出力の実効予測時間は履歴の同じlock内で取得し、clamp=0では0になります。
補間では追加予測0となり、過去方向への予測は負です。
現行libsurviveの記録はセンサーtick・入力受信時刻を含まないため、
入力→描画の遅延はnative記録から直接計算できません。

通常の記録をONにしてもfilterやZVU、静止判定閾値は変更しません。
native recorderが設定ファイルを記録名から自動選択する副作用を避けるため、
`SURVIVE_CONFIGFILE`（未指定なら `config.json`）を元の表記のまま明示します。
libsurviveは裸のファイル名を `$XDG_CONFIG_HOME/libsurvive`、未設定なら
`$HOME/.config/libsurvive` から読みます。`./名前` や絶対パスはそのまま使用します。
記録時も同じ解釈を維持し、較正ファイルを作業ディレクトリの別ファイルへ切り替えません。
追加の native recorder は同期式で、独立したdrop/書込エラーカウンターや
closeの戻り値を公開していません。これらの指標はnull/取得不能と明記し、終了後に
gzipのCRCと読み取り、タグの存在、ハッシュを検査します。完全な入力無損失は証明しません。
記録の負荷そのものは未測定で、通常利用と計測中の揺れを同一視しないでください。

## 微動のA→B→A比較

`~/libsurvive/docs/libsurvive-micro-motion-handoff.md` の実験用です。
ダッシュボードとサーバーを終了して、次のどちらかで起動します。
記録を有効にし、libsurviveを選択してから内部Monadoを起動してください。

```sh
env WIVRN_SURVIVE_BIAS_COMPARISON=A ./build-dashboard/server/wivrn-dashboard
env WIVRN_SURVIVE_BIAS_COMPARISON=B ./build-dashboard/server/wivrn-dashboard
```

この明示的な比較モードだけが `kalman-minimize-state-space=0` を指定し、
Aはgyro/acc bias process weight=0、Bは両方1e-8にします。
各セッション専用の新しい `calibration.json` を使いlibsurvive自身で較正します。
Valveの外部較正はインポートしません。通常の記録ではこの切り替えは行いません。
比較モードA/Bを症状が解消した設定とは扱いません。

まず共通のprediction clampを0 msにし、同じBS・置き方でA→B→Aを記録します。
起動後30〜60秒のウォームアップ、机上完全静止20秒、肘を支えた手持ち微動20〜30秒、
向きを保った数mm〜1cmのゆっくりした移動20〜30秒、±1〜3度のゆっくりした回転20〜30秒、
通常の動作・速い往復20秒、最後の静止20秒を含めます。

各区間の開始・終了に別ターミナルから実行します（SESSIONは起動ログの保存先）。

```sh
tools/lh-recorder/lh-recorder mark /absolute/path/SESSION warmup-end
tools/lh-recorder/lh-recorder mark /absolute/path/SESSION handheld-micro-start
tools/lh-recorder/lh-recorder mark /absolute/path/SESSION handheld-micro-end
```

手動の移動距離・角度は目標値であり真値の実測ではありません。
正常終了後、次で検証し、そのセッションをlibsurvive側へ渡せます。

```sh
tools/lh-recorder/lh-recorder validate /absolute/path/SESSION
```

静止RMSだけでなく、微動の滑る方向、raw gyroと残留bias、静止判定の継続時間、
光学/IMU更新の鮮度、動き出し・反転の追従と欠測復帰を評価します。
これらの実機比較は実装時点では未実施です。
圧縮・展開は既存の `compress` / `decompress` が利用できます。
`align` はusbmon向けでnative sensor時刻の変換には使えません。
`export-public` はnativeのserial/較正を安全に除去する実装がないため拒否します。
