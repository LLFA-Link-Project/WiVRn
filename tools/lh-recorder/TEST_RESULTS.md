# 実施した検証

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
