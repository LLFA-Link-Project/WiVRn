# Lighthouse Reference Dataset Recorder v1

Valve Pose は Ground Truth ではなく **Reference Pose** です。本実装はデータ収集を担当します。
Dashboard の設定項目は「Enable recording」と「Output directory」の2つだけです。
設定は既存の WiVRn JSON / D-Bus 保存経路を通り、Monado 子プロセスを起動する際の
`configuration` のコピーから一度だけ読みます。稼働中の編集は現在のセッションに反映されません。
Pose のロガーはプロセス終了時まで動作し、専用 supervisor は Linux pidfd で終了を検出します。
ランタイム設定 IPC、設定ファイル監視、Dashboard の状態ポーリングはありません。

## ソース調査と revision

| コンポーネント | 調査・実装の基準 |
| --- | --- |
| WiVRn | `34afe754f8750e8a98e2e6db8e0c25b44502e72a`、本実装コミットの親 revision |
| Monado | `monado-rev`: `09741cbcb45236f4f4f79790ea133cd90d68d5eb` + WiVRn patches 0001–0015 |
| ローカルの参考 Monado | `ad7610a5561325a9b368cda5d45fe195d67f4c3b`。実装パッチは上の WiVRn pin に対して作成 |
| libsurvive | `23f03665070549f0c13a6e768dfaf7764f75c417`、CMake project version `0.3`、CLI 表示 `v1.01-206-g23f0366-dirty` |
| ロガー | `1.0.0`、schema version `1` |

追加パッチ:

- `patches/monado/0016-d-steamvr_lh-record-valve-reference-poses.patch`
- `patches/libsurvive/0001-usbmon-safe-passive-recording.patch`

WiVRn の `CMakeLists.txt` は `patches/monado/*` を既に FetchContent 時に適用します。
Monado の `src/xrt/drivers/CMakeLists.txt` に独立した recorder コンポーネントを追加しました。
既存の checkout や `FETCHCONTENT_SOURCE_DIR_MONADO` を使う場合は、0016 の適用を確認してください。
実行時 manifest は期待する pin と実際の WiVRn/Monado ビルド revision を区別します。
libsurvive の実際の full revision は `--libsurvive-source` を指定した場合に記録し、未指定なら
`null` です。バイナリ SHA-256 と CLI バージョン出力も保存します。期待 pin を実測 revision に見せません。

## Pose の伝搬経路

1. Valve が `Context::TrackedDevicePoseUpdated(index, DriverPose_t, size)` を呼ぶ。
2. callback 入口で `CLOCK_MONOTONIC` を測定する。既存のデバイス検索後、`Device::update_pose` の前で
   `DriverPose_t` を固定容量キューへコピーする。ポーリングは行わない。
3. `Device::update_pose` は driver-from-head / world-from-driver / chaperone を合成し、角速度を回転し、
   接線速度を加え、pose offset を nanoseconds に変換して relation history に保存する。
4. `Device::get_pose` は history の補間・予測と既存の raw-reference-first 補正を行う。
   `ControllerDevice::get_tracked_pose` はさらに grip/aim 等の profile offset を合成する。
5. WiVRn の `server/driver/wivrn_session.cpp` は `steamvr_lh_create_devices` でこれらの `xrt_device` を
   OpenXR 用 system devices / roles に組み込む。アプリ側の要求時刻と空間変換はこの後段にある。

`poses.jsonl` は手順2の A ストリームだけです。Application-Facing Pose（B）は今回実装していません。
Monado の Linux `os_monotonic_get_ns()` は `CLOCK_MONOTONIC`、この pin の `chrono_timestamp_ns()` は
`std::chrono::steady_clock` です。この Linux/libstdc++ ビルドでは後者も monotonic ですが、他の標準ライブラリへの
移植時は対応を再検証してください。ロガーの clock ID は標準ライブラリの推測に依存させていません。

## ビルドと導入

WiVRn 本体の通常のビルド依存関係に加えて、Python 3.9 以上、libpcap、libusb、zlib の development packages が必要です。
Linux pidfd 対応 kernel が必要です。USB 権限を取得するために WiVRn 全体を root で実行する必要はありません。

