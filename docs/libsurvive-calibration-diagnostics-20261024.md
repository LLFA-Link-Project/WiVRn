# 較正診断の更新と固定初期値による比較 (2026-10-24)

# Historical note (2026-10-28)

This diagnostic option and the POSE_STATE/POSE_BIAS records described below
belong to an older libsurvive experiment branch. Current `~/libsurvive` does not
provide them, and WiVRn no longer passes `record-tracking-state`.

後続の手持ち A/B では、[20:37 の新しい固定較正と更新版バイナリ](libsurvive-handheld-jitter-ab-20261024.md)を使う。以下はそれ以前の較正診断の記録。

`~/libsurvive/docs/libsurvive-calibration-diagnostics-handoff-20261024.md` の返答を反映。
依存コピーは `llfa-main` の `2f2e69fb3330736fad2785d8645fa5a22ea61cf2` に更新。
診断修正 `4def8c2` を含む。元の `~/libsurvive` は変更しない。
今回の更新は較正品質の診断であり、較正失敗や体感ジッターを解消したという更新ではない。
予測・filter・bias・静止判定の実行設定は変更しない。

## 解析結果の訂正・更新

- 不良な初期較正からの数十m級発散とresetは、full stateを固定したQ=0でも再現した。
  bias random walk追加がなくても起きるが、Qが全状況で無関係という証明ではない。
- 最初の悪い新規較正は、初期値なしの直列再生でも、実時間・両worker有効の3回の再生でも再現していない。
  liveの初回scene選択・スレッド順序・記録負荷まで一致していない。
- 19:34の100秒超の時刻差は**無効姿勢だけ**だった。有効姿勢の差は約−2.4〜+6.8 ms。
  未初期化timecode=0の変換に由来するため、追跡の実遅延や発散原因の証拠にしない。
  19:36の有効姿勢には最大約251 msの差があり、更新途絶などは別途確認する。
  いずれもcallbackと推定姿勢時刻の差で、物理入力→描画の実測遅延ではない。
- 古い `up err` は軸間の誤差が相殺する集計だった。新しい値と過去値を直接比較しない。
  光学・物体重力・BS重力の指標も、それぞれの観測・定義を分けて解釈する。

## 新しい記録

既存の `record-tracking-state=1` で、native.rec.gzと通常ログへ次が追加される。

```text
GSS_QUALITY status=... scenes=... optical_rms_rad=... optical_count=... object_gravity_rms_deg=... object_gravity_count=...
GSS_LIGHTHOUSE_GRAVITY index=... channel=... angle_deg=...
```

失敗した最適化候補も含む。出力された行を「採用済み」「十分な品質」とは判断しない。
欠測角度のnanは誤差ゼロではない。BSの取り付け角を制限する機能でもない。
raw-input再生だけではGSSが採用したscene/観測/初期値/順序までの完全再現は保証しない。
完全なGSS入力snapshotは今後の課題。

## 固定した自己較正

liveで更新される普段のconfigは比較の初期値にしない。
libsurvive側の評価と同じ19:54記録の不変snapshotをコピーした。

- 元: `/home/reina/LH-Dump/2026-10-24_195448_libsurvive_111892475_777039/calibration.initial.json`
- 固定コピー: `/home/reina/WiVRn/build-dashboard/diagnostics/calibration-comparison-20261024/baseline.json`
- SHA-256: `424dea8f455a67de635da57894f85ee56acf8121f1d956d1351b43d4ec0af748`
- 来歴: 同ディレクトリの `baseline-provenance.json`

固定コピーは読み取り専用にしてある。普段の設定も記録内の原本も上書きしていない。
用意した `trial-good-01/config.json` は同じ内容の書き込み可能な試行用コピー。
`trial-fresh-01/config.json` はまだ存在せず、新規較正用として使える。
これらの試行を実機で起動したわけではない。

## 最初に行う比較：良好初期値と新規較正

各実行前にダッシュボードとserverを終了する。WiVRnディレクトリで実行する。
記録を有効にし、libsurviveを選択、clamp=0 msを維持する。
比較途中で他の推定設定・BS・置き方を変更しない。

良好な初期値を使う場合は、毎回新しい試行コピーを作る。
`install -m 600` は固定コピーの読み取り専用属性を引き継がず、nativeの保存先を分離する。

```sh
baseline="$PWD/build-dashboard/diagnostics/calibration-comparison-20261024/baseline.json"
trial_dir="$(mktemp -d /tmp/wivrn-survive-seeded.XXXXXX)"
install -m 600 "$baseline" "$trial_dir/config.json"
sha256sum "$trial_dir/config.json"
env -u WIVRN_SURVIVE_BIAS_COMPARISON -u SURVIVE_INIT_CONFIGFILE \
    SURVIVE_CONFIGFILE="$trial_dir/config.json" \
    ./build-dashboard/server/wivrn-dashboard
```

新規較正は同じバイナリ・同じ設定で、初期configだけ存在しない専用パスにする。

```sh
trial_dir="$(mktemp -d /tmp/wivrn-survive-fresh.XXXXXX)"
env -u WIVRN_SURVIVE_BIAS_COMPARISON -u SURVIVE_INIT_CONFIGFILE \
    SURVIVE_CONFIGFILE="$trial_dir/config.json" \
    ./build-dashboard/server/wivrn-dashboard
```

実行ごとに保存された `survive-settings.json`、`native-options.json`、較正snapshot、
manifest内の実ロードcore/pluginパス・SHAを確認する。
新規較正が失敗した場合も、強い平滑化や閾値変更を加えず、そのnative記録と診断を残す。

まず上の比較で良好初期値と新規較正の追跡可否を分ける。
較正が良好な条件で [微動区間の記録](libsurvive-recording.md) に進む。
Qの追加比較ではfull stateを明示し、Qだけを変更し、同じ固定snapshotから各試行を始める。
既存のWIVRN_SURVIVE_BIAS_COMPARISONモードは通常、新しい較正から始めるため、
そのまま良好初期値とのQ比較を兼ねたものにはしない。
今回こちらで実機の比較試行やQの追加評価を行ったわけではない。
