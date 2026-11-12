# Miner game — build with `make` (release) or `make DEBUG=1` (debug logs compiled in).
# Debug and release builds use separate output dirs so objects never mix.

CC      ?= gcc
CFLAGS  := -Wall -Wextra -std=gnu11 -Iinclude -pthread
LDFLAGS := -pthread

ifeq ($(DEBUG),1)
  BUILD  := build-debug
  CFLAGS += -O0 -g -DMINER_DEBUG
else
  BUILD  := build
  CFLAGS += -O2 -g
endif

LIB_SRC    := $(wildcard lib/*.c)
SERVER_SRC := $(wildcard server/*.c)
CLIENT_SRC := $(wildcard client/*.c)

LIB_OBJ    := $(LIB_SRC:%.c=$(BUILD)/%.o)
SERVER_OBJ := $(SERVER_SRC:%.c=$(BUILD)/%.o)
CLIENT_OBJ := $(CLIENT_SRC:%.c=$(BUILD)/%.o)

LIB    := $(BUILD)/libminer.so
SERVER := $(BUILD)/miner_server
CLIENT := $(BUILD)/miner_client

# Both executables link the shared library; rpath=$ORIGIN finds it next to them.
LINK_LIB := -L$(BUILD) -lminer -Wl,-rpath,'$$ORIGIN'

.PHONY: all clean
all: $(LIB) $(if $(SERVER_SRC),$(SERVER)) $(if $(CLIENT_SRC),$(CLIENT))

$(LIB): $(LIB_OBJ)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

$(SERVER): $(SERVER_OBJ) $(LIB)
	$(CC) -o $@ $(SERVER_OBJ) $(LINK_LIB) $(LDFLAGS)

$(CLIENT): $(CLIENT_OBJ) $(LIB)
	$(CC) -o $@ $(CLIENT_OBJ) $(LINK_LIB) $(LDFLAGS)

# Library objects need -fPIC; all objects depend on headers via -MMD.
$(BUILD)/lib/%.o: lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -rf build build-debug

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