```sh
# 初回のみ。スクリプトは既存 checkout を変更しません。
tools/lh-recorder/build-libsurvive.sh /tmp/lh-survive-src /tmp/lh-survive-build

# WiVRn を起動する環境に渡す。shared plugin directories は build 内に残してください。
export LH_RECORDER_SURVIVE_CLI=/tmp/lh-survive-build/survive-cli

# 開発ビルド。production install では recorder path override を省略。
cmake -S . -B build-lh -G Ninja \
  -DWIVRN_BUILD_CLIENT=OFF -DWIVRN_BUILD_SERVER=ON -DWIVRN_BUILD_DASHBOARD=ON \
  -DWIVRN_BUILD_TEST=ON \
  -DWIVRN_LH_RECORDER_EXECUTABLE="$PWD/tools/lh-recorder/lh-recorder"
cmake --build build-lh # server / Dashboard / OpenXR runtime library を含む
```

インストールでは CLI を `${CMAKE_INSTALL_BINDIR}/lh-recorder` と
`${CMAKE_INSTALL_LIBEXECDIR}/wivrn/lh-recorder` に配置します。ビルド用 override を設定したままの
バイナリはそのパスを参照するので、配布ビルドではデフォルトに戻してください。
libsurvive は別途導入し、`survive-cli` を PATH に置くか上の環境変数を使います。
systemd サービス起動ではサービスにもその環境変数を渡す必要があります。
Flatpak は usbmon デバイス、Python、libsurvive plugin と出力先へのアクセスが必要です。
この変更では Flatpak に依存ライブラリや権限を追加していないため、native install を検証対象にしてください。

管理者による USB 権限設定の例（本実装は自動で権限変更しません）:

```sh
sudo modprobe usbmon
sudo setfacl -m u:"$USER":r /dev/usbmon0
```

usbmon0 は全 bus を監視します。libsurvive は既知の Lighthouse VID/PID と起動時のアドレスだけを保存します。
kernel の observer ring は他の USB traffic にも影響されるので drop counts は Lighthouse 限定の損失数ではありません。
ACL は reboot/module reload で失われる可能性があります。個々の OS に合わせて永続的な設定を行ってください。
特権 capture プロセスを別に用意する場合も、一般ユーザーの session ディレクトリへ出力できる構成にしてください。

## Dashboard での利用

SteamVR tracked devices の利用設定を有効にし、Settings の Lighthouse Recording で記録を ON にして保存します。
出力先は既定で `$XDG_DATA_HOME/wivrn/lighthouse-recordings`、未設定時は
`$HOME/.local/share/wivrn/lighthouse-recordings` です。文字列入力と FolderDialog はローカルパスを扱います。
絶対パスを指定してください。日本語や空白を含むパスも保存できます。

```json
{
  "use-steamvr-lh": true,
  "lighthouse-recording": {
    "enabled": true,
    "output-directory": "/home/user/VRTrackingLogs"
  }
}
```

次の Monado 子プロセス起動時に日付・nanoseconds・PID を含む新しいディレクトリを作り、USB を先に開始してから
Valve driver の初期化へ進みます。設定が OFF の場合、recorder の spawn や出力ディレクトリの作成はありません。
開始失敗は stderr とセッションに記録され、通常のトラッキングの起動を続けます。
記録中に OFF / 出力先変更を保存しても、既存の recorder の環境・設定は変わりません。
Monado の終了時に Pose writer を join/flush し、supervisor が USB を SIGINT で停止して manifest を確定します。
自動削除、容量上限、ローテーションはありません。ディスク不足は異常終了として扱います。

## CLI

```sh
lh-recorder start --output /absolute/path/new-session \
  --survive-cli /path/to/patched/survive-cli --libsurvive-source /path/to/source
# 出力された LH_RECORDER_ENABLE=1 / LH_RECORDER_OUTPUT を WiVRn の起動環境に設定する。
# Dashboard による自動起動の場合、この手動操作は不要。
lh-recorder status /absolute/path/new-session
# 先に Monado を通常終了する。Pose logger はプロセス寿命に従い、runtime control IPC は持たない。
lh-recorder stop /absolute/path/new-session
lh-recorder validate /absolute/path/new-session
lh-recorder align /absolute/path/new-session
lh-recorder export-public /absolute/path/new-session /absolute/path/new-public-export
```

