# KV260 ボード実装

FPGA アプリは **`fades`**、Linux デバイスは **`/dev/fades`**、
Linux モジュールは **`fades.ko`**。現行構成は 6 コア / 100 MHz、DMA / AXI-Lite は 200 MHz。

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
|`build.tcl`|`make vivado-kv260` の入口。回路生成から実装・検査・firmware出力まで|
|`export.tcl`|`build.tcl`から呼ばれる。実装済み回路の検査とfirmware/XSA出力|
|`build_core.tcl`|`make vivado-core`。KV260上の単一コアOOC評価|
|`fades_kv260.sv`|IP Integratorと`rtl/solver_axi.sv`を接続する薄いラッパー|
|`overlay.dts`|LinuxにDMA・IRQ・クロック・制御レジスターを教えるDevice Tree|
|`shell.json`|`xmutil`がFPGAアプリを認識するための定義|

`constraints/` の `fan.xdc` はファン端子、`solver_cdc.xdc` はクロック境界の
タイミング制約、`core_ooc.xdc` は開発用の単一コア評価に使用する。
CDCの保持バス・固定設定・resetに対するレビュー根拠は`export.tcl`内の出力検査に記載する。

Vivado 2024.2のmodule-referenceはSystemVerilog型の入口を受け付けないため、
Verilog互換構文だけの`fades_kv260.sv`に限り、生成時の`FILE_TYPE`をVerilogに指定する。
下位の推論・探索・供給回路はSystemVerilogとして扱う。

## 開発機で生成

Vivado 2024.2 と K26 デバイスサポート、`dtc` を用意し、リポジトリルートで実行する。
Vivado 配下の XilinxBoardStore を参照するため、Vivado の環境を設定しておく。

```sh
make lint test stream-test
make fifo-test
make vivado-kv260 JOBS=2
make package-kv260
```

既定出力は `build/kv260/`。`KV260_OUT` で変更できる。
`KV260_STAGE=bd/synth/bit/export` を指定でき、既定は `bit`。
bd/synth/bit はプロジェクトを作り直し、export は既存の実装済みプロジェクトから再出力する。
export は setup/hold/pulse-width、CDC、bus skew、ファン端子を確認し、成功時のみ
`build_complete` を作成する。`package-kv260` はこの印を要求する。

主要な生成物:

```text
build/kv260/
  firmware/fades/       fades.bit.bin, fades.dtbo, shell.json
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

以下は SSH で root に接続できる実機を `BOARD` に指定する例。
実機作業ディレクトリは `/root/fades/solver/` とする。
初回は `/root/fades` を用意し、既存の内容を確認してから転送する。

```sh
BOARD=root@kv260
ssh "$BOARD" 'ls /root/fades'
ssh "$BOARD" 'mkdir -p /root/fades/solver/sim /root/fades/solver/firmware'
scp -r host reference "$BOARD":/root/fades/solver/
scp -r sim/data "$BOARD":/root/fades/solver/sim/
scp build/kv260/firmware/fades/* "$BOARD":/root/fades/solver/firmware/
ssh "$BOARD" 'make -C /root/fades/solver/host tools module'
```

モジュールは実行中カーネルに対応したヘッダーでビルドする。
ヘッダーの場所や出力先の指定は [PS ソフトウェア](../../host/README.md) を参照。

## ロードと試験

実機の root シェルで、使用中のプログラムと mmap を閉じてから実行する。

```sh
mkdir -p /lib/firmware/xilinx/fades
cp /root/fades/solver/firmware/* /lib/firmware/xilinx/fades/
if test -d /sys/module/fades; then rmmod fades; fi
xmutil unloadapp
xmutil loadapp fades
xmutil listapps
insmod /root/fades/solver/build/host/driver/fades.ko
ls -l /dev/fades
python3 /root/fades/solver/host/tests/check_abi.py
/root/fades/solver/build/host/fades_verify /root/fades/solver/sim/data/17puz49158.txt 1 1 1
/root/fades/solver/build/host/fades_verify /root/fades/solver/sim/data/17puz49158.txt 1024 3 1 mixed
/root/fades/solver/build/host/fades_verify /root/fades/solver/sim/data/17puz49158.txt 8193 6 1
/root/fades/solver/build/host/fades_verify /root/fades/solver/sim/data/HardestDatabase110626.txt 375 1 1
/root/fades/solver/build/host/fades_benchmark /root/fades/solver/sim/data/17puz49158.txt 10
```

`xmutil` の終了値に加え、listapps、デバイスの存在、機能試験でロードを確認する。
デバイスが出ない場合や転送異常時は root で `dmesg` を確認する。
転送中断・タイムアウト時は、プロセスを終了してモジュールと FPGA アプリを再ロードする。
通常の DMA packet の TLAST ではキューやコアをリセットしないが、ホストの RUN は
全 round を排出し、次の round の開始時に全体 reset を実施する。

測定結果には回路・クロック・転送サイズ・計測範囲を添えて保存する。

## 制御レジスター

|offset|意味|
|---|---|
|0x000|bit0 reset（初期値1）、bit1 enable|
|0x004 / 0x008|総レコード件数 / 出力packet件数。reset中に設定|
|0x00c|bit0 sticky framing error、bit1 snapshot pending|
|0x010|bit0=1書き込みでcounter snapshotを要求|
|0x014|identity `0x46414401`|
|0x100〜0x25f|44個の64-bit counter、下位32bitが先|

counter順は `host/include/fades_ioctl.h` のコメントを参照。
クロックの異なるcounterはhandshakeで保持・採取し、全件排出後に件数を照合する。
