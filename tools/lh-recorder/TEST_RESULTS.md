# gzip 記録・tar.xz 保存 v2.1 の検証

基準: WiVRn `bd799f4feef31f79d5aaf5bd2e3971829c10fa02` + Monado追加パッチ0018。

- server / 起動 harness ビルド成功。zlibでPose・events・clock_sync・usb_devicesをgzipに逐次保存。
- 全自動テスト **26件成功**（Pose、設定snapshot、通常／異常終了、permission failure、overflow、replay、圧縮・展開）。
- 圧縮処理の最終変更後も専用テスト7件が成功。標準 `xz -t` / `tar -tf` で読込可能。
- 旧JSONLとgzip JSONLの両方を検証し、align派生インデックス／public exportもgzipで出力。
- 2セッション一括圧縮・展開後のJSONL原文／USB payloadが元データと一致。gzipセッションと旧非圧縮セッションの両方をgzip level 6へ復元し、再計算したSHA-256と全内容validationが成功。元セッションの全ファイルSHA-256は変更なし。
- 展開後の2セッションのUSB記録を実際のpatched libsurviveで再生し、各120 packet読込を確認。
- gzipを展開してpreset 9 + EXTREMEの標準tar.xzに再圧縮。tar内に生JSONL／pcapが入り、復元はgzip level 6に統一。繰り返しパターンの合成ログでarchiveが元gzipより小さくなることを確認。元セッションを変更せず、同名archive／展開先の上書きを拒否。
- 記録中、圧縮中のファイル変更、symlink、path traversal、重複entry、切れたxz trailer、archive内のhash不一致を拒否。一時出力を除去。旧tar.xzのgzip／非圧縮JSONLをgzip level 6に統一できることも確認。
- 圧縮後も100,000更新負荷試験でqueue capacity4096、欠落counter整合、callback平均100µs未満、Pose executableのピークRSS64MiB未満。
  Linux `ru_maxrss` は親Pythonのxz圧縮時のhigh-water markがexec後にも影響するため、harnessのRSS測定を `/proc/self/status` の `VmHWM` に変更。

実機でのトラッキング品質・長時間記録と、実測センサー入力からのPose solveは未検証です。
以下は過去版の検証記録です。

# Native 記録 v2 の検証

基準: WiVRn `dad7b6a29a52ec6624b8895d5d7892f73147059a`。記録用 CLI / 外部 supervisor を廃止し、server 内に libpcap / gzip / clock sampler / session finalization を組み込みました。
Monado の追加0017は Pose writer を明示的に停止・flush するAPIです。0016は初版のまま保持しています。

- Native server / Dashboard / OpenXR runtime のビルド成功。
- 自動テスト **19件成功**。Qt Settings の保存・復元テストも成功。
- PATH を空にし、Python や survive-cli を起動できない状態で同時記録・通常終了を確認。
- OFF では外部環境変数が残っていても出力ディレクトリを作らない。
- 記録中のOFF / 出力先変更 / 再度start呼出しは無視。新しいMonado起動で変更を反映。
- `~/LH-Dump` を HOME へ展開し、保存できることを確認。
- 正常終了は owner_exit_status=0、異常終了は1で確定。両方でPose writer停止、USB source破棄、gzipを閉じることを確認。
- 終了後に記録ファイルが変わらず、SHA-256と一致することを確認。
- 内蔵writerで生成したpcapを、実際のpatched libsurviveで2回読込。ファイルを変更せず、各回120 packetを読み込む。
- 新しい `synthetic-native-session` は全内容検証でvalid、120 Pose / 120 USB packets。実際のセンサー由来ではなく、Pose solve は未実施。
- usbmon開始をpermission-deniedで失敗させても、Poseは記録してflushし、manifestはabnormalとして失敗理由を保存する。
- 実際のusbmon backendも実行。この環境には `/dev/usbmon0` がないため失敗を検出。Pose 120件を保存し、USB 0件をabnormalとして記録した（Poseはテスト入力）。
- SteamVR LighthouseをOFFにした別ビルドでも、recordingの無効stubがコンパイルできることを確認。recordingのlibpcap依存はこの構成で追加しない。
- ステージングinstall成功。オフラインCLIに残るのは validate / align / export-public のみ。

実機のValve callbackとの同時収集、追跡品質、実測センサーからのlibsurvive Pose solve、長時間の実機記録は未検証です。
Poseキューは飽和／mutex競合で待機せずdropし、その件数を保存します。低レートのテストでも稀な競合によるdropがあり得るため、試行数・保存数・announcement数・drop数の整合性を検証しています。
終了時の自動validationはcounter/packet length/writer状態の検査で、`full_content_validation=false` を明記します。オフラインvalidateが全内容・hash・clock mappingを検査します。

以下は初版の検証記録です。recording helper / Python / survive-cli の依存は v2 で除去されています。

## 初版 v1 の検証記録

実装基準と patch hashes は `provenance.json`。WiVRn の変更は本実装コミットに含まれます。
環境: Linux `7.2.9-arch1-1`、GCC `16.2.1`、Qt `6.12.0`、libpcap `1.11.0`、libusb `1.0.30`。
WiVRn のビルド表示は `v26.9-62-g34afe754`。Monado pin に WiVRn patches 0001–0015 と追加0016を適用して検証しました。

