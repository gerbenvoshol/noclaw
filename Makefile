# noclaw — The absolute smallest AI assistant. Pure C.
# Target: <100KB binary, <500KB RAM, <1ms startup.

CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wno-unused-parameter
LDFLAGS :=

BEARSSL_DIR := vendor/BearSSL
BEARSSL_INC := $(BEARSSL_DIR)/inc
BEARSSL_LIB := $(BEARSSL_DIR)/build/libbearssl.a

# Source files
SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)

# Output
BIN := noclaw

# ── Platform detection ───────────────────────────────────────────

UNAME := $(shell uname -s)

ifeq ($(UNAME),Darwin)
  # macOS: SecureTransport for TLS (system framework, no extra deps)
  LDFLAGS += -framework Security -framework CoreFoundation
else
  # Linux: build against vendored BearSSL
  CFLAGS += -I$(BEARSSL_INC)
  LDFLAGS += $(BEARSSL_LIB)
endif

# ── Build modes ──────────────────────────────────────────────────

.PHONY: all release debug clean test install uninstall bearssl

all: release

release: CFLAGS  += -Os -DNDEBUG -flto -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables
ifeq ($(UNAME),Darwin)
release: LDFLAGS += -flto -Wl,-dead_strip
else
release: LDFLAGS += -flto -Wl,--gc-sections
endif
release: $(BIN)
	@ls -lh $(BIN) | awk '{print "Binary: " $$5}'

debug: CFLAGS += -O0 -g -DDEBUG -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: $(BIN)

# ── Link ─────────────────────────────────────────────────────────

$(BIN): $(OBJS) $(if $(filter Darwin,$(UNAME)),,$(BEARSSL_LIB))
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(BEARSSL_LIB):
	$(MAKE) -C $(BEARSSL_DIR) lib

# ── Compile ──────────────────────────────────────────────────────

src/%.o: src/%.c src/nc.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Test ─────────────────────────────────────────────────────────

test: CFLAGS += -O0 -g -DDEBUG -DNC_TEST
test: $(if $(filter Darwin,$(UNAME)),,$(BEARSSL_LIB))
	$(CC) $(CFLAGS) -o noclaw_test src/*.c -DNC_TEST_MAIN $(LDFLAGS)
	./noclaw_test
	@rm -f noclaw_test

# ── Musl static build (Linux only) ───────────────────────────

.PHONY: musl
musl: CC := musl-gcc
musl: CFLAGS  += -Os -DNDEBUG -flto -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -I$(BEARSSL_INC)
musl: LDFLAGS := -static $(BEARSSL_LIB) -lm -flto -Wl,--gc-sections
musl: $(BEARSSL_LIB) $(BIN)
	@strip -s $(BIN)
	@ls -lh $(BIN) | awk '{print "Binary: " $$5 " (static, stripped)"}'

# ── Install ──────────────────────────────────────────────────────

PREFIX ?= /usr/local

install: release
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(PREFIX)/bin/$(BIN)

# ── Clean ────────────────────────────────────────────────────────

clean:
	rm -f src/*.o $(BIN) noclaw_test

# ── Size report ──────────────────────────────────────────────────

.PHONY: size
size: release
	@echo "--- Binary size ---"
	@ls -lh $(BIN)
	@echo "--- Section sizes ---"
	@size $(BIN) 2>/dev/null || true
