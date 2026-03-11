# Lab1

## FAQ

### 要如何確認 QEMU 的 UART base address 以及 register layout?

1. 確認 QEMU 啟動時使用的 device tree。

```bash
qemu-system-riscv64 -machine virt,dumpdtb=virt.dtb
```

2. 將取得的 dtb 檔轉為人類可讀的格式後，確認 serial node 的 `compatible` 屬性內容，以確定 QEMU 模擬的 UART 行為與哪個晶片相容，再去確認該晶片的 datasheet 來確認 register layout。

```bash
dtc -I dtb -O dts virt.dtb > qemu_dtb.txt
```

3. 根據 `compatible` 屬性可知，QEMU UART driver 與 ns16550a 晶片相容，因此可透過 ns16550a 的 datasheet 來確認 register layout。

```C
// qemu_dtb.txt
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
