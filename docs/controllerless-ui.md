# Linux ロビーのコントローラーレス操作

Linux クライアントのロビーで、HMD の正面方向（Head Gaze）と USB キーボードの Space を使って既存 UI を操作する。Eye Tracking、PC へのキー転送、SteamVR コントローラーのエミュレーションは使用しない。

## 操作

- コントローラー／ハンドが UI を指していない場合、頭部の向きで UI 上の円形カーソルを移動する。
- Space の押下が左 Mouse Down、解放が Mouse Up。押し続けて頭を動かすとスライダーやスクロールバーをドラッグできる。リピートは新しいクリックにしない。
- コントローラー／ハンドのポインターを優先する。VR 側でドラッグしていない場合は Space で Head Gaze に切り替えられる。Head Gaze のドラッグ中に VR 側のボタンを押しても別のドラッグを開始しない。
- パネル外、姿勢無効、フォーカス喪失、画面／モーダル切替、キーボード切断／イベント欠落時はローカル入力を解除する。復帰時に Space を押したままなら、一度解放して押し直す。
- PIN は上段／テンキーの数字と Backspace でも入力できる。入力順序、6 桁制限と既存の自動送信を維持し、PIN をログに出さない。
- テキスト欄の編集中は US 配列の ASCII 文字を直接入力できる。Shift、Backspace、Delete、Enter、矢印、Home / End に対応する。Space は空白になり、クリックには使用しない。Enter または Escape で編集を終了すると、Space をクリックに戻せる。
- 非 US 配列の変換、IME、Unicode 入力、Caps Lock は未対応。既存の仮想キーボードは残している。

## 起動と入力デバイス

通常の起動で Linux ロビーの Head Gaze が有効になる。

```sh
cd ~/WiVRn
cmake --build build-client --parallel 4
XR_RUNTIME_JSON=/home/reina/monado/build/openxr_monado-dev.json \
  ./build-client/bin/wivrn
```

`/dev/input/event*` のうち Space と Enter を持つキーボードを自動検出し、2 秒間隔で再検出する。指定する場合は、実際のキーボードの event ノードまたは `by-id` の symlink を使う。

```sh
WIVRN_UI_KEYBOARD=/dev/input/by-id/実際のキーボード-event-kbd \
XR_RUNTIME_JSON=/home/reina/monado/build/openxr_monado-dev.json \
  ./build-client/bin/wivrn
```

`WIVRN_HEAD_GAZE=0` で Head Gaze とローカルキーボードバックエンドを無効にできる。設定画面での切替は今回追加していない。

キーボードの読み取りはロビーが OpenXR の `FOCUSED` 状態にある間だけ行う。読み取りは非ブロッキングかつ非排他的で、デバイスを grab しない。ストリーミングへのシーン切替時には fd を閉じる。デスクトップや SDL のウィンドウは不要。

通常ユーザーにキーボード event ノードの読み取り権限が必要。権限不足なら `Local UI keyboard cannot read ...` が出る。WiVRn は権限を変更しない。既存の seat / udev 設定を使用し、必要な場合に管理者が対象キーボードだけの ACL を設定する。以下は対象を確認してから実施する例であり、今回の実装作業では実行していない。

```sh
# eventN を対象 USB キーボードの event ノードに置き換える。
# 再接続後はノード／ACL が変わる場合がある。
sudo setfacl -m u:reina:r /dev/input/eventN
```

デスクトップ併用時には他アプリケーションにもキーボードイベントが届く。ローカル UI の入力判定は OpenXR フォーカスに従う。

## 実装

| ファイル | 内容 |
| --- | --- |
| `client/linux/local_keyboard.{h,cpp}` | evdev の検出・読み取り、複数キーボードの押下状態、リピート、切断、SYN_DROPPED の復帰、開始時に押されているキーの抑止。 |
| `client/linux/keyboard_ui.{h,cpp}` | ImGui のキー／ASCII 文字イベントと、順序を維持する PIN 用キーの変換。 |
| `client/render/head_gaze_input.h` | 1 回の Space 押下につき 1 回のドラッグを許可する状態管理。 |
| `client/render/pointer_intersection.h` | 既存のパネルとの交差計算を共通化。Head Gaze と VR 入力で使用し、ハンドの接触距離の許容値を維持。 |
| `client/render/imgui_impl.{h,cpp}` | 頭部 pose の取得、既存カーソルへの追加、ポインターの選択、ImGui の mouse イベント、短い Space タップの Down / Up の分離、無効状態の解除。 |
| `client/scenes/lobby.{h,cpp}` | Linux ロビーでの有効化、未描画／無効 view／OpenXR フォーカス喪失時の解除。 |
| `client/scenes/lobby_gui.cpp` | タブ切替時の解除、順序を保った数字／Backspace の PIN 入力。既存ウィジェットは変更せず使用。 |
| `client/CMakeLists.txt`, `tests/client_gaze.cpp` | GPU 不要の入力・交差計算・Dear ImGui ウィジェットの回帰テスト。 |

`gui_common.cpp` と `lobby_keyboard.cpp` も調査したが、個別ウィジェットや仮想キーボードの書換えは不要だった。既存の Vulkan/OpenXR pose 検証を使用し、無効な HMD pose は使用しない。新しい外部依存ライブラリ、Monado の変更、トラッキング／ネットワーク／ストリーミングの入力転送は追加していない。

Head Gaze の pose を取得する部分とパネルへのポインター変換は分けてあり、将来 Eye Gaze の pose へ切り替えても UI ウィジェットの個別改修は不要。

参考: [Linux input event codes](https://cdn.kernel.org/doc/html/latest/input/event-codes.html)、[Dear ImGui 入力 API](https://github.com/ocornut/imgui/blob/master/imgui.h)。使用している ImGui はリポジトリ指定の 1.92.9。

## 検証と実機で残る確認

ローカル x86_64 環境で実施:

```sh
cmake --build build-client --parallel 4
ctest --test-dir build-client --output-on-failure
git diff --check
```

ビルド成功。CTest は pose と gaze の 2/2 成功。gaze テストは実際の Dear ImGui のボタン、チェックボックス、タブ、スライダー、InputText に入力を送り、単発クリック、Space 保持、パネル外解除、文字／空白／Backspace／Enter を確認する。交差計算と押下状態は本番の共通実装を使用する。既存の pose テストも成功している。

この環境では Orange Pi / arpara / USB キーボード / 使用可能な Vulkan ICD がなく、実機の `xrEndFrame`、ロビー表示、Head Gaze の動き、evdev の USB 読み取り、実サーバー接続は未検証。ソフトウェアの回帰テストを HMD 上の表示成功とは扱わない。

実機では次を順に確認する:

1. VR コントローラーなしでロビーが表示され、キーボード検出ログが出る。
2. 頭を動かすとカーソルが動き、パネル外ではクリックできない。
3. Space で接続先を選択し、PIN を画面ボタンまたは物理数字キーで入力して接続できる。
4. 設定タブ、チェックボックス、スライダー、スクロールバーを操作できる。Space 保持でボタンが繰り返しクリックされない。
5. アドレス欄の Space が空白となり、Enter / Escape で編集を終えるとクリックに戻る。
6. Space 保持中にフォーカス／追跡を失わせ、復帰後は解放・押し直しで正常に戻る。キーボードを抜き差しした場合も確認する。
7. コントローラー／ハンドを使う既存操作と、サーバー接続後の通常ストリーミングに回帰がない。