`run --owner-pid PID` は WiVRn の起動処理が使う foreground supervisor です。
`start` は supervisor を背景起動して初期化の成否を待ちます。`stop` は Pose writer が終了していることを確認して
supervisor を終了します。稼働中の Monado の Pose ロガーを動的に停止する操作ではありません。
CLI と Dashboard は USB/Pose の個別スイッチを提供しません。`LH_RECORDER_ENABLE=1` の明示指定は CLI 管理セッションを
選ぶため、Dashboard の OFF より優先します。通常の Dashboard 利用ではこの変数を設定しないでください。

## Session format

全 JSON は schema version 1。JSONL の各行は単独の JSON object です。

| ファイル | 内容 |
| --- | --- |
| `manifest.json` | 状態、開始/終了 realtime、設定 snapshot、revision、kernel、clock domains、損失と SHA-256 |
| `poses.jsonl` | 原精度の DriverPose の double 値、xyzw quaternion、ドライバー flag、変換 metadata |
| `events.jsonl` | device announcement、ドライバーの connected/valid/tracking-result 変化 |
| `usb.pcap.gz` | 原パケット bytes。DLT_USB_LINUX_MMAPPED=220、snaplen=65535 |
| `usb.pcap.gz.usbdevs` | libsurvive 原形式: `decimal VID PID bus address codename` |
| `usb_stats.json` | pcap statistics の可用性、received/drop/ifdrop、write error |
| `clock_sync.jsonl` | start/periodic/end/finalize の realtime と monotonic 前後サンプル、RAW 対応 |
| `devices.json` | sysfs の起動時の VID/PID、bus/address、serial と stable ID（不明なら null） |
| `usb_devices.jsonl` | 記録中の USB enumeration 変化。bus/address を永続 ID としない |
| `pose_status.json` | 試行数、書込数、drop、queue capacity、停止状態、writer error |
| `validation.json` | 構造・hash・timestamp・不足データの検証結果 |
| `usb_timestamps.jsonl` | `align` が生成する派生インデックス。原 timestamp と変換値・誤差を併記 |
| `libsurvive-config/` | capture プロセスの private config。利用者の既存 libsurvive config を変更しない |

manifest 自身、validation、lock/ready/manager.log は hash 集合から除外します。その他の nested config を含めて SHA-256 を計算します。
`align` で追加した派生ファイルは manifest の原データ hash 集合に追加しません。
異常終了時にバッファ末尾が壊れた JSONL / gzip、未確定 manifest は validation で成功としません。
SIGKILL / ホスト電源断では supervisor 自身が確定処理を実行できない場合があります。
既存 WiVRn の終了処理には3.5秒の timeout があり、極端に遅い writer drain は強制終了で欠損する可能性があります。

### Pose の意味

`position_m`, `linear_velocity_m_s` は driver world の追跡参照点の値です。
`orientation_xyzw` と保存した world-from-driver / driver-from-head の quaternion は必ず xyzw。
角速度・角加速度はそのまま保存します。Monado の処理は角速度を tracking reference local から姿勢 quaternion で回転します。
Quaternion を正規化したり無効 pose をゼロへ置換したりしません。非有限の成分はその成分だけ `null` にします。

DriverPose が提供するのは `pose_is_valid` と `tracking_result` などで、個別の position/orientation/velocity validity ではありません。
個別 validity は `null` です。Monado の後処理で付与される relation flags と混同しません。
`vecAcceleration` は取得できますが、この pin の OpenVR header は単位を meters/second と記載しているため
`linear_acceleration_driver` / `linear_acceleration_unit=openvr_header_says_m_s_ambiguous` として原値を保存します。
ドライバー内部での測定/推定・フィルタの内訳は取得不能です。Reference Pose はセンサー原測定値ではありません。

`capture_time_ns` は callback 入口の実測 `CLOCK_MONOTONIC`。
`pose_time_offset_s` はドライバーの原値。`pose_time_estimate_ns` は callback 時刻に offset を足した派生値です。
ドライバーの call と callback 入口の遅延は不明で、生成時刻や実測 reference timestamp として扱いません。
`generation_time_ns`, `reference_time_ns`, `prediction_horizon_s` は取得不能なので `null`。
DriverPose に prediction が含まれるかは特定できず、「無予測 Ground Truth」として扱えません。
全更新は enqueue を試行します。複数 callback thread ではキュー取得順と測定時刻/sequence 順が異なる可能性があります。
比較前に device ごとに timestamp を並べ、sequence gaps と counters を確認してください。

