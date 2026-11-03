# KV260 ボード実装

FPGA アプリは **`fades`**、Linux デバイスは **`/dev/fades`**、
Linux モジュールは **`fades.ko`**。既定構成は6コア / 100 MHz、DMA / AXI-Liteは200 MHz。
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
|`rtl/fades_kv260.sv`|IP Integratorとリポジトリ直下の`rtl/fades_axi.sv`を接続するラッパー|
|`overlay.dts`|LinuxにDMA・IRQ・クロック・制御レジスターを教えるDevice Tree|
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
cmake --preset fpga-kv260 -DFADES_CORES=6 -DFADES_VIVADO_JOBS=2
cmake --build --preset fpga-kv260 --target kv260-package
```

`FADES_CORES` は既定6、対応範囲は1〜6。ボードトップ→AXI制御→コア群へ伝播する。
単一コアもこの実装経路を使う。ABIは6コア分のカウンター領域を保持し、未実装分は0。

既定出力は `build/fpga-kv260/fpga/kv260/`。configure時の `-B` で変更できる。
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
build/fpga-kv260/fpga/kv260/
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

SSHエイリアス `kria` でrootに接続する。実機作業ディレクトリは `/root/fades/cmake/` とする。
初回は `/root/fades` を用意し、既存の内容を確認してから転送する。

```sh
ssh kria 'ls /root/fades'
ssh kria 'mkdir -p /root/fades/cmake/sim /root/fades/cmake/firmware'
scp CMakeLists.txt CMakePresets.json kria:/root/fades/cmake/
scp -r host reference kria:/root/fades/cmake/
scp -r sim/data kria:/root/fades/cmake/sim/
scp build/fpga-kv260/fpga/kv260/firmware/fades/* kria:/root/fades/cmake/firmware/
ssh kria 'cmake --preset host-kv260 -S /root/fades/cmake'
ssh kria 'cmake --build /root/fades/cmake/build/host-kv260 --parallel 4'
ssh kria 'cmake --build /root/fades/cmake/build/host-kv260 --target host-module'
ssh kria 'ctest --test-dir /root/fades/cmake/build/host-kv260 --output-on-failure'
```

モジュールは実行中カーネルに対応したヘッダーでビルドする。
実機には `build-essential cmake ninja-build python3` と対応するカーネルヘッダーが必要。
ヘッダーの場所や出力先の指定は [PS ソフトウェア](../../host/README.md) を参照。

## ロードと試験

実機の root シェルで、使用中のプログラムと mmap を閉じてから実行する。

```sh
mkdir -p /lib/firmware/xilinx/fades
cp /root/fades/cmake/firmware/* /lib/firmware/xilinx/fades/
if test -d /sys/module/fades; then rmmod fades; fi
xmutil unloadapp
xmutil loadapp fades
xmutil listapps
insmod /root/fades/cmake/build/host-kv260/host/driver/fades.ko
ls -l /dev/fades
python3 /root/fades/cmake/host/tests/check_abi.py
/root/fades/cmake/build/host-kv260/host/fades_verify /root/fades/cmake/sim/data/17puz49158.txt 1 1 1
/root/fades/cmake/build/host-kv260/host/fades_verify /root/fades/cmake/sim/data/17puz49158.txt 1024 3 1 mixed
/root/fades/cmake/build/host-kv260/host/fades_verify /root/fades/cmake/sim/data/17puz49158.txt 8193 6 1
/root/fades/cmake/build/host-kv260/host/fades_verify /root/fades/cmake/sim/data/HardestDatabase110626.txt 375 1 1
/root/fades/cmake/build/host-kv260/host/fades_benchmark /root/fades/cmake/sim/data/17puz49158.txt 10
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
