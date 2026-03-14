TOOLCHAIN ?= riscv64-unknown-elf
CC = $(TOOLCHAIN)-gcc
LD = $(TOOLCHAIN)-ld
OBJCOPY = $(TOOLCHAIN)-objcopy
CFLAGS = -mcmodel=medany -ffreestanding -nostdlib -g -Wall
CFLAGS += -I include
QEMU = qemu-system-riscv64
TARGET = kernel
KERNEL_BASE ?= 0x00200000

all: clean
	$(CC) $(CFLAGS) -c src/*.S src/*.c
	# $(LD) -T src/linker.ld -o $(TARGET).elf *.o
	$(LD) -T src/linker.ld -defsym KERNEL_BASE=$(KERNEL_BASE) -o $(TARGET).elf *.o
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin
	mkimage -f kernel.its kernel.fit

test: CFLAGS += -DQEMU
test: KERNEL_BASE = 0x80200000
test: all $(TARGET).bin
	$(QEMU) -M virt -m 8G -kernel $(TARGET).bin -display none -serial stdio

clean:
	rm -f $(TARGET) $(TARGET).elf *.o *.bin kernel *.fit
