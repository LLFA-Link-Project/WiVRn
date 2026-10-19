# Lighthouse ベースステーションを基準にする実験機能

この機能は、WiVRn と VIVE Tracker の空間合わせがずれる原因を測定するための実験機能です。
ベースステーションとトラッカーの pose を同じ変換段階で記録し、座標変化が共通の剛体変換か確認します。
実験用の安定化は、シリアルで固定した BS の位置・姿勢を使い、Lighthouse デバイス群を BS 原点・BS の軸へ変換します。
WiVRn HMD の座標や motoc の校正 offset は変更しません。既定では無効です。

`patches/monado/0019-d-steamvr_lh-record-diagnostic-base-stations.patch` を、
`monado-rev` と既存のパッチ 0001–0018 に追加して適用します。
安定化処理は `0020-d-steamvr_lh-stabilize-against-selected-base-station.patch` に含まれます。
通常の WiVRn ビルドでは CMake がこのパッチも適用します。

## 設定画面から有効にする

1. 新しいサーバーと Dashboard をビルドして起動し、Lighthouse を有効にします。
2. 初回は、補正 OFF のまま一度 HMD とトラッカーを接続します。ドライバが初期探索で通知した BS のシリアルを候補として自動保存します。記録機能は OFF でも構いません。
3. Dashboard の設定を開き直し、`Use a base station as tracking origin (experimental)` を ON にします。
4. `Reference base station serial` で候補を選びます。初回からシリアルが分かっていれば手入力もできます。保存した手入力も次回の候補になります。
5. 設定を保存し、HMD を切断・再接続して反映します。**VR 内で位置合わせを使う場合は、初回の有効化後と基準 BS の変更後に motoc で校正し直してください。**
   pose ログだけを使う遮蔽・復帰試験には motoc の再校正は不要です。
6. 台とトラッカーを固定して静止状態を記録し、10秒程度遮蔽してから復帰後30秒程度を記録します。正常終了後に補正前後を比較します。

候補が増えても選択した基準を自動では切り替えません。補正を OFF にしても候補は保持します。
設定を開いている間に検出された候補は、その画面を保存しても失われません。選択肢の更新は画面を開き直すと反映されます。
補正を OFF に戻した場合も座標規約が変わるため、その規約に合った校正を使ってください。

保存先は通常 `~/.config/wivrn/config.json` です。`XDG_CONFIG_HOME` やサーバーの `-f` 指定がある場合はそちらを使います。
`lighthouse-reference-serial` が選択中の基準、`lighthouse-reference-candidates` が記憶した候補の配列です。
シリアルが空または未指定なら補正は無効です。候補だけでは補正を有効にしません。
実行中の接続には設定を適用せず、次の Monado 起動時の設定を使います。
サーバーは内部環境変数 `LH_STABILIZE_REFERENCE_SERIAL` で Monado に渡します。WiVRn では Dashboard/config の設定が優先されます。

## 補正と基準喪失時の動作

補正前の BS pose を B、デバイス pose を P とすると、`inverse(B) × P` を履歴へ入れます。
毎回 BS 原点・BS の軸を使うため、起動ごとに初回 pose を新しい基準として保存する方式ではありません。
固定した同じ BS と同じ座標規約を使う限り、再起動後も同じ規約です。保存する基準 pose ファイルはありません。
規約の識別子は `base_station_origin_xyzw_v1` で、記録の manifest に選択シリアルとともに残します。
既存のずれた motoc 校正を自動で修復する機能はありません。

補正は、Lighthouse HMD を含め当該 Context の pose に適用します。WiVRn の HMD は別のデバイスなので対象外です。
速度は補正の回転だけで変換し、補正の切り替えを移動速度にはしません。
補正済みサンプルを履歴に入れ、その後で既存の予測処理を使います。過去の pose に現在の BS 値を掛け直しません。

原 DriverPose の world transform が BS と一致し、かつ BS の pose 推定時刻・受信時刻が将来にない組を使います。
world transform の一致は並進および quaternion 成分の距離がそれぞれ 1e-5 未満で判定し、quaternion の符号反転は同一の回転として扱います。
BS 値は低頻度なので保持しますが、別の world transform のデバイスに古い値を適用しません。
直近64回分の補正区間を保持します。

| 記録・ログの状態 | 動作 |
| --- | --- |
| `waiting_for_reference` | 有効な基準が未取得。補正対象の pose は無効 |
| `tracking` | 対応する有効な基準を使って補正 |
| `held` | 最後の基準から1秒超、または基準の無効通知を受信。同じ world transform なら最後の補正を保持 |
| `waiting_for_matching_reference` | world transform の切り替わり、未来の基準しかない等。対応する BS 更新が届くまで pose は無効 |
| `reference_changed` | world transform 外で基準 pose が初回から5 cm超または約3°超変化。再起動まで pose を無効化 |

