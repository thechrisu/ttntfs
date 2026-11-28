# Build mkseeds against the libntfs-3g built by tools/build-ntfsprogs.sh.
#   make -C tools/fuzz            # builds ./mkseeds
#   make -C tools/fuzz seeds      # generates tools/fuzz/corpus/*.img
TOOLS   := $(abspath ..)
ROOT    := $(abspath $(TOOLS)/..)
LOCAL   := $(TOOLS)/.local
CC      ?= cc
ARCH    := $(shell uname -m)
SDK     := $(shell xcrun --show-sdk-path 2>/dev/null)
CFLAGS  ?= -O2 -g
# -arch / -isysroot are clang-on-macOS only: gcc on Linux rejects -arch.
ifeq ($(shell uname -s),Darwin)
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration \
           -arch $(ARCH) $(if $(SDK),-isysroot $(SDK)) -I$(LOCAL)/include
LDFLAGS += -arch $(ARCH) $(if $(SDK),-isysroot $(SDK))
else
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration \
           -I$(LOCAL)/include
endif
LDLIBS  := $(LOCAL)/lib/libntfs-3g.a

all: mkseeds

mkseeds: mkseeds.c $(LOCAL)/lib/libntfs-3g.a
	$(CC) $(CFLAGS) -o $@ mkseeds.c $(LDFLAGS) $(LDLIBS)

$(LOCAL)/lib/libntfs-3g.a:
	$(TOOLS)/build-ntfsprogs.sh

seeds: mkseeds dirty-seed
	./mkseeds -o corpus --mkntfs $(LOCAL)/sbin/mkntfs --force

# The dirty-log seed needs this driver, not libntfs-3g: build the
# generator with CMake and run it (corpus/seed-dirty-log.img).
dirty-seed:
	cmake -S $(ROOT) -B $(ROOT)/build >/dev/null
	cmake --build $(ROOT)/build --target mkdirtyseed
	TMPDIR=$${TMPDIR:-/tmp} $(ROOT)/build/mkdirtyseed -o corpus/seed-dirty-log.img --mkntfs $(LOCAL)/sbin/mkntfs

clean:
	rm -f mkseeds
	rm -rf mkseeds.dSYM

.PHONY: all seeds dirty-seed clean
