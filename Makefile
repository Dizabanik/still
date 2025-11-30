# Compiler and Linker
CC      = clang
LD      = $(CC)

# -pedantic \
# -Wextra \
# -Wformat-security \
# -Wfloat-equal \
# -Wshadow \
# -Wconversion \
# -Wjump-misses-init \
# -Wlogical-not-parentheses \
# -Wnull-dereference \
# -Wvla \


# Flags
CFLAGS  = -g -O3 -Wall -Wno-unused-parameter -std=c11 -D_POSIX_C_SOURCE=200809L \
          -I include/ $(shell llvm-config --cflags)

LDFLAGS = $(shell llvm-config --ldflags --link-shared --libs --system-libs \
            core executionengine analysis native passes coroutines bitwriter)

# Directories
SRC_DIR = src
BUILD_DIR = build

# Files
SRCS  = $(wildcard $(SRC_DIR)/*.c)
OBJS  = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
TARGET = kawac

# Pretty Colors
GREEN  := \033[1;32m
BLUE   := \033[1;34m
YELLOW := \033[1;33m
RESET  := \033[0m

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "$(BLUE)[Linking]$(RESET) $@"
	@$(LD) $(OBJS) -o $@ $(LDFLAGS)
	@echo "$(GREEN)[Done]$(RESET) Built $@"

# Build object files in build/
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "$(YELLOW)[Compiling]$(RESET) $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Create build directory if missing
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@echo "$(YELLOW)[Cleaning]$(RESET)"
	@rm -rf $(BUILD_DIR) $(TARGET) *.ll *.bc
	@echo "$(GREEN)[Clean complete]$(RESET)"

.PHONY: all clean
