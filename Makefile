CC      ?= gcc
CFLAGS  ?= -O2 -g
# kept separate from CFLAGS on purpose: "make CFLAGS=-Werror" on the
# command line replaces CFLAGS completely, += in here doesn't survive that
BASE_CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?=

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))
BIN  := build/uds_server

.PHONY: all clean test asan

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

build/%.o: src/%.c | build
	$(CC) $(BASE_CFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

build:
	mkdir -p build

test: $(BIN)
	python3 -m pytest -v tests

# rebuild with address + UB sanitizer, then run the tests against it
asan: clean
	$(MAKE) CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address,undefined"
	python3 -m pytest -q tests

clean:
	rm -rf build

-include $(OBJS:.o=.d)
