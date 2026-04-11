# OSC 2026

## Environment Setup

### Cross Compiler

```bash
sudo apt update && sudo apt install gcc-riscv64-unknown-elf
```

### QEMU

```bash
sudo apt update && sudo apt install qemu-system-riscv64 opensbi u-boot-qemu
```

### Bootloader Tools

```bash
sudo apt update && sudo apt install u-boot-tools
```

## VSCode Extension

推薦使用 TraceNotes 擴充套件紀錄程式邏輯。

## FAQ

### Lab1: 要如何確認 QEMU 的 UART base address 以及 register layout?

1. 確認 QEMU 啟動時使用的 device tree。

```bash
qemu-system-riscv64 -machine virt,dumpdtb=virt.dtb -m 8G
```

2. 將取得的 dtb 檔轉為人類可讀的格式後，確認 serial node 的 `compatible` 屬性內容，以確定 QEMU 模擬的 UART 行為與哪個晶片相容，再去確認該晶片的 datasheet 來確認 register layout。

```bash
dtc -I dtb -O dts -o virt.dts virt.dtb
```

3. 根據 `compatible` 屬性可知，QEMU UART driver 與 ns16550a 晶片相容，因此可透過 ns16550a 的 datasheet 來確認 register layout。

```C
// virt.dts
...
		serial@10000000 {
			interrupts = <0x0a>;
			interrupt-parent = <0x03>;
			clock-frequency = "\08@";
			reg = <0x00 0x10000000 0x00 0x100>;
			compatible = "ns16550a";
		};
...
```

### Lab2: 哪裡規定了 `a1` register 必須放 dtb address 的？

OpenSBI [官方 github](https://github.com/riscv-software-src/opensbi/blob/master/docs/firmware/fw.md) 定義了在 RISC-V 架構中，前一個啟動階段會透過 `a1` 暫存器將 DTB 位址傳給 OpenSBI。
