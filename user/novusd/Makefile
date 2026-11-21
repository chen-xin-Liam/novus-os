# NovusDefender - reproducible build entry point.
# Run from WSL:  make / make test / make lint / make test-e2e
SHELL := /bin/bash

ROOT := $(CURDIR)
export ROOT

CARGO_HOME ?= /home/dev/.cargo
RUSTUP_HOME ?= /home/dev/.rustup
export CARGO_HOME RUSTUP_HOME
export PATH := $(CARGO_HOME)/bin:$(PATH)

CARGO := $(CARGO_HOME)/bin/cargo
CC ?= gcc
AR ?= ar
CLANG ?= clang
CFLAGS := -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
          -Wall -Wextra -Werror -I$(ROOT)/c/core/include -I$(ROOT)/c/core/src

.PHONY: all build c-lib rust-build link test test-c test-rust test-e2e lint \
        clean check-static check-core-purity size-check novus-app docs

all: build

build: c-lib rust-build link

c-lib:
	@bash $(ROOT)/scripts/env.sh check
	@mkdir -p $(ROOT)/build/c
	@for f in $(ROOT)/c/core/src/*.c; do \
	  $(CC) $(CFLAGS) -c "$$f" -o "$(ROOT)/build/c/$$(basename $$f .c).o" || exit 1; \
	done
	$(AR) rcs $(ROOT)/build/c/libndcore.a $(ROOT)/build/c/*.o

rust-build:
	RUSTFLAGS="-C target-feature=+crt-static" $(CARGO) build --release -p nd-cli

link: rust-build
	@mkdir -p $(ROOT)/out
	cp $(ROOT)/target/x86_64-unknown-linux-gnu/release/nd-cli $(ROOT)/out/novusd 2>/dev/null || \
	  cp $(ROOT)/target/release/nd-cli $(ROOT)/out/novusd
	  @bash $(ROOT)/scripts/check-static.sh $(ROOT)/out/novusd

test: test-c test-rust

test-c: c-lib
	@mkdir -p $(ROOT)/build/c-tests
	@for f in $(ROOT)/c/core/tests/test_*.c $(ROOT)/c/core/tests/test_harness.c; do \
	  $(CC) $(CFLAGS) -I$(ROOT)/c/core/tests -c "$$f" -o "$(ROOT)/build/c-tests/$$(basename $$f .c).o" || exit 1; \
	done
	$(CC) $(ROOT)/build/c-tests/*.o $(ROOT)/build/c/libndcore.a -o $(ROOT)/build/c-tests/run-tests
	$(ROOT)/build/c-tests/run-tests

test-rust:
	$(CARGO) test --workspace --all-targets

test-e2e: link
	bash $(ROOT)/tests/e2e/scan_flow.sh
	bash $(ROOT)/tests/e2e/dropped_eicar_flow.sh
	bash $(ROOT)/tests/e2e/update_flow.sh

lint:
	@for f in $(ROOT)/c/core/src/*.c; do $(CLANG) $(CFLAGS) -fsyntax-only "$$f" || exit 1; done
	$(CARGO) clippy --workspace --all-targets -- -D warnings
	$(CARGO) fmt --all -- --check

check-static: link
	bash $(ROOT)/scripts/check-static.sh $(ROOT)/out/novusd

check-core-purity:
	bash $(ROOT)/scripts/check-core-purity.sh

size-check: novus-app
	bash $(ROOT)/scripts/size-check.sh

novus-app:
	$(CARGO) build --release -p nd-platform-novus --target x86_64-unknown-none --features app

clean:
	rm -rf $(ROOT)/build $(ROOT)/out $(ROOT)/target
