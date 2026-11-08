# KV260 ボード実装

FPGA アプリは **`fades`**、Linux デバイスは **`/dev/fades`**、
Linux モジュールは **`fades.ko`**。既定構成は6コア / 約115.385 MHz、DMA / AXI-Liteは187.5 MHz。
`FADES_CORES` で1〜6コアを指定できる。

## ハードウェア

- KV260 revB / K26、`xck26-sfvc784-2LV-c`、SOM 1.4 公式プリセット。
- AXI DMA Scatter-Gather、メモリ / ストリーム 128 bit、burst 最大 64 beat、40-bit address。
  長さフィールド 23 bit。ホストの 1 packet は最大 1 MiB。
- PS HP0 は MM2S + SG descriptor、HP1 は S2MM。非コヒーレントなので DT に
  `dma-coherent` は指定しない。
- DMA AXI-Lite は `0xa0000000`、solver AXI-Lite は `0xa0010000`。
- MM2S/S2MM IRQ は `pl_ps_irq0[0]/[1]`、GIC SPI 89/90、level high。
- ファンは **PS TTC0 wave[2] → EMIO → fan_en_b → A12**。
  LVCMOS33、drive 4 mA、slew SLOW。Linux の PWM 設定が実端子へ届く配線を含む。

## このディレクトリのファイル

|ファイル|用途 / 使われる場所|
|---|---|
|`CMakeLists.txt`|Vivado実行とfirmwareパッケージの定義|
|`scripts/build.tcl`|回路生成から実装・検査・firmware出力まで|
|`scripts/export.tcl`|`build.tcl`から呼ばれる。実装済み回路の検査とfirmware/XSA出力|
|`constraints/clocks.xdc.in` / `scripts/configuration.tcl.in`|CMakeの共通クロック設定から生成する制約とPS設定|
|`rtl/fades_kv260.sv`|IP Integratorとリポジトリ直下の`rtl/fades_axi.sv`を接続するラッパー|
|`overlay.dts.in`|LinuxにDMA・IRQ・クロック・制御レジスターを教えるDevice Treeのテンプレート|
|`shell.json`|`xmutil`がFPGAアプリを認識するための定義|

`constraints/` の `fan.xdc` はファン端子、`solver_cdc.xdc` はクロック境界の
タイミング制約。CDCの保持バス・固定設定・resetに対するレビュー根拠は
`scripts/export.tcl` 内の出力検査に記載する。

Vivado 2024.2のmodule-referenceはSystemVerilog型の入口を受け付けないため、
Verilog互換構文だけの`fades_kv260.sv`に限り、生成時の`FILE_TYPE`をVerilogに指定する。
下位の推論・探索・供給回路はSystemVerilogとして扱う。

## 開発機で生成

必要ツールはCMake 3.25以降、Ninja、Vivado 2024.2とK26デバイスサポート、`dtc`。
リポジトリルートで実行する。
Vivado 配下の XilinxBoardStore を参照するため、Vivado の環境を設定しておく。

```sh
cmake --preset kv260-pl -DFADES_CORES=6 -DFADES_VIVADO_JOBS=2
cmake --build --preset kv260-pl
```

`FADES_CORES` は既定6、対応範囲は1〜6。ボードトップ→AXI制御→コア群へ伝播する。
単一コアもこの実装経路を使う。ABIは6コア分のカウンター領域を保持し、未実装分は0。

通常のbuildで回路生成・検査・firmwareパッケージまで実行する。
クロック設定は `CMakeLists.txt` で一元管理し、PS設定・タイミング制約・DTBOに反映する。
動作周波数はcore 115,384,615 Hz / DMA 187,500,000 Hz。
制約周期は1 ps単位で切り下げた **core 8.666 ns / DMA 5.333 ns** を
トップレベル合成と配置配線の両方に適用する。XPMのCDC制約もこの周期から評価される。
BDはIPごとのOOC合成ではなく一括合成し、ソルバーにも合成時からクロック制約を適用する。

実装は `Performance_ExplorePostRoutePhysOpt` strategyで配置・物理最適化・配線の後、
`phys_opt_design -directive AggressiveExplore` を実行してからbitstreamを生成する。
最終結果のタイミング検査に失敗した場合はfirmwareパッケージを生成しない。
追加のTcl実行やDevice Treeの手編集は不要。
既定出力は `build/kv260-pl/fpga/kv260/`。configure時の `-B` で変更できる。
`VIVADO_EXECUTABLE` と `DTC_EXECUTABLE` でツールのパスを明示指定できる。
`FADES_VIVADO_JOBS` はVivado内部の並列数で、CMakeの `--parallel` とは独立。

