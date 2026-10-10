# Monado 担当 Codex への作業指示：arpara 3DoF の位置有効フラグ

## 目的と作業範囲

`/home/reina/monado` を編集できる環境で、arpara ドライバの固定位置を「有効だが未追跡」として公開する最小修正を適用し、既存設定でビルド・テストする。WiVRn のロビー表示を妨げる姿勢初期化の条件を解消するための修正である。

この指示書を作成した Codex は Monado のソースを変更していない。WiVRn 側の UI 初期姿勢、無効 pose の除外、view 有効フラグの検証、Vulkan feature 選択は別リポジトリで変更している。両リポジトリの変更は別々に管理する。

## 調査で確認した事実

調査時の両リポジトリは未コミット変更なし。

- WiVRn: `dc14ced6` (`26.9-69-gdc14ced6`)。
- Monado: `25a6b5588` (`v25.1.0-901-g25a6b5588`)。
- 現在の作業環境は x86_64。Orange Pi 5 / PanVK / arpara 実機の表示は観測できていない。

`src/xrt/drivers/arpara/arpara_hmd.cpp` の `get_pose()` は出力をゼロ初期化し、IMU の orientation を設定するが、常に返す有効フラグは `XRT_SPACE_RELATION_ORIENTATION_VALID_BIT` だけである。`POSITION_VALID_BIT` は設定されない。IMU の鮮度・接続状態に応じて `ORIENTATION_TRACKED_BIT` を追加する。

この固定原点は 3DoF の描画用の推定位置として公開できる。`POSITION_VALID` は値が利用可能であるという意味であり、実測位置追跡を意味する `POSITION_TRACKED` とは別である。位置を実測していないことを理由に VALID も外すと、VIEW を STAGE に対して locate するアプリケーションは位置を利用できなくなる。

OpenXR 仕様では有効フラグがない成分をアプリケーションが読んではならない。推定／固定位置は VALID を設定し、TRACKED を外して扱う。

