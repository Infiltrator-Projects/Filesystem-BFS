# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
PROFILE ?= portable
BASE = -std=c11 -Wall -Wextra -Werror -fPIC -fvisibility=hidden -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns -I Libraries/include
SRC = native/filesystems/bfs/bfs.c
OUT = build/BFS.so
LINK = -shared -nostdlib -Wl,-z,defs,-Bsymbolic
ifeq ($(PROFILE),aggressive)
OPT = -O3 -march=native -flto
PGO = $(CURDIR)/build/pgo
EXTRA = build/test_bfs
else
OPT = -O2 -march=x86-64 -mtune=generic
endif
.PHONY: all check clean
all: $(OUT)
$(OUT): $(SRC) $(EXTRA)
	mkdir -p build
ifeq ($(PROFILE),aggressive)
	mkdir -p $(PGO)
	$(CC) $(BASE) $(OPT) -shared -fprofile-generate=$(PGO) -o $@ $(SRC)
	build/test_bfs ./$(OUT)
	$(CC) $(BASE) $(OPT) $(LINK) -fprofile-use=$(PGO) -fprofile-correction -Werror=missing-profile -o $@ $(SRC)
else
	$(CC) $(BASE) $(OPT) $(LINK) -o $@ $(SRC)
endif
build/test_bfs: tests/test_bfs.c
	mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -I Libraries/include -o $@ $< -ldl
check: $(OUT) build/test_bfs
	test "$$(nm -D --defined-only $(OUT) | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	test -z "$$(nm -D --undefined-only $(OUT))"
	! readelf -d $(OUT) | grep -q NEEDED
	build/test_bfs ./$(OUT)
clean:
	rm -rf build
