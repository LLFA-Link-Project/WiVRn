# 微動計測ロガーの引き継ぎ (2026-10-24)

`~/libsurvive/docs/libsurvive-micro-motion-handoff.md` に対応するWiVRn側の実装。
libsurvive側の作業ツリーは変更していません。変更は本書を含むWiVRnコミットと
`patches/monado/0029-survive-record-native-and-monado-pose-stages.patch`。
パッチ内のMonadoコミットは `5e4ebce05e61eb2de946d4c8e7aaa25839d130d5` です。

## ビルドと実行ファイル

依存コピーはローカル `llfa-main` の `b28834318d1b21a4b4fa52439e5e830ef025456f`。
`d50e4e0` / `1fc8b3d` を含みます。

- WiVRn起動: `build-dashboard/server/wivrn-dashboard`
- サーバー: `build-dashboard/server/wivrn-server`
- 依存ライブラリ・plugin: `build-dashboard/_deps/libsurvive-micro-motion/libsurvive-llfa-main-install/lib/`
- ビルド用Monadoコピー: `build-dashboard/_deps/monado-recorder-src`

serverのRUNPATHとlddで上記libsurviveを参照することを確認します。
実セッションには、初期化後の `dl_iterate_phdr` によるロード先一覧とSHA-256、
実行ファイルのSHA-256、ビルド時のソースrevision/diff/patch SHA-256を保存します。
隣に置いたライブラリの名前からロード先を推測しません。
ビルド専用コピーを使うため、今回の `build-dashboard` のCMake cacheには
`FETCHCONTENT_SOURCE_DIR_MONADO` で上記Monadoコピーを指定しています。
新規ビルドでは既存のパッチ適用処理で0029も適用されます。

## 検証

- 更新したlibsurviveの関連CTest: 15/15成功。
- libsurviveロガーのPython/実Monadoアダプター統合テスト: 11/11成功。
  0/100 ms/無制限、raw/履歴/アプリ/手首の段階、serial/role、取得不能時刻のnull、
  受信double値の保持、明示的A/B、キュー上限8192と10万投入時の計数・drain、
  `/dev/full` での書込失敗、nativeのgzip破損、診断欠如、圧縮・展開を含む。
- 実libsurviveの空Playbackを、実server初期化・終了経路から実行。
  USB observerを開かず、native記録・設定snapshot・ロード先/ハッシュを保存し、
  空データをabnormalとして確定することを検証。追跡品質の成功試験ではない。
- 既存SteamVRロガー: 26件中20件成功、6件は未指定の任意harness/replay依存でskip。
  server起動・設定snapshot・正常/異常終了・USB権限失敗の回帰試験は実行済み。
- SteamVRバックエンドOFF / libsurvive ONの別CMake構成で記録オブジェクトのコンパイル成功。
- Monadoパッチを直前のパッチ適用済みソースに適用できることを確認。
- server/dashboard/OpenXR/Monadoをビルド。

主な再実行コマンド:

```sh
env LH_SURVIVE_HARNESS="$PWD/build-dashboard/tests/lh-survive-hotplug-test" \
    LH_STARTUP_HARNESS="$PWD/build-dashboard/tests/lh-recording-startup-test" \
    python3 -m unittest discover -s tools/lh-recorder/tests -p test_survive_recorder.py -v

env LH_STARTUP_HARNESS="$PWD/build-dashboard/tests/lh-recording-startup-test" \
    python3 -m unittest discover -s tools/lh-recorder/tests -p test_recorder.py -v
```

## 実機計測について

操作・schema・A→B→A・区間marker・終了後検証は
[libsurvive-recording.md](libsurvive-recording.md) にまとめました。
ダッシュボードの既存ON/OFF・出力先を両バックエンドで利用できます。
保存先は設定したディレクトリ内の `日時_libsurvive_…`。
raw入力は同じコンテキストのnative.rec.gz、Monado側は非同期JSONLです。
未観測のセンサーtick/Hzやvelocity時刻はnullで、nativeの各診断タグへ突合します。

実機の静止・手持ち微動・遅い並進/回転・速い動作のA→B→Aは未採取です。
現段階で静止ジッターや滑りの原因・改善は判定していません。
native側の同期記録負荷・入力drop・close結果は取得不能の範囲をmanifestに明示します。
セッションのタグ存在/CRC検査は、全入力の無損失や追跡精度の証明ではありません。
完全静止RMSだけで解決扱いにはしません。