指定した BS が見つからなくても、無補正や別の BS に自動では戻りません。
`held` に保持時間の上限はありません。低頻度の固定基準を保持している状態であり、現在も観測できているという意味ではありません。
world transform がデバイスごとに異なるドライバ構成では、一致条件を満たさず pose が無効になる可能性があります。

`reference_changed` は基準の移動や非共通の再推定を疑うための検出です。
単一の BS では、その物理移動が共通の world transform として表現された場合と座標系の変更を区別できません。
複数 BS の相互整合性の自動判定は今回の補正には含めません。BS は固定したまま試験してください。

## 記録する

1. [Lighthouse recorder](lighthouse-recorder.md) の記録を Dashboard で有効にし、出力先を設定します。
2. 実験するベースステーションと検証用トラッカーを物理的に固定します。
3. WiVRn サーバーを終了し、**サーバーのプロセスに**次の環境変数を渡して再起動します。

   ```sh
   LH_DIAGNOSTIC_REFERENCES=1 /path/to/wivrn-server
   ```

   Dashboard や systemd から起動する場合も、起動されるサーバーに環境変数を渡してください。
   この設定は起動時に読み取ります。既定値は無効です。
   recorder の有効化には既存の Dashboard 設定を使います。
   `LH_RECORDER_ENABLE` と `LH_RECORDER_OUTPUT` は内部受渡し用です。
4. サーバーログで `Activated diagnostic base station SERIAL at index INDEX` を確認します。
   `Found lighthouse base station` だけでは Activate 成功や有効 pose の取得を意味しません。
5. 正常終了後に session の `poses.jsonl.gz`、`events.jsonl.gz`、`pose_status.json` を確認します。
   `device_class=4` がベースステーション、`device_class=3` が GenericTracker です。
   announcement はあっても pose がない場合、ドライバが pose を返しているとは判断できません。

初期探索だけに対応します。遅れて現れたデバイスが拒否された場合は、まず
`LH_DISCOVER_WAIT_MS` を増やして起動し直してください。ベースステーション用の index は 17–32、
コンテナ handle は 18–33 です。HMD の index 0 とコントローラの index 1–16 は専用の枠として残します。
1 Context あたり最大16回の参照デバイスの Activate を試せます。失敗した index は遅延コールバックとの混同を防ぐため再利用しません。
同じシリアルまたは driver オブジェクトの重複登録は拒否します。

診断オブジェクトは Context が所有し、Activate の前に登録します。
ドライバの Activate / Deactivate / Cleanup 呼出し中は `devices_mut` を保持しません。
正常に Activate した参照デバイスは Context 終了時に Deactivate し、provider の Cleanup 後に解放します。
コントローラの入力プロファイルや OpenXR のデバイス列挙には追加しません。

## pose の解釈

原 DriverPose の double 値とフラグは従来のフィールドに保存します。
追加フィールドも schema version 1 の JSON に含めます。旧ログでは追加フィールドがありません。

| フィールド | 内容 |
| --- | --- |
| `composed_pose` | `chaperone × world_from_driver × raw_pose × driver_from_head` の position と orientation |
| `composed_coordinate_frame` | `lighthouse_chaperone_before_tracking_origin` |
| `chaperone_transform` | 当該 Context が使う chaperone の position と orientation |
| `stabilization` | 補正有効時の状態、適用フラグ、基準推定時刻、補正変換、補正後 pose・速度・relation flags |
| `universe_id` | 当該デバイスの `Prop_CurrentUniverseId_Uint64`。精度を失わない10進文字列。`"0"` は未取得 |

pose オブジェクトは `position_m` と `orientation_xyzw` を持ちます。
`composed_pose` は Monado の pose 合成で得た float 精度の値です。
motoc の tracking-origin offset、WiVRn HMD の変換、アプリ向けの予測は含みません。
ベースステーションも同じ合成処理を使い、診断 pose を予測履歴や公開デバイスには登録しません。
`composed_pose` は常に補正前です。`stabilization.output_pose` は補正後・アプリ向け予測前で、同じキュー項目に保存します。
原 pose と追加データは同じキュー項目に保存します。callback 内にファイル I/O や JSON/gzip 処理は追加しません。

chaperone は Context の最初のデバイス作成時に一度だけ読み込み、Context 内で共有します。
読み込み失敗時は既存のエラーログを確認してください。identity transform だけでは読み込み成否を判断できません。
設定ファイルを変更した場合はサーバーを再起動してください。
`universe_id` と `chaperone_transform` は公開用の `export-public` から除外します。
`universe_id` は chaperone ファイルから選択した universe の識別情報ではありません。
読み込む universe の選択規則は既存実装のままです。

`capture_time_ns` は callback 入口の CLOCK_MONOTONIC、`pose_time_offset_s` はドライバの秒単位 offset です。
`pose_time_estimate_ns` は両者を足した推定時刻で、ドライバ内部から callback までの遅延は不明です。
有効状態は接続フラグ、pose 有効フラグ、tracking result を合わせて判断してください。
キューは有限で、競合・満杯時にはサンプルやイベントが落ちます。`pose_status.json` の `dropped` と `error` を必ず確認します。
各 pose の `device_class` は announcement の欠落に依存せず保存します。

