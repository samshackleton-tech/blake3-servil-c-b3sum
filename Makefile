# Reproducible local example build. The C API is built by cargo-c.
SERVIL_DIR := $(CURDIR)/vendor/blake3-servil
PREFIX := $(CURDIR)/build/servil
TARGET := $(CURDIR)/build/cargo
BIN := $(CURDIR)/build/b3sum-c
STATIC_BIN := $(CURDIR)/build/b3sum-c-static
PKG := PKG_CONFIG_PATH=$(PREFIX)/lib/pkgconfig
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic

.PHONY: all servil test clean benchmark
all: $(BIN) $(STATIC_BIN)

servil:
	git submodule update --init --recursive
	cargo cinstall --release --manifest-path $(SERVIL_DIR)/capi/Cargo.toml --target-dir $(TARGET) --prefix $(PREFIX) --libdir lib

$(BIN): src/b3sum.c | servil
	$(CC) $(CFLAGS) $< -o $@ $(shell $(PKG) pkg-config --cflags --libs b3_servil) -pthread -Wl,-rpath,'$$ORIGIN/servil/lib'

$(STATIC_BIN): src/b3sum.c | servil
	$(CC) $(CFLAGS) $< -o $@ $(shell $(PKG) pkg-config --static --cflags --libs b3_servil) -pthread

test: all
	python3 tests/test_b3sum.py --bin $(BIN) --static-bin $(STATIC_BIN)

benchmark: all
	python3 benchmark/benchmark.py --bin $(BIN) --bench-dir $(BENCH_HASHES_DIR) --quick --rounds 4

clean:
	rm -rf build
