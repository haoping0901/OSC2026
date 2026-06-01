TOOLCHAIN ?= riscv64-unknown-elf
CC      = $(TOOLCHAIN)-gcc
LD      = $(TOOLCHAIN)-ld
OBJCOPY = $(TOOLCHAIN)-objcopy
MKIMAGE = mkimage

# Set platform-specific flags
# Default platform is qemu, can switch to opi with make PLATFORM=opi
PLATFORM ?= qemu

ifeq ($(PLATFORM), qemu)
    KERNEL_BASE = 0x80200000
    PLAT_CFLAGS = -DQEMU
else ifeq ($(PLATFORM), opi)
    KERNEL_BASE = 0x00200000
    PLAT_CFLAGS = 
endif

# Set paths
SRC_DIR   = src
BUILD_DIR = build/$(PLATFORM)
TARGET    = kernel

CFLAGS  = -march=rv64gc_zifencei_zicbom -mabi=lp64d -mcmodel=medany -ffreestanding -nostdlib -g -Wall -I include $(PLAT_CFLAGS)
LDFLAGS = -T $(SRC_DIR)/linker.ld -defsym KERNEL_BASE=$(KERNEL_BASE) --no-warn-rwx-segments

# Source files and corresponding object files
C_SRCS := $(wildcard $(SRC_DIR)/*.c)
S_SRCS := $(wildcard $(SRC_DIR)/*.S)
OBJS   := $(C_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o) \
          $(S_SRCS:$(SRC_DIR)/%.S=$(BUILD_DIR)/%.o)

.PHONY: all clean run qemu opi

all: $(TARGET).fit

# shortcut targets
qemu:
	@$(MAKE) PLATFORM=qemu all

opi:
	@$(MAKE) PLATFORM=opi all

# Compile rules
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	cp $@ kernel_$(PLATFORM).bin

$(TARGET).fit: $(TARGET).bin initramfs.cpio
	$(MKIMAGE) -f kernel.its $@

# Run the kernel in QEMU
QEMU = qemu-system-riscv64
QEMU_OPTS = -M virt -m 8G -serial stdio -vga none -device ramfb

rootfs/hello.txt:
	mkdir -p rootfs
	echo "Hello world from initramfs!" > rootfs/hello.txt

# Stage the pre-built user program into rootfs/ so it ends up inside
# the initramfs cpio archive. prog.bin is committed in the kernel/ tree.
rootfs/prog.bin: prog.bin
	mkdir -p rootfs
	cp prog.bin rootfs/prog.bin

# Lab5 Basic Ex2 fork_test binary, supplied by the course.
rootfs/osctest.bin: osctest.bin
	mkdir -p rootfs
	cp osctest.bin rootfs/osctest.bin

initramfs.cpio: rootfs/hello.txt rootfs/prog.bin rootfs/osctest.bin
	cd rootfs && find . | cpio -o -H newc > ../initramfs.cpio

test: qemu initramfs.cpio
	$(QEMU) $(QEMU_OPTS) -kernel $(TARGET).bin -initrd initramfs.cpio

clean:
	rm -rf build rootfs $(TARGET).elf $(TARGET)*.bin $(TARGET).fit *.cpio