## 相対 pose を解析する

シリアルを明示し、保持を許す基準 pose の最大経過時間を指定します。
次の 1000 ms は解析設定の例であり、実機で妥当性を保証する値ではありません。

```sh
python3 tools/lh-recorder/base_station_diagnostics.py /path/to/session \
  --reference 'BASE_STATION_SERIAL' --max-age-ms 1000 \
  --samples-output /tmp/lighthouse-relative-poses.jsonl
```

標準出力はデバイス別の JSON 集計です。出力ファイルには絶対 pose、基準 pose、
`inverse(base) × device` の相対 pose、両方の推定時刻・受信時刻・universe ID を記録します。
出力ファイルは新規作成し、既存ファイルを上書きしません。
別のベースステーションも比較対象に含むため、台間の相対 pose も確認できます。

解析は過去の最新の基準サンプルを保持して組み合わせ、未来のサンプルや補間・予測は使いません。
接続中かつ有効で tracking result が Running_OK のサンプルのみを測定値として扱います。
基準未取得、基準無効、最大経過時間超過、デバイス無効、時刻順序の逆転は除外数として報告します。
古い有効な基準値を、より新しい無効通知を越えて使うことはありません。
基準ログをメモリに保持し、トラッカーログはストリームで読み取ります。

`max_absolute_displacement_m` / `max_absolute_rotation_deg` は最初のペアからの絶対 pose の最大変化、
`max_relative_displacement_m` / `max_relative_rotation_deg` は相対 pose の最大変化です。
`max_relative_step_m` / `max_relative_step_deg` は連続する有効ペア間の最大変化です。
無効・基準喪失区間では step の連続性を切りますが、初回からの変位は継続して測ります。
これらはペアを作れた区間だけの統計です。除外区間の安定性は示しません。

この比較は固定デバイス向けです。基準値の更新が遅いと、新旧の座標系が混ざったペアになります。
時刻が近いことだけでは座標世代が同じとは証明できません。集計は自動的な合否判定を出しません。
最大経過時間を大きくして除外を減らすだけでは、低頻度の基準 pose を検証したことにはなりません。

## 実機での判断

同じ固定配置で、静止、トラッカー遮蔽と再捕捉、ベースステーション1台の見失いと復帰、
トラッカー再接続、WiVRn 再起動、問題の座標ずれの再現条件をそれぞれ記録します。
診断 OFF と ON でトラッキングの挙動も比較してください。
再起動ごとのログを保持し、再起動前後の相対 pose は出力されたペアから別途比較します。
集計の初期値は session ごとに取り直すため、session 内の最大変位だけでは再起動をまたぐ再現性を判断できません。

- 絶対 pose のジャンプに対してトラッカーと台間の相対 pose が保たれるなら、共通変換を相殺する根拠になります。
- 基準 pose が変わらずトラッカーだけずれるなら、その基準出力を使う補正は成立しません。
- 台間の相対 pose も変化するなら、単一の共通変換だけでは説明できません。
- 切り替わり中だけ相対 pose が飛ぶなら、両者の更新時刻と最大誤差を調べ、更新保留の方針を決めます。

基準台を実際に動かした場合と座標系変更を区別できるかも確認します。
この実験機能は、まず単一の固定 BS を使った遮蔽・復帰で評価してください。校正の再利用精度は実機で確認します。
WiVRn HMD 自身の再ローカライズやドリフトは、この方式での補正対象外です。

## 自動検証

専用 Monado checkout に固定 revision と全パッチを適用して実行します。
`patches/apply.sh` は作業ツリーをリセットするため、既存の開発 checkout に直接実行しないでください。

```sh
cmake -S "$MONADO_SRC" -B /tmp/lh-reference-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DXRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE=ON
cmake --build /tmp/lh-reference-build --target tests_steamvr_lh_references tests_steamvr_lh_stabilization
ctest --test-dir /tmp/lh-reference-build -R steamvr_lh_ --output-on-failure
LH_REFERENCE_HARNESS=/tmp/lh-reference-build/tests/tests_steamvr_lh_references \
  python3 -m unittest discover -s tools/lh-recorder/tests -p test_base_station_diagnostics.py -v
```

模擬ドライバは Activate 中の同期・別スレッドのコールバック、プロパティ、
失敗した index の後始末、重複・上限・遅延検出、Deactivate/Cleanup の順序を検証します。
記録試験は回転・並進を含む local/world 合成を照合します。
解析試験は共通の回転・並進に対する相対 pose の不変性、トラッカーだけのジャンプ、無効・古い基準を検証します。
Valve の非公開ドライバが実機で返す pose の妥当性は、これらの試験からは判断できません。

OpenVR のライフサイクルと DriverPose の定義は [Valve Driver API](https://github.com/ValveSoftware/openvr/blob/master/docs/Driver_API_Documentation.md) を参照してください。
