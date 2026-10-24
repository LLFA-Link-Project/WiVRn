# 新規較正と発散の実機記録 (2026-10-24)

WiVRnのロガー移植後に追跡が大きく飛ぶとの報告があり、19:34/19:36の実記録を調査した。
原因の全体は未確定。WiVRn側の設定パス回帰を特定・修正したが、それだけで
libsurvive側の新規較正の収束性・悪い初期推定からの回復性を説明したことにはしない。

## 確定した回帰と修正

ロガーが `config.json` をCWD基準の絶対パスへ変えて渡していた。
libsurvive本来の解釈では、裸のファイル名はXDG_CONFIG_HOME/libsurvive、
未設定ならHOME/.config/libsurviveが基準。
実機の従来の較正は `/home/reina/.config/libsurvive/config.json` で、
15:40終了時のファイルが残り、3台ともPositionSet/OOTXSet=1だった。

最初の19:34記録は別ファイル `/home/reina/WiVRn/config.json` を指定し、
既存較正のinitial snapshotがない。19:36記録は前回の不安定な較正結果を初期値として読む。
従来ファイルはこの2実行では変更されていない。
0030のMonadoパッチで元の引数表記をlibsurviveへ渡し、snapshotだけを本来の規約で解決する。
XDG/HOME/裸のカスタム名/明示的な./相対パスと、実ライブラリの較正読込・保存を回帰テストした。
不安定なCWD側のファイルや記録は削除・上書きせず保存している。

## 収録物

- `/home/reina/LH-Dump/2026-10-24_193444_libsurvive_112556333_758814`
- `/home/reina/LH-Dump/2026-10-24_193640_libsurvive_225267645_761146`

各フォルダのnative.rec.gz、poses.jsonl.gz、manifest、native-options、
較正snapshot、build-provenance、loaded-survive-objectsを使用する。
両方ともWiVRn `8f9dfb70` / Monado `5e4ebce05` / libsurvive `b28834318`。
実行中のA/B指定なし。process-weight-gyro-bias/acc-biasは両方1e-8。
clamp設定も記録された実効追加予測も0。
writer drop=0、unwritten=0、終了済み、native gzip読み取り/CRC成功。
ただしnative同期ロガーの負荷や入力lossがゼロだった証明ではない。

## 観測

19:34の新規較正ではMPFITの反復上限到達が繰り返され、成功ステータスの回も
up error約0.128、trace約0.031。15:35の正常時にはup error約0.0001程度だった。
この値を単独の絶対精度や真値との誤差として解釈しない。

19:36の128秒程度の記録では、Monadoへ入る前のbackend_received段階で既に発散している。
世界原点からの最大距離は左Index26.802 m、右Index20.967 m、Tracker23.460 m。
callback間隔20 ms以下で10 cmを超える位置変化は、左625回、右624回、Tracker289回。
最大の一回変化は右約20.97 m。手首オフセットや追加予測で生じたものではない。
自然な動作の真値がないため、この集計をそのまま運動誤差RMSEとは呼ばない。

同記録にはGSSの較正失敗と `Too many failures ... reseting calibration` が繰り返される。
加速度残留biasが±0.1 gに張り付く例もあるが、原因か発散後の結果かは未確定。
初回記録では単純API由来のpose時刻とcallbackの推定差がIndexだけ100秒以上になる例もある。
この差を物理入力→出力の実測遅延とは解釈しない。センサー時計の対応やreset時の扱いも確認候補。

## libsurvive側での改善・切り分け候補

1. 同じraw入力を、空の専用較正から始める条件と、以前のlibsurvive自身の較正を初期値にする条件で再生比較。
   Valveの外部較正を使う比較ではない。
2. 同じ最新バイナリ、full state明示のままbias random walk 0/1e-8を比較する。
   較正の初期値・threading・入力・他の推定設定まで同時に変えない。
3. 初期BS配置、基準BSの選択、観測配置の退化、外れ値、GSSの採用条件・再試行を調べる。
   solverの成功コードだけで較正の品質が良いと判断しない。
4. 信頼度の低い較正を保存・次回再利用する判断、発散やresetからの復帰を検討する。
   今回の悪い結果が保存された事実だけで、保存禁止の閾値を決めない。
5. 初期化・較正中・追跡喪失の品質状態を単純APIから利用側へ伝えられるか確認する。
   Monado側も有限な数値という理由だけで良好な追跡と扱うべきではない。
   メートル単位の飛びを平滑化や強い予測clampで隠す変更は解決扱いにしない。
6. nativeロガー有無の負荷、およびセンサーtimecode→単純API時刻の対応とreset前後を分けて調べる。

再生では実ファイルを上書きしないよう、configfile/record/outputを必ず専用の一時ディレクトリへ向ける。
ライブと直列再生ではGSS/filterの実行順が変わり得るため、数値一致や再現性の範囲を明記する。
パス修正後の実機で以前の較正を使った確認は、修正時点では未実施。
