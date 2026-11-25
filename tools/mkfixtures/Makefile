# Build mkfixtures against the libntfs-3g built by tools/build-ntfsprogs.sh.
#   make -C tools/mkfixtures            # builds ./mkfixtures
#   make -C tools/mkfixtures fixtures   # generates tools/images/*.img + manifests
TOOLS   := $(abspath ..)
LOCAL   := $(TOOLS)/.local
IMAGES  := $(TOOLS)/images
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

SRCS := mkfixtures.c ../common/json.c ../common/sha256.c

all: mkfixtures

mkfixtures: $(SRCS) ../common/json.h ../common/sha256.h $(LOCAL)/lib/libntfs-3g.a
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LDLIBS)

$(LOCAL)/lib/libntfs-3g.a:
	$(TOOLS)/build-ntfsprogs.sh

fixtures: mkfixtures
	./mkfixtures -o $(IMAGES) --mkntfs $(LOCAL)/sbin/mkntfs $(FIXTURES)

clean:
	rm -f mkfixtures
	rm -rf mkfixtures.dSYM

.PHONY: all fixtures clean
