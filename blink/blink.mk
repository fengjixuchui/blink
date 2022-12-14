#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#───vi: set et ft=make ts=8 tw=8 fenc=utf-8 :vi───────────────────────┘

PKGS += BLINK
BLINK_FILES := $(wildcard blink/*)
BLINK_SRCS = $(filter %.c,$(BLINK_FILES))
BLINK_HDRS = $(filter %.h,$(BLINK_FILES))

# avoid junk being placed in micro-operations
# TODO(jart): separate out uops into disjoint files
o/$(MODE)/blink/uop.o: private CFLAGS += -fno-stack-protector
ifneq ($(MODE), prof)
o/$(MODE)/blink/uop.o: private CFLAGS += -fomit-frame-pointer
endif

# vectorization makes code smaller
o/$(MODE)/blink/sse2.o: private CFLAGS += -O3
o/$(MODE)/x86_64/blink/sse2.o: private CFLAGS += -O3
o/$(MODE)/aarch64/blink/sse2.o: private CFLAGS += -O3

# these files have big switch statements
o/tiny/blink/cvt.o: private CFLAGS += -fpie
o/tiny/x86_64/blink/cvt.o: private CFLAGS += -fpie
o/tiny/aarch64/blink/cvt.o: private CFLAGS += -fpie
o/tiny/blink/fpu.o: private CFLAGS += -fpie
o/tiny/x86_64/blink/fpu.o: private CFLAGS += -fpie
o/tiny/aarch64/blink/fpu.o: private CFLAGS += -fpie
o/tiny/blink/x86.o: private CFLAGS += -fpie
o/tiny/x86_64/blink/x86.o: private CFLAGS += -fpie
o/tiny/aarch64/blink/x86.o: private CFLAGS += -fpie
o/tiny/blink/syscall.o: private CFLAGS += -fpie
o/tiny/x86_64/blink/syscall.o: private CFLAGS += -fpie
o/tiny/aarch64/blink/syscall.o: private CFLAGS += -fpie

o/$(MODE)/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/%.o)))
o/$(MODE)/i486/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/i486/%.o)))
o/$(MODE)/m68k/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/m68k/%.o)))
o/$(MODE)/x86_64/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/x86_64/%.o)))
o/$(MODE)/arm/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/arm/%.o)))
o/$(MODE)/aarch64/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/aarch64/%.o)))
o/$(MODE)/riscv64/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/riscv64/%.o)))
o/$(MODE)/mips/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/mips/%.o)))
o/$(MODE)/mipsel/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/mipsel/%.o)))
o/$(MODE)/mips64/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/mips64/%.o)))
o/$(MODE)/mips64el/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/mips64el/%.o)))
o/$(MODE)/s390x/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/s390x/%.o)))
o/$(MODE)/microblaze/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/microblaze/%.o)))
o/$(MODE)/powerpc/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/powerpc/%.o)))
o/$(MODE)/powerpc64le/blink/blink.a: $(filter-out %/blink.o,$(filter-out %/tui.o,$(BLINK_SRCS:%.c=o/$(MODE)/powerpc64le/%.o)))

o/$(MODE)/blink/blink: o/$(MODE)/blink/blink.o o/$(MODE)/blink/blink.a
	$(CC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/i486/blink/blink: o/$(MODE)/i486/blink/blink.o o/$(MODE)/i486/blink/blink.a
	$(VM) o/third_party/gcc/i486/bin/i486-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/m68k/blink/blink: o/$(MODE)/m68k/blink/blink.o o/$(MODE)/m68k/blink/blink.a
	$(VM) o/third_party/gcc/m68k/bin/m68k-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/x86_64/blink/blink: o/$(MODE)/x86_64/blink/blink.o o/$(MODE)/x86_64/blink/blink.a
	$(VM) o/third_party/gcc/x86_64/bin/x86_64-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/arm/blink/blink: o/$(MODE)/arm/blink/blink.o o/$(MODE)/arm/blink/blink.a
	$(VM) o/third_party/gcc/arm/bin/arm-linux-musleabi-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/aarch64/blink/blink: o/$(MODE)/aarch64/blink/blink.o o/$(MODE)/aarch64/blink/blink.a
	$(VM) o/third_party/gcc/aarch64/bin/aarch64-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/riscv64/blink/blink: o/$(MODE)/riscv64/blink/blink.o o/$(MODE)/riscv64/blink/blink.a
	$(VM) o/third_party/gcc/riscv64/bin/riscv64-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/mips/blink/blink: o/$(MODE)/mips/blink/blink.o o/$(MODE)/mips/blink/blink.a
	$(VM) o/third_party/gcc/mips/bin/mips-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/mipsel/blink/blink: o/$(MODE)/mipsel/blink/blink.o o/$(MODE)/mipsel/blink/blink.a
	$(VM) o/third_party/gcc/mipsel/bin/mipsel-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/mips64/blink/blink: o/$(MODE)/mips64/blink/blink.o o/$(MODE)/mips64/blink/blink.a
	$(VM) o/third_party/gcc/mips64/bin/mips64-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/mips64el/blink/blink: o/$(MODE)/mips64el/blink/blink.o o/$(MODE)/mips64el/blink/blink.a
	$(VM) o/third_party/gcc/mips64el/bin/mips64el-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/s390x/blink/blink: o/$(MODE)/s390x/blink/blink.o o/$(MODE)/s390x/blink/blink.a
	$(VM) o/third_party/gcc/s390x/bin/s390x-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/microblaze/blink/blink: o/$(MODE)/microblaze/blink/blink.o o/$(MODE)/microblaze/blink/blink.a
	$(VM) o/third_party/gcc/microblaze/bin/microblaze-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/powerpc/blink/blink: o/$(MODE)/powerpc/blink/blink.o o/$(MODE)/powerpc/blink/blink.a
	$(VM) o/third_party/gcc/powerpc/bin/powerpc-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/powerpc64le/blink/blink: o/$(MODE)/powerpc64le/blink/blink.o o/$(MODE)/powerpc64le/blink/blink.a
	$(VM) o/third_party/gcc/powerpc64le/bin/powerpc64le-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@

o/$(MODE)/blink/tui: o/$(MODE)/blink/tui.o o/$(MODE)/blink/blink.a
	$(CC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/i486/blink/tui: o/$(MODE)/i486/blink/tui.o o/$(MODE)/i486/blink/blink.a
	$(VM) o/third_party/gcc/i486/bin/i486-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/m68k/blink/tui: o/$(MODE)/m68k/blink/tui.o o/$(MODE)/m68k/blink/blink.a
	$(VM) o/third_party/gcc/m68k/bin/m68k-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/x86_64/blink/tui: o/$(MODE)/x86_64/blink/tui.o o/$(MODE)/x86_64/blink/blink.a
	$(VM) o/third_party/gcc/x86_64/bin/x86_64-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/arm/blink/tui: o/$(MODE)/arm/blink/tui.o o/$(MODE)/arm/blink/blink.a
	$(VM) o/third_party/gcc/arm/bin/arm-linux-musleabi-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/aarch64/blink/tui: o/$(MODE)/aarch64/blink/tui.o o/$(MODE)/aarch64/blink/blink.a
	$(VM) o/third_party/gcc/aarch64/bin/aarch64-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/riscv64/blink/tui: o/$(MODE)/riscv64/blink/tui.o o/$(MODE)/riscv64/blink/blink.a
	$(VM) o/third_party/gcc/riscv64/bin/riscv64-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/mips/blink/tui: o/$(MODE)/mips/blink/tui.o o/$(MODE)/mips/blink/blink.a
	$(VM) o/third_party/gcc/mips/bin/mips-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/mipsel/blink/tui: o/$(MODE)/mipsel/blink/tui.o o/$(MODE)/mipsel/blink/blink.a
	$(VM) o/third_party/gcc/mipsel/bin/mipsel-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/mips64/blink/tui: o/$(MODE)/mips64/blink/tui.o o/$(MODE)/mips64/blink/blink.a
	$(VM) o/third_party/gcc/mips64/bin/mips64-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/mips64el/blink/tui: o/$(MODE)/mips64el/blink/tui.o o/$(MODE)/mips64el/blink/blink.a
	$(VM) o/third_party/gcc/mips64el/bin/mips64el-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/s390x/blink/tui: o/$(MODE)/s390x/blink/tui.o o/$(MODE)/s390x/blink/blink.a
	$(VM) o/third_party/gcc/s390x/bin/s390x-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/microblaze/blink/tui: o/$(MODE)/microblaze/blink/tui.o o/$(MODE)/microblaze/blink/blink.a
	$(VM) o/third_party/gcc/microblaze/bin/microblaze-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/powerpc/blink/tui: o/$(MODE)/powerpc/blink/tui.o o/$(MODE)/powerpc/blink/blink.a
	$(VM) o/third_party/gcc/powerpc/bin/powerpc-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@
o/$(MODE)/powerpc64le/blink/tui: o/$(MODE)/powerpc64le/blink/tui.o o/$(MODE)/powerpc64le/blink/blink.a
	$(VM) o/third_party/gcc/powerpc64le/bin/powerpc64le-linux-musl-gcc $(LDFLAGS_STATIC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@

o/$(MODE)/blink/oneoff.com: o/$(MODE)/blink/oneoff.o o/$(MODE)/blink/blink.a
	$(CC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@

o/$(MODE)/blink:				\
		o/$(MODE)/blink/tui		\
		o/$(MODE)/blink/blink		\
		$(BLINK_HDRS:%=o/$(MODE)/%.ok)