|ターゲット|処理|
|---|---|
|`kv260-bd`|BD生成・検証のみ（`bd/`）|
|`kv260-synth`|合成まで（`synth/`）|
|`kv260-bitstream`|配置配線・bitstream生成・出力検査（`implementation/`）|
|`kv260-package`|必要ならbitstreamを生成し、DTBOと合わせてfirmwareを配置|

各段階は独立したプロジェクトを使い、ソース・制約・コア数の変更で再生成する。
出力検査はsetup/hold/pulse-width、CDC、bus skew、ファン端子を確認し、成功時のみ
`build_complete` を作成する。パッケージもこの印を要求する。

主要な生成物:

```text
build/kv260-pl/fpga/kv260/
  firmware/fades/       fades.bit.bin, fades.dtbo, shell.json
  implementation/
    timing_summary.rpt   setup / hold / pulse-width と未制約経路
    utilization.rpt      LUT / FF / CLB / BRAM / URAM
    cdc.rpt              レビュー済み CDC
    bus_skew.rpt         保持バス・FIFO pointer の skew
    io.rpt, drc.rpt       ファン端子と DRC
    verified_routed.dcp   検査済み checkpoint
    fades.xsa            ハードウェア定義
    project/             Vivado プロジェクトと詳細 runme.log
```

## ソースと firmware を実機へ転送

リポジトリのソースと `build/kv260-pl/fpga/kv260/firmware/fades/` の内容を、
ユーザーが任意の方法で実機へコピーする。ソースはGitで取得してもよい。
接続方法・転送先はユーザーが選び、ビルドシステムにはSSH接続・自動転送を含めない。

実機のUbuntuで必要なパッケージを入れる（初回）:

```sh
sudo apt install build-essential cmake ninja-build python3 "linux-headers-$(uname -r)"
```

実機側のリポジトリルートで実行する:

```sh
cmake --preset kv260-ps
cmake --build --preset kv260-ps
```

通常のbuildで `build/kv260-ps/host/driver/fades.ko` を生成する。
モジュールは実行中カーネルに対応したヘッダーでビルドする。Vivado・RTL検証ツールは不要。
ヘッダーを明示する場合はconfigure時に `-DFADES_KERNEL_BUILD=/path/to/headers` を指定する。

## ロードと試験

実機のrootシェルで、リポジトリルートから実行する。
使用中のプログラムとmmapを閉じ、`FIRMWARE_DIR` は実際にコピーしたfirmwareのディレクトリへ変更する。

```sh
FIRMWARE_DIR=/path/to/copied/firmware/fades
mkdir -p /lib/firmware/xilinx/fades
cp "$FIRMWARE_DIR"/* /lib/firmware/xilinx/fades/
if test -d /sys/module/fades; then rmmod fades; fi
xmutil unloadapp
xmutil loadapp fades
xmutil listapps
insmod build/kv260-ps/host/driver/fades.ko
ls -l /dev/fades
python3 host/tests/check_abi.py
python3 host/tests/check_stream.py
```

`xmutil` の終了値に加え、listapps、デバイスの存在、機能試験でロードを確認する。
デバイスが出ない場合や転送異常時は root で `dmesg` を確認する。
転送中断・タイムアウト時は、プロセスを終了してモジュールと FPGA アプリを再ロードする。
ABI v3 は STREAMON でスロット容量16,384問を設定して一度だけ初期化し、継続処理する。
出力は N 件または登録済みの処理がすべて完了した端数で区切る。
通常の DMA packet の TLAST でキューやコアをリセットしない。
STREAMOFF で両 DMA と全 solver/FIFO を reset してから再開始できる。
DMA MMIO・IRQ は FADES ドライバーが直接所有し、overlay に別 DMA provider は作らない。

測定結果には回路・クロック・転送サイズ・計測範囲を添えて保存する。

## 制御レジスター

|offset|意味|
|---|---|
|0x000|bit0 reset（初期値1）、bit1 enable|
|0x004|出力スロット容量 N。reset中のみ設定|
|0x008|投入登録件数を加算。送信待ちを含め、TX開始前に通知。読み出しは累積件数の下位32bit|
|0x00c|bit0 sticky framing error、bit1 snapshot pending|
|0x010|bit0=1書き込みでcounter snapshotを要求|
|0x014|identity `0x46414403`|
|0x018|投入登録累積件数の上位32bit|
|0x100〜0x25f|44個の64-bit counter、下位32bitが先|

counter順は `host/include/fades_ioctl.h` のコメントを参照。
クロックの異なるcounterはhandshakeで保持・採取し、全件排出後に件数を照合する。
