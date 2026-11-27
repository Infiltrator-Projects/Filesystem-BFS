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

# A separately qualified baseline AMD64 image is embedded in the single
# installed hosted carrier. Native optimization remains a hosted concern.
OBJCOPY ?= objcopy
FIRMWARE_FLAGS = -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -mno-red-zone -mgeneral-regs-only -fcf-protection=none -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fPIC -fvisibility=hidden
FIRMWARE_LINK = -shared -nostdlib -Wl,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now,-z,nopack-relative-relocs,-z,max-page-size=4096
.PHONY: firmware carrier
firmware: build/firmware/BFS.so
build/firmware/BFS.so: native/filesystems/bfs/bfs.c $(wildcard native/filesystems/bfs/*.h) module/firmware/CMakeLists.txt Makefile
	mkdir -p build/firmware
	$(CC) $(FIRMWARE_FLAGS) -DIFS_BOOT_FIRMWARE=1 -ILibraries/include $(FIRMWARE_LINK) -o $@ native/filesystems/bfs/bfs.c
	test "$$(nm -D --defined-only $@ | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	test -z "$$(nm -D --undefined-only $@)"
	! readelf -dW $@ | grep -E 'NEEDED|SONAME|INIT_ARRAY|FINI_ARRAY|RELR'
carrier: build/BFS.so firmware
	mkdir -p build/carrier
	$(OBJCOPY) --add-section .infiltrator_boot=build/firmware/BFS.so --set-section-flags .infiltrator_boot=readonly build/BFS.so build/carrier/BFS.so
	$(OBJCOPY) --dump-section .infiltrator_boot=build/carrier/verify.elf build/carrier/BFS.so build/carrier/verify-carrier.elf
	cmp build/firmware/BFS.so build/carrier/verify.elf
	rm -f build/carrier/verify.elf build/carrier/verify-carrier.elf
