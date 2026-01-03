# SCHED_EXT Process Tree Demo
# Makefile

# Directories
SRC_DIR := src
BUILD_DIR := build

# Compiler settings
CC := gcc
CLANG := clang
CFLAGS := -O2 -Wall
BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_x86

# Libraries
LDFLAGS_BPF := -lbpf -lelf -lz
LDFLAGS_PTHREAD := -pthread

# Source files
BPF_SRC := $(SRC_DIR)/scx_scheduler.bpf.c
LOADER_SRC := $(SRC_DIR)/scx_loader.c
RUNNER_SRC := $(SRC_DIR)/scx_run.c
TREE_SRC := $(SRC_DIR)/process_tree.c

# Build outputs
VMLINUX_H := $(BUILD_DIR)/vmlinux.h
BPF_OBJ := $(BUILD_DIR)/scx_scheduler.bpf.o
LOADER_BIN := $(BUILD_DIR)/scx_loader
RUNNER_BIN := $(BUILD_DIR)/scx_run
TREE_BIN := $(BUILD_DIR)/process_tree

# Default target
all: $(LOADER_BIN) $(RUNNER_BIN) $(TREE_BIN)

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Generate vmlinux.h from kernel BTF
$(VMLINUX_H): | $(BUILD_DIR)
	@echo "Generating vmlinux.h from kernel BTF..."
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@

# Compile BPF scheduler
$(BPF_OBJ): $(BPF_SRC) $(VMLINUX_H) | $(BUILD_DIR)
	@echo "Compiling BPF scheduler..."
	$(CLANG) $(BPF_CFLAGS) -I$(BUILD_DIR) -I/usr/include/bpf -c $< -o $@

# Compile userspace scheduler loader (with pthread for dumper thread)
$(LOADER_BIN): $(LOADER_SRC) $(BPF_OBJ) | $(BUILD_DIR)
	@echo "Compiling scheduler loader..."
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS_BPF) $(LDFLAGS_PTHREAD)

# Compile scx_run launcher
$(RUNNER_BIN): $(RUNNER_SRC) | $(BUILD_DIR)
	@echo "Compiling scx_run launcher..."
	$(CC) $(CFLAGS) $< -o $@

# Compile process_tree demo
$(TREE_BIN): $(TREE_SRC) | $(BUILD_DIR)
	@echo "Compiling process_tree..."
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS_PTHREAD)

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Install (copy BPF object to build dir for loader to find)
install: all
	@echo "Build complete. Files in $(BUILD_DIR)/"
	@echo ""
	@echo "Usage:"
	@echo "  1. Load scheduler:  sudo $(LOADER_BIN)"
	@echo "  2. Run with EXT:    $(RUNNER_BIN) $(TREE_BIN)"
	@echo "  3. Run without EXT: $(TREE_BIN)"

# Help
help:
	@echo "SCHED_EXT Process Tree Demo"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build everything (default)"
	@echo "  clean    - Remove build directory"
	@echo "  install  - Build and show usage"
	@echo "  help     - Show this help"
	@echo ""
	@echo "Build outputs in $(BUILD_DIR)/:"
	@echo "  scx_scheduler.bpf.o - BPF scheduler (the scheduling logic)"
	@echo "  scx_loader          - Loads BPF scheduler into kernel (run as root)"
	@echo "  scx_run             - Launches programs with SCHED_EXT policy"
	@echo "  process_tree        - Demo animation program"

.PHONY: all clean install help
