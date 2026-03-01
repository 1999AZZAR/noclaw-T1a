CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Wno-unused-parameter
INCLUDES = -Isrc
LDFLAGS = -lbearssl

# Optimization: small binary, fast code
RELEASE_FLAGS = -Os -DNDEBUG -flto -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables
RELEASE_LDFLAGS = -flto -Wl,--gc-sections

DEBUG_FLAGS = -g -O0 -DDEBUG

SRC_DIR = src
OBJ_DIR = obj

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

TARGET = noclaw

.PHONY: all clean debug release

all: release

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(OPT_FLAGS) $(INCLUDES) -c $< -o $@

debug: OPT_FLAGS = $(DEBUG_FLAGS)
debug: $(TARGET)

release: OPT_FLAGS = $(RELEASE_FLAGS)
release: LDFLAGS += $(RELEASE_LDFLAGS)
release: $(TARGET)
	@echo "Binary: $$(du -h $(TARGET) | cut -f1)"

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OPT_FLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)