`driver_state_changed` は受信した flag/result の変化から writer が生成したイベントです。
無効化・再有効化、切断・再接続を識別できますが、更新が来なくなっただけのケースを切断と推測しません。
キュー損失中の状態遷移は復元できません。Base Station は callback が通知した識別子と class を announcement として保存します。

### USB 互換修正

既存 `--usbmon-record` を再利用しています。`--usbmon --usbmon-only-record --usbmon-record FILE` で
HTCVive のデフォルト排他的 driver を選ばず、patched only-record モードを受動にします。
デバイスを claim/detach せず、列挙では USB string descriptor のための open/read も行いません。
通常の tracking / offline replay の設定は変更しません。
256-byte snaplen を 65535 に拡大し、終了時 statistics、write error、nonblocking read、空読み時の待機を追加しました。
libpcap 1.11 の 4-byte-aligned ABI wrapper と旧 scalar field の両方に対応する memcpy 読取を使用します。
短い header と切り詰め payload の再生は警告して skip し、範囲外を読まないようにしています。

USB raw header は bus/address、endpoint/direction、transfer type、URB ID、S/C/E、status、length、data/setup flags、
kernel timestamp を保持します。原 payload は pcap に保存し、派生インデックスは位置を参照します。
IN submission / OUT completion の payload 不在は通常の usbmon 動作であり、それだけで損失とは数えません。
起動時に列挙した既知 VID/PID だけが保存対象です。再接続で新アドレスになったデバイスや未対応 VID/PID は
観測されない可能性があり、sysfs snapshots と validation の unobserved report に表示します。
アドレス再利用による機器対応の曖昧さは解消できません。全 USB traffic を保存するフラグは本 CLI では使いません。

## 時刻同期と誤差

