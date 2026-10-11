# WiVRn Server の外部 Lighthouse HMD 統合

## 対象と実装

WiVRnの表示用HMDを保持したまま、PC側のSteamVR Lighthouseドライバが返す位置・回転をHMD追跡へ取り込む。WiVRn Client、GStreamer、Orange Pi側Monado、ネットワークパケットには変更を加えない。クライアントのIMUとLighthouseの姿勢を混合する新しいフィルターは追加しない。Lighthouseドライバ自身の追跡・予測結果を使う。

`~/ArparaHIDProtocolReverseEnginiering/arpara-original.json` で確認したarparaのシリアルは `LHR-CEC1027F`、種別は `hmd`。この値をソースへハードコードしていない。同JSONの `head.plus_x` / `head.plus_z` はドライバが解釈するヘッド基準の校正情報であり、WiVRn側で重複適用しない。

- シリアル指定：`WIVRN_EXTERNAL_HMD_SERIAL`。Lighthouseドライバが提供するHEAD_POSEを優先し、Generic TrackerのTRACKER_POSEにも対応する。
- 自動選択：`WIVRN_EXTERNAL_HMD=auto`。Lighthouseドライバが最初に認識したHMDを使用する。複数のHMDを接続する場合はシリアル指定を使う。シリアル指定が優先される。
- デバイスはarparaに限定しない。対象はこのサーバーでSteamVR Lighthouseドライバが認識できるHMD／トラッカー。任意のUSB HMDや他のMonadoドライバを自動的に追加する機能ではない。
- 指定時はLighthouseドライバを自動的に有効にする。デバイスが見つからなければ接続初期化をエラーで終了し、別の追跡源へ黙って切り替えない。

追跡専用HMDは表示コンポーネントを持たなくても有効化できる。WiVRnの解像度、FOV、左右眼のヘッド相対姿勢、可視性マスク、ユーザー存在状態、映像配信は従来のクライアント情報から得る。OpenXRのHMD pose取得とview pose取得の両方で外部6DoFを使い、クライアントの3DoFフラグで外部位置追跡を消さない。Lighthouseコントローラ／トラッカーと同じtracking originを共有する。

## ビルドと起動

PC側でUSB/IP接続を済ませ、SteamVRのLighthouseドライバを利用できる状態にする。クライアントの既存ビルドを変更する必要はない。以下はすべて一行のコマンド。

```bash
cmake -S . -B build-server-lighthouse -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWIVRN_BUILD_CLIENT=OFF -DWIVRN_BUILD_SERVER=ON -DWIVRN_BUILD_SERVER_LIBRARY=ON -DWIVRN_BUILD_DASHBOARD=OFF -DWIVRN_FEATURE_STEAMVR_LIGHTHOUSE=ON -DWIVRN_BUILD_TEST=ON
cmake --build build-server-lighthouse --target wivrn-server wivrn-external-head-test wivrn-lighthouse-driver-test --parallel 4
ctest --test-dir build-server-lighthouse --output-on-failure -R 'wivrn-(external-head|lighthouse-driver)'
WIVRN_EXTERNAL_HMD_SERIAL=LHR-CEC1027F ./build-server-lighthouse/server/wivrn-server > /tmp/wivrn-lighthouse-arpara.log 2>&1
WIVRN_EXTERNAL_HMD=auto ./build-server-lighthouse/server/wivrn-server > /tmp/wivrn-lighthouse-auto.log 2>&1
```

`WIVRN_BUILD_SERVER_LIBRARY=ON` では、`wivrn-server` ターゲットと一緒にOpenXR／libmonadoライブラリも更新する。古いライブラリが残ると、アプリ側のランタイム初期化やサーバーとのIPC接続に失敗する可能性がある。

最後の二行は別々の起動方法。一度に両方を実行しない。既存のWiVRnサーバーを停止してから実行する。必要なら `STEAMVR_PATH` でSteamVRのインストール先を指定する。デバイスの検出待ちは既存の `LH_DISCOVER_WAIT_MS` を利用する。

確認するログ：

```bash
rg 'Lighthouse device:|WiVRn HMD tracking:|External HMD Lighthouse|Activated tracking-only|Ignoring HMD|Activating HMD failed' /tmp/wivrn-lighthouse-arpara.log
```

