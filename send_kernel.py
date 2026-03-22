#!/usr/bin/env python3
"""
Send a kernel image over UART using the BOOT protocol.
Usage: python3 send_kernel.py <serial_device> <kernel.bin>
Example: python3 send_kernel.py /dev/pts/3 kernel_uart.bin
"""
import struct
import sys

BOOT_MAGIC = 0x544F4F42  # "BOOT" in hex

def main():
    # check arguments
    if len(sys.argv) != 3:
        print("Usage: send_kernel.py <serial_device> <kernel.bin>", file=sys.stderr)
        sys.exit(1)
    dev = sys.argv[1]
    path = sys.argv[2]

    # read kernel image
    with open(path, "rb") as f:
        kernel_data = f.read()
    size = len(kernel_data)
    header = struct.pack("<II", BOOT_MAGIC, size)

    # send kernel image
    with open(dev, "wb", buffering=0) as tty:
        tty.write(header)
        tty.write(kernel_data)
    print(f"Sent {size} bytes (header + kernel) to {dev}")

if __name__ == "__main__":
    main()