[Linux mon_bin.c](https://github.com/torvalds/linux/blob/master/drivers/usb/mon/mon_bin.c) の binary timestamp は
`ktime_get_real_ts64`。[libpcap usb backend](https://github.com/the-tcpdump-group/libpcap/blob/master/pcap-usb-linux.c) は
その値を pcap header に転写します。これをこの Linux capture の clock basis として明記し、実データの epoch を
clock_sync の範囲で検査します。kernel の独自改造等で別 domain になっていれば変換不能として報告します。

各測定は monotonic-before → realtime → monotonic-after。
対応 offset は realtime − monotonic midpoint、read uncertainty は前後の半幅です。
同様に RAW 読取を MONOTONIC で挟んだサンプルも保存します。既定周期は1秒、CLI で0.05〜60秒に指定できます。
開始・記録中・終了のサンプルが揃わない session は正常扱いしません。

`align` は隣接測定で挟まれた USB realtime のみを monotonic に補間します。
外挿、backward jump による重複区間、offset の不連続区間では変換を生成しません。
不連続の閾値は 1ms + 区間長の500ppm + read uncertainties。suspend での realtime/monotonic 差も検出対象です。
推定誤差は両端の最大 read uncertainty + 観測 offset 変動 + USB の1µs量子化。
validation は最大 read uncertainty と USB alignment の最大推定誤差を数値で報告します。
これは保証された精度ではなく、測定の間に起きて戻った clock step、kernel capture latency、driver 内部遅延は
この値では拘束できません。同期精度と姿勢推定の精度は別です。
長時間セッションの変換インデックスは一時 SQLite と固定 page cache で作成し、全 packets を RAM に保持しません。

## Replay と比較ツールの後続設計

```sh
/path/to/patched/survive-cli \
  --usbmon-playback /absolute/path/session/usb.pcap.gz --playback-factor 0
```

`.usbdevs` は同じ filename の隣に必要です。`--playback-factor 0` は待機しない再生。
デフォルトの1で取得時のペースに従って再生します。元 pcap は変更しないため繰り返し利用できます。
実機での solve にはセンサー配置の configuration / calibration が必要です。
Valve が記録開始後に取得する設定応答を USB から収集するため、USB は Valve driver 初期化前に開始します。
設定が取得されなかった記録が reader に読めることと、pose を solve できることは別です。
`--usbmon-allow-fs-config` は不適切な機器設定を使う可能性があるため自動指定しません。

後続比較ツールは libsurvive の pose callback を別の `libsurvive_pose` stream に出力し、object ID と
再生中の原 URB timestamp / sensor ticks の対応を記録します。libsurvive の replay clock は最初の認識 packet を基準にするので、
単純に両 stream の開始時刻を引いてはいけません。原 USB timestamp と clock mapping を適用し、ドライバー offset / prediction が
不明な区間は区別します。USB receiver serial と tracked object serial は異なる場合があるため、捕捉した configuration
から対応を解決し、解決不能は未対応として残します。

同じ参照点に body offset を合成した後、同時観測に対する SE(3) の座標 alignment を求めます。
position の距離と quaternion geodesic angle（符号同値を考慮）を評価し、tracking lost / reacquisition を
state events から集計します。時間 alignment と空間 alignment の fitted values / residuals はそれぞれ保存します。
追跡不能・欠落・時刻不確かな区間を正常な error サンプルに混ぜない設計です。

## 公開用匿名化

`export-public` は session 内で一貫した salted SHA-256 logical IDs に置換し、salt を破棄します。
serial、host path、calibration、private config、USB payload を公開物に含めません。
USB の設定 payload 自体にシリアル等が入る可能性があるので、匿名性と無改変 replay を同時に保証しません。
公開 export は Pose/clock の共有用で、manifest に `usb_replay_available=false` を記録します。
完全な replay 用原 session は私的に保持してください。

## 自動テストと実機手順

```sh
# build-lh は上の WIVRN_BUILD_TEST=ON ビルド。
cmake --build build-lh --target lh-recording-settings-test lh-recording-startup-test
build-lh/tests/lh-recording-settings-test
# 実際の binary 配置は generator / CMAKE_RUNTIME_OUTPUT_DIRECTORY に従う。

# Pose 単体テストの harness。MONADO_SRC は0016適用済み checkout。
c++ -std=c++17 -Wall -Wextra -Werror -pthread \
  -isystem "$MONADO_SRC/src/external/openvr_includes" \
  -I"$MONADO_SRC/src/xrt/drivers/steamvr_lh" \
  "$MONADO_SRC/src/xrt/drivers/steamvr_lh/reference_recorder.cpp" \
  tools/lh-recorder/tests/pose_harness.cpp -o /tmp/lh-pose-harness
LH_POSE_HARNESS=/tmp/lh-pose-harness \
LH_STARTUP_HARNESS="$PWD/build-lh/tests/lh-recording-startup-test" \
  python3 -m unittest discover -s tools/lh-recorder/tests -v
```

`tests/fake_usb.py` は USB にアクセスしない合成 fixture です。起動時 snapshot / owner 終了 / manifest のテストで使用します。
`tools/lh-recorder/samples/synthetic-session` は合成 Pose と合成 URB、実際に測定した host clock brackets を含みます。
センサー由来のデータや solve 用 calibration は含みません。

実機では以下を実施してください。

1. OFF で接続: session 不生成、通常の Lighthouse tracking と同等の動作を確認。
2. ON で複数 tracker を起動: USB/Pose の生成、device IDs、config payload、validation を確認。
3. 記録中に Dashboard の OFF と出力先変更を保存: 今の出力先で更新が続くことを確認。
4. 再接続で Monado を再起動: 新設定の反映と session の衝突がないことを確認。
5. 静止、ゆっくり移動、高速回転、遮蔽、復帰、tracker OFF/ON を記録。
6. 数時間以上の記録で RSS、callback latency、kernel/pose drops を記録。遅いディスクでも tracking thread が待機しないことを確認。
7. 通常終了後に validation、2回の replay と実際の pose solve を確認。
8. usbmon 権限なし・disk full・プロセス強制終了の異常 session を確認し、通常 tracking が続くことを確認。

実施結果と未検証項目は `tools/lh-recorder/TEST_RESULTS.md` に記載します。
