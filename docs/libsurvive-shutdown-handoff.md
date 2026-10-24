libsurvive の Codex への引き継ぎ指示書（2026-10-24）

`~/libsurvive` の `llfa-main` に、Global Scene Solver の終了時デッドロック修正を取り込んでください。
WiVRn 側では別コピーで修正・検証し、元の `~/libsurvive` の作業ツリーとブランチは変更していません。
そちらの進行中の変更と AGENTS.md を確認し、既存の作業を保持した状態で取り込んでください。

パッチ: [0002-fix-global-scene-solver-shutdown.patch](../patches/libsurvive/0002-fix-global-scene-solver-shutdown.patch)

- ベース: `2cdfabbcf5550a983de1c67035fcfff8ad7269cc`（作成時の `llfa-main`）。
- 別コピーで作成したコミット: `b074556`。パッチには著者・コミットメッセージ・回帰テストを含みます。
- 変更対象は `src/driver_global_scene_solver.c` と `src/test_cases/global_scene_solver.c` です。

実機では WiVRn の終了要求とアプリ切断が済んだ後も `wivrn-server` が残り、SIGKILL が必要でした。
採取したスタックでは、メインスレッドが
`survive_destroy → survive_simple_close → survive_close → DriverRegGlobalSceneSolverClose`
で Solver の終了を待ち、Solver 側は
`survive_threaded_gss_thread_fn → run_optimization → PoserMPFIT → solve_global_scene → survive_optimizer_run`
で計算中でした。Monado のイベントスレッドは終了済みです。

`survive_close` はコンテキストのセマフォを保持しています。一方、Solver は最適化の前後で
`survive_get_ctx_lock` を呼びます。コンテキストを保持したまま Solver を join すると、
Solver が計算後にロックを取り直せず、終了側と相互に待ち続けます。
既存の `threaded_driver_close` と同じく、join の間だけコンテキストを解放し、join 後に取り直す修正です。
併せて、終了後は保留中の solve を開始せず、`needsSolve` の消費を通知用ミューテックス内に移しています。

取り込み例です。既存の変更と競合する場合は手動で統合し、進行中の作業を reset で消さないでください。

```sh
cd ~/libsurvive
git status --short
git am --3way /home/reina/WiVRn/patches/libsurvive/0002-fix-global-scene-solver-shutdown.patch
```

既存のビルドがあればそれを使用できます。独立したテスト用ビルドの例は次のとおりです。
リプレイを使わない単体テストなので、extras データ用のディレクトリを先に作り、
configure 時の不要なリプレイデータ取得を避けています。

```sh
mkdir -p build-shutdown-check/src/test_cases/libsurvive-extras-data
cmake -S . -B build-shutdown-check \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_STATIC=OFF \
  -DENABLE_TESTS=ON -DBUILD_APPLICATIONS=OFF \
  -DDOTNET=OFF -DPYTHON_GENERATED_DIR= -DUSE_OPENBLAS=OFF
cmake --build build-shutdown-check --target test-global_scene_solver -j4
ctest --test-dir build-shutdown-check -R '^global_scene_solver$' \
  --output-on-failure --timeout 10
```

検証結果:

- `CloseReleasesContextWhileJoining`: USB 不使用。コンテキストを取り直す worker と終了側の待ちを再現。
  join 周囲の解放・再取得だけを元に戻すと `timeout 3` が終了コード 124、修正後は成功しました。
- `CloseSleepingWorker`: 待機中の実際の GSS worker を正常に終了できることを確認しました。
- `global_scene_solver` テスト一式: 成功しました。

この修正は、進行中の最適化を完了するまで待ちます。強制的なスレッドキャンセル、
最適化回数の制限、予測上限やフィルタ設定の変更は含みません。
計算そのものの終了待ちが長い場合、安全な協調キャンセルは別の変更として検討してください。
その場合は、計算結果をキャリブレーションへ反映しないこと、姿勢の内部表現と共分散処理を
中断時にも整合させること、join 前に ctx/オブジェクト/シーンを解放しないことを確認してください。

WiVRn 側の `tools/update-libsurvive-backend.sh` はこのパッチを専用コピーに適用します。
元リポジトリのチェックアウトは変更せず、更新前に自分の管理パッチだけを外します。
パッチが upstream に統合済みなら再適用をスキップします。

別件として、ユーザーは静止時のジッターをもう少し減らしたいと報告しています。
原因は未確定です。生の位置・向き・線速度・角速度・サンプル時刻を静止中に記録し、
姿勢推定自体の揺れと、速度の揺れを外挿が増幅する場合を分けて評価してください。
WiVRn 側は raw IMU 原点で姿勢と速度をそろえ、予測後にデバイス・手のオフセットを合成済みです。
最大予測時間は steamvr_lh と共有しています。0 ms にすると遅延が増えるとの実機報告があるため、
予測の無効化だけで解決した扱いにせず、今回の終了修正とは分けて評価してください。