Lighthouseの初期化はクライアント接続時に行われる。`External HMD Lighthouse 6DoF tracking acquired` が出てから、VRアプリで頭部を前後左右上下に動かし、平行移動も反映されることを確認する。Lighthouseの遮蔽時には `tracking unavailable or stale`、復帰時には `tracking acquired` が出る。

## ヘッド中心の取り付け補正

既定ではLighthouseドライバのヘッド姿勢をそのまま使用する。OpenVRの `DriverFromHead` 変換はドライバ側で適用済み。別の汎用トラッカーを取り付ける場合だけ、追加の固定変換を設定する。

`WIVRN_EXTERNAL_HMD_POSE_OFFSET=x,y,z,qx,qy,qz,qw` は **ドライバが返す追跡基準からヘッド中心への変換**。位置はメートル、回転はXYZWの単位Quaternion。`world_from_head = world_from_source * source_from_head` として適用し、取り付け位置による速度もMonadoのrelation chainで変換する。arparaの校正行列を推測してここへコピーしない。

## 時刻・追跡喪失・所有権

追加したMonadoパッチは、実際の最新追跡観測の時刻とrelationを履歴から取得する内部APIを提供する。OpenVRの秒単位 `poseTimeOffset` を符号付きナノ秒へ変換し、履歴読み書きを保護する。入力が未接続または追跡が劣化した場合は6DoFとして受理しない。

最新観測が100msより古い場合はhead relationを無効とする。予測は観測時刻から最大20msまで。新しい観測がなくても無限に追跡済み姿勢を予測し続けない。外部追跡喪失時にクライアントIMUの任意のヨー座標へ切り替えることはせず、無効な追跡として通知する。アプリ側の追跡喪失時の表示は実機で確認が必要。

Lighthouseデバイスはsessionが所有し、HMD側の追跡参照は非所有。接続初期化で例外が起きた場合も、検出済みLighthouseデバイスを解放する。映像描画用HMDそのものを破棄・入れ替えない。

## multi_wrapper の検討

Monadoの `multi_create_tracking_override(XRT_TRACKING_OVERRIDE_DIRECT, ...)` は表示用デバイスと追跡用デバイスを組み合わせ、view poseのhead relationも差し替えられるため、用途は適合する。ただし現在のWiVRnはセッション内の `wivrn_hmd` 具体型を保持し、layer squasherも直接参照する。またwrapper生成時の `u_device_populate_function_pointers` はWiVRn独自の可視性マスクなどのコールバックを標準処理へ置き換える。

そのため今回はHMD全体をwrapperへ置換せず、既存HMDのhead relationだけを置き換える方が変更が少ない。取り付け変換にはmulti_wrapperと同じMonado relation chainを利用する。鮮度・追跡状態・予測上限のチェックはwrapperを使う場合にも別途必要となる。

## 検証と未検証事項

ローカルx86_64でWiVRn Serverのビルド成功、外部headの単体試験と模擬OpenVR providerによる統合試験は5/5成功。統合試験は実際のLighthouseドライバ読み込み経路を通し、表示コンポーネントを持たないHMD、シリアル指定、自動選択、誤ったシリアル、通常モードの既存挙動、秒からナノ秒への変換、ヘッド変換の一回適用、追跡喪失と復帰、終了時解放、再生成を確認した。依存Monadoパッチがリポジトリ指定のrevisionと既存パッチ列の後に適用できることも確認した。Lighthouseをビルドで無効にした場合のセッションコードは構文チェックに成功した（その構成の全体リンクは未実施）。

**USB/IP越しの実arpara、SteamVR Lighthouseでの実6DoF、ルーム空間と他トラッカーの整合、再接続、VRChatの表示は未検証。** 実機ではまず上記シリアル指定で検出と6DoF取得を確認する。その後、ヘッド基準とルーム校正、長時間動作、遮蔽／復帰を確認する。Orange Pi側IMUとPC側Lighthouseの姿勢基準差、ローカルリプロジェクション時の長期ドリフトは今回のサーバー統合だけでは解決済みとはしない。