| 検証 | 結果・範囲 |
| --- | --- |
| WiVRn の CMake 全ビルド | 成功。server、Dashboard、OpenXR runtime `libopenxr_wivrn.so` を含む |
| libsurvive の capture/replay 用ビルド | 成功。shared build、usbmon/vive/mpfit/statebased plugins、gzip FOPENCOOKIE 有効 |
| Python 自動テスト | **19 tests passed**。最終フル実行約19.2秒 |
| Qt Settings の単体テスト | 成功。既定値、保存 JSON、再読込、変更前 snapshot、Unicode/空白パス、URL decode、reset、型不正値を確認 |
| 起動・終了の統合テスト | 実際の `configuration` parser と WiVRn 起動 helper を使用。USB は明記した合成 fixture |
| Monado clock domain | 起動 harness 内で `os_monotonic_get_ns()` の前後 bracket に `steady_clock` が入ることを1000回確認。この Linux/libstdc++ に限定 |
| QML / 日本語翻訳 | Dashboard の QML cache compilation、`msgfmt --check` 成功 |
| install | `DESTDIR=/tmp/lh-install-check cmake --install` 成功。ホストへのインストールは行っていない |
| パッチ適用 | 0001–0015適用済みの新しい Monado checkout で0016の `git am` 成功。libsurvive patch の reverse-check 成功 |
| ログ無効 | Pose harness と WiVRn 起動 helper の両方でログ／出力ディレクトリを作成しないことを確認 |
| ログ有効 | 合成 USB と raw Pose を同じ session に生成。複数device、状態変化、終了時flushを確認 |
| 設定変更 | 記録中にconfigをOFF／新出力先へ変更しても120 Pose updates を旧sessionへ保存。再起動後にOFF／新出力先を反映 |
| 非有限値 / locale | NaN/Infinity を成分ごとのnullとして保存。decimal comma / digit grouping の process locale でも有効JSON |
| 時刻変換 | 定常offset、外挿拒否、forward/backward step、重複区間拒否、disk/SQLite版とmemory版の一致 |
| I/O error | Pose出力先をファイルとして開けない場合もharnessは正常終了、errorを記録 |
| USB開始失敗 | 実際の環境のusbmon不在を検出しabnormal manifestを確定。別途permission-denied fixtureでもWiVRn起動helper側が継続 |
| libsurvive再読込 | 提供する合成 `.pcap.gz` を2回読込、各回3 packet、WM0の機器対応、USBMon_Playbackのみ、exit code 0、原ファイル無改変 |
| sample validation | 30 Pose records、2device、3USB packets。clock read uncertainty最大461ns、alignment推定誤差最大1586ns（その合成sessionの値） |

libsurvive のEOFでは既存の `Driver reported -1` warningが出ますが、この合成reader検証はexit code 0で終了し、
packet countを別途確認しています。未知report `0xff` の合成packetなので、センサー解釈や姿勢solveの成功を示しません。
USBのkernel loss statisticsは合成sessionでは取得不可と記録しています。

## キュー負荷測定

単独harnessで100,000 Pose更新を休止なしで投入しました。

- queue容量: 4096 records。満杯／mutex競合でproducerは待機しない。
- attempted: 100,002（device announcements 2件を含む）。Pose書込4,360、drop95,640。
- producer loop: 6,970,075ns、平均約69.7ns/update。
- `getrusage(RUSAGE_SELF).ru_maxrss`: 6,320KiB。
- writerは受理したデータを終了時にflushし、dropを報告。

これは極端な合成入力に対するoverflow経路の測定です。大半がdropなので、通常の全更新保存時の性能や
VRのtracking latencyを表す値ではありません。数時間の実機記録・遅い実ディスクでの追跡品質の検証は未実施です。

## 未実施と制約

この環境には `/dev/usbmon*` がありません。次の成功は報告していません。

- 実機での通常のLighthouse trackingとUSB/Pose同時収集。
- ロギングON/OFFでの追跡品質／callback latencyの実機比較。
- 実際のDashboard画面操作、D-Busでの保存、GUI再起動の一連の実行。既存保存経路に沿う実装・設定codec・QML buildを検証。
- 静止、移動、高速回転、遮蔽、復帰、実機power OFF/ON。
- 実測USB config / sensor payloadからのlibsurvive Pose solve。
- 実際のUSB ACL denial、disk full、電源断、Flatpak packaging。

既知の制約:

- libsurviveの既知VID/PIDと起動時USBアドレスを対象とする。再接続の新アドレス・未対応機器は取りこぼす可能性があり、sysfs変化とunobserved reportを保存する。
- USB receiver serialとValve tracking object serialの対応は自動推測しない。captured configurationがなければ比較時の対応は不明。
- 個別のPosition/Orientation/Velocity validity、Pose生成時刻、ドライバー内部prediction horizonは取得不能。
- OpenVR headerのlinear acceleration単位記載が曖昧なため、原値と単位の不確実性を保持。
- 時刻補間の誤差は推定値。測定間に発生して戻ったclock jump、kernel/driver内部latencyの上限は保証しない。
- Application-Facing Pose streamと姿勢比較ツールは未実装。後続設計は仕様書に記載。
- SIGKILL／電源断では未確定manifestやgzip末尾欠損が残る。既存WiVRnの終了timeout（3.5秒）より遅いdrainも強制終了で欠損する可能性がある。validationで成功として扱わない。
- 完全なUSB replayを保持した公開用匿名化は保証しない。public exportは識別情報を含み得るUSB payloadを除外する。
- 容量管理／ローテーションは追加指示に従い実装していない。

実機での検証手順は [仕様書](../../docs/lighthouse-recorder.md) を参照してください。