参照: [OpenXR 1.1: view state / space location](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#XrViewStateFlagBits)

## WiVRn の障害との因果関係

1. `client/scenes/lobby.cpp` の主 UI、ポップアップ、キーボードの viewport は position と orientation を省略して構築されていた。
2. 省略された GLM quaternion はこの構築方法ではゼロになる。使用中の GLM 1.0.2 と同じ初期化を小さな C++ プログラムで再現し、`0,0,0,0` を確認した。明示した初期単位 Quaternion は recenter tip の viewport のみに存在していた。
3. UI を頭部の前に配置する `move_gui()` は、`application::locate_controller(VIEW, STAGE, time)` が成功した初回の再配置で呼ばれる。
4. このヘルパーは位置と向きの VALID フラグを両方要求する。arpara の位置 INVALID により `head_position` が得られず、UI 再配置が行われない。
5. `imgui_context::end_frame()` は viewport の orientation をそのまま `XrCompositionLayerQuad.pose.orientation` にコピーする。
6. lobby は背景 Projection → GUI Quad → 光線等 Projection → recenter tip の順で追加する。通常の passthrough なし構成で `layers[1]` は最初の可視 GUI Quad。主 UI が表示されているなら主 UI の Quad である。
7. `scene::render_end()` は追加順を維持してポインタを構築し、`xr::session::end_frame()` → `xrEndFrame()` に渡す。この段階に pose の座標変換や Quaternion の再計算はない。

これはコード上で特定した経路であり、実機で修正前後の location flags を取得した結果ではない。実機では以下の検証を必ず行う。

WiVRn の修正後は、無効な `xrLocateViews` の結果で描画を進めず、レイヤーなしでフレームを終了する。そのため Monado の POSITION_VALID が直らないと、例外が消えてもロビー描画を待機し続ける可能性がある。WiVRn だけで無効な位置を有効と決めつける変更は行っていない。

## 必要な最小修正

隣接ファイル `monado-arpara-3dof.patch` にレビュー可能なパッチを用意した。対象は次の 2 ファイルのみ。

- `src/xrt/drivers/arpara/arpara_hmd.cpp`
- `tests/tests_arpara.cpp`

`get_pose()` の常時有効フラグを次の組合せにする。

```cpp
out->relation_flags = (enum xrt_space_relation_flags)(
    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
    XRT_SPACE_RELATION_POSITION_VALID_BIT);
```

固定位置は既存のゼロ初期化による `(0, 0, 0)` を維持する。`POSITION_TRACKED_BIT` は設定しない。`supported.position_tracking` は false を維持する。IMU orientation、鮮度判定、切断処理、`ORIENTATION_TRACKED_BIT` の条件は変更しない。全ての Quaternion を identity に書き換えない。

既存テストに、IMU 更新時、fixed モード、起動応答タイムアウト時の位置 VALID / 未追跡の確認を追加する。必要であれば、切断後も固定位置が VALID / 未追跡である確認と、view pose 呼出しの位置・orientation の確認も追加する。

## 適用と検証

作業開始時に現在の差分と履歴を再確認し、既存の作業を保護する。

```sh
cd /home/reina/monado
git status --short
git diff
git log -5 --oneline
git apply --check /home/reina/WiVRn/docs/monado-arpara-3dof.patch
git apply /home/reina/WiVRn/docs/monado-arpara-3dof.patch
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
git diff --check
```

既存の CMake 設定を使用する。`git reset --hard`、`git clean -fd` は禁止。新たな依存ライブラリやシステムパッケージの大量更新は不要。

テストが利用可能な場合、少なくとも `tests_arpara`、`tests_arpara_fixed` と既存 Vulkan format / DRM geometry / DMA-BUF / KMS fence のテストを実行する。テスト名は `ctest --test-dir build -N` で確認する。

更新したバイナリを使うため、現在の monado-service を通常手順で停止・再起動する。別バージョンのクライアントライブラリ／サービスを混在させない。稼働中プロセスを一律 kill せず、その環境の既存起動方法を確認する。

## 実機確認

Orange Pi 5 上で既存の DRM/KMS 起動方法を維持し、更新した Monado と WiVRn を使用する。

```sh
cd /home/reina/WiVRn
cmake --build build-client --parallel 4
XR_RUNTIME_JSON=/home/reina/monado/build/openxr_monado-dev.json \
  ./build-client/bin/wivrn
```

必要な診断では `xrLocateSpace(VIEW, STAGE)` と `xrLocateViews` の戻り値、VALID / TRACKED flags、pose の位置、orientation とそのノルムを確認する。期待値:

- VIEW orientation と position が VALID。
- position は未追跡の固定原点を元にした値。POSITION_TRACKED は不要。
- IMU が新鮮な場合に ORIENTATION_TRACKED が設定され、回転が反映される。
- Quaternion のノルムが約 1、ゼロ・NaN・Inf ではない。
- WiVRn に `Lobby: skipping rendering until view poses are valid` が継続しない。
- `XR_ERROR_POSE_INVALID` が発生せず、`xrEndFrame` が繰り返し成功する。
- HMD でロビーが見え、頭を回すと表示が追従する。60 秒以上の継続描画を確認する。

WiVRn の提出レイヤーを記録する場合は index と type を必ず記録する。passthrough や可視ウィンドウによって index は変わるので、index 1 を無条件に主 UI と仮定しない。

## 今回変更してはいけない経路

Monado HEAD の format 修正は、配列容量を 64 に拡張したうえで書き込み前に上限を確認し、IPC / OpenXR 側の count も検証している。assert を消しただけの修正ではない。今回の pose 修正で触る必要はない。

DRM/KMS、DMA-BUF、LINEAR scanout、3 画像、sync FD → IN_FENCE_FD、ownership / fence / 復元処理には変更を加えない。実機ログの次の項目を確認する。

- 最終 render extent / framebuffer は `3840x1920`。
- 左 viewport は `(0,0,1920,1920)`、右は `(1920,0,1920,1920)`。
- 片眼の OpenXR 推奨解像度 `1792x2015` と最終出力を混同しない。
- 左右の物理対応、暫定 90 度 FOV / レンズ間隔 / distortion なしの校正妥当性は、コードから成功と断定しない。
- swapchain memory size mismatch は再発した場合のみ別調査。サイズ検証を無効にしない。

## 報告すること

適用差分、ビルド・テスト結果、実機 location / view flags、`xrEndFrame` の成功と継続時間、実際に HMD で観測した表示と IMU 追従、未解決事項を報告する。実機を観測できなければ「ロビー表示成功」と断定しない。

## WiVRn 側で完了した修正・検証（2026-10-10）

変更したソースと目的:

| ファイル | 変更 |
| --- | --- |
| `client/application.cpp` | Vulkan 1.1 の `getFeatures2` で feature を問い合わせ。必須の multiview / samplerYcbcrConversion は未対応なら明確なエラー。shaderClipDistance は HTC ray shader の選択時だけ要求し、その場合に未対応なら明確なエラー。既存の timeline semaphore / uint8 index の任意判定は維持。 |
| `client/application.h` | xrLocateSpace の失敗、有効フラグ不足、不正 pose では位置・向きを返さない。 |
| `client/xr/pose.h` | Quaternion ノルム、有限成分、view 有効フラグを確認する共通処理。`-ffast-math` 下でも NaN / Inf を検出するため浮動小数点の exponent bits を検査。 |
| `client/render/imgui_impl.h` | viewport の初期 position をゼロ、初期 orientation を GLM の単位 Quaternion `{1,0,0,0}` に明示。 |
| `client/render/imgui_impl.cpp` | 不正 pose の UI Quad を提出から除外し、一度診断ログを出す。既存の有効な Quaternion を identity に上書きしない。 |
| `client/scenes/lobby.cpp` / `lobby.h` | 初回頭部取得前の UI 配置を初期化。初回 recenter と以後の有効な頭部姿勢を尊重。不正 view / 有効フラグ不足のフレームは空のレイヤーで終了し、状態変化時に診断する。 |
| `tests/client_pose.cpp` | viewport の初期 pose、ゼロ／非単位／NaN／Inf／overflow quaternion、不正位置、有効な未追跡 view、無効な片眼 view の回帰テスト。 |
| `CMakeLists.txt` / `client/CMakeLists.txt` | 既存の WIVRN_BUILD_TEST オプションで上記テストをビルド・CTest に登録。 |

検証結果:

- `cmake --build build-client --parallel 4`: 成功。x86_64 のクライアントを生成。
- `ctest --test-dir build-client --output-on-failure`: 1/1 成功。pose テストは `-O2 -ffast-math` でコンパイル。
- `git diff --check`: 成功。
- 全クライアント shader の SPIR-V を確認: `OpCapability ClipDistance` を含むのは `ray_htc.vert` と `ray_skinned_htc.vert` の 2 つだけ。通常 shader は要求しない。
- 起動確認: Monado サービスが未稼働のため `xrCreateInstance` で `XR_ERROR_RUNTIME_UNAVAILABLE`。例外を捕捉してプロセスは exit 0 だが、起動成功を示すものではない。
- `vulkaninfo`: 利用可能な ICD がなく `ERROR_INCOMPATIBLE_DRIVER`。PanVK 実機の feature / Vulkan device / xrEndFrame はこの環境では未検証。
- Monado ソースにはこちらから変更を加えていない。パッチの `git apply --check` のみ成功を確認。

指定の既存 `build-client` がこの環境には存在しなかったため、リポジトリの client プリセットを使用した。既存の server / dashboard ビルド設定は変更していない。システムパッケージは変更せず、KTX 4.4.2 の公式配布ツールを `/tmp/wivrn-ktx` に展開し、libktx と OpenXR loader は WiVRn の既存 FetchContent オプションでビルドした。ローカルの構成は次に相当する:

```sh
cmake --preset client \
  -DCMAKE_PROGRAM_PATH=/tmp/wivrn-ktx/KTX-Software-4.4.2-Linux-x86_64/bin \
  -DWIVRN_USE_SYSTEM_OPENXR=OFF -DWIVRN_USE_SYSTEM_LIBKTX=OFF \
  -DWIVRN_BUILD_TEST=ON
cmake --build build-client --parallel 4
ctest --test-dir build-client --output-on-failure
```

この x86_64 ツールパス／バイナリを Orange Pi に流用しない。Orange Pi の既存 CMake 設定でソースを差分ビルドする。ローカルログは `/tmp/wivrn-client-configure.log`、`/tmp/wivrn-client-build.log`、`/tmp/wivrn-client-run.log`、`/tmp/wivrn-vulkaninfo.log`。

未解決なのは Monado パッチの適用・ビルド・回帰テストと、実機でのフレーム提出、ロビー表示、IMU 追従、物理左右配置と光学校正の確認。WiVRn Server 接続やストリーミング検証は未実施。
