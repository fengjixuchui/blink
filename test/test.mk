#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#───vi: set et ft=make ts=8 tw=8 fenc=utf-8 :vi───────────────────────┘

emulates = o/$(MODE)/$1.ok $(foreach ARCH,$(ARCHITECTURES),o/$(MODE)/$(ARCH)/$1.emulates)

# make -j8 o//test/alu
# very time consuming tests of our core arithmetic ops
# https://github.com/jart/cosmopolitan/blob/master/test/tool/build/lib/optest.c
# https://github.com/jart/cosmopolitan/blob/master/test/tool/build/lib/alu_test.c
# https://github.com/jart/cosmopolitan/blob/master/test/tool/build/lib/bsu_test.c
o/$(MODE)/test/alu:								\
		$(call emulates,third_party/cosmo/alu_test.com)			\
		$(call emulates,third_party/cosmo/bsu_test.com)
	@mkdir -p $(@D)
	@touch $@

# make -j8 o//test/sse
# fast and fairly comprehensive tests for our simd instructions
# https://github.com/jart/cosmopolitan/blob/master/test/libc/intrin/intrin_test.c
o/$(MODE)/test/sse:								\
		$(call emulates,third_party/cosmo/intrin_test.com)		\
		$(call emulates,third_party/cosmo/popcnt_test.com)		\
		$(call emulates,third_party/cosmo/pshuf_test.com)		\
		$(call emulates,third_party/cosmo/pmulhrsw_test.com)		\
		$(call emulates,third_party/cosmo/divmul_test.com)		\
		$(call emulates,third_party/cosmo/palignr_test.com)
	@mkdir -p $(@D)
	@touch $@

# make -j8 o//test/lib
# test some c libraries
o/$(MODE)/test/lib:								\
		$(call emulates,third_party/cosmo/atoi_test.com)		\
		$(call emulates,third_party/cosmo/qsort_test.com)		\
		$(call emulates,third_party/cosmo/kprintf_test.com)
	@mkdir -p $(@D)
	@touch $@

# make -j8 o//test/sys
# test linux system call emulation
o/$(MODE)/test/sys:								\
		$(call emulates,third_party/cosmo/renameat_test.com)		\
		$(call emulates,third_party/cosmo/clock_gettime_test.com)	\
		$(call emulates,third_party/cosmo/clock_getres_test.com)	\
		$(call emulates,third_party/cosmo/clock_nanosleep_test.com)	\
		$(call emulates,third_party/cosmo/access_test.com)		\
		$(call emulates,third_party/cosmo/chdir_test.com)		\
		$(call emulates,third_party/cosmo/closefrom_test.com)		\
		$(call emulates,third_party/cosmo/commandv_test.com)		\
		$(call emulates,third_party/cosmo/dirstream_test.com)		\
		$(call emulates,third_party/cosmo/dirname_test.com)		\
		$(call emulates,third_party/cosmo/clone_test.com)		\
		$(call emulates,third_party/cosmo/fileexists_test.com)		\
		$(call emulates,third_party/cosmo/getcwd_test.com)		\
		$(call emulates,third_party/cosmo/lseek_test.com)		\
		$(call emulates,third_party/cosmo/mkdir_test.com)		\
		$(call emulates,third_party/cosmo/makedirs_test.com)		\
		$(call emulates,third_party/cosmo/nanosleep_test.com)		\
		$(call emulates,third_party/cosmo/readlinkat_test.com)		\
		$(call emulates,third_party/cosmo/symlinkat_test.com)		\
		$(call emulates,third_party/cosmo/tls_test.com)			\
		$(call emulates,third_party/cosmo/tmpfile_test.com)		\
		$(call emulates,third_party/cosmo/xslurp_test.com)
	@mkdir -p $(@D)
	@touch $@

o/$(MODE)/%.runs: o/$(MODE)/%
	$<
	@touch $@

o/$(MODE)/%.ok: % o/$(MODE)/blink/blink
	@mkdir -p $(@D)
	o/$(MODE)/blink/blink $<
	@touch $@

o/$(MODE)/i486/%.runs: o/$(MODE)/i486/% o/third_party/qemu/qemu-i486
	$(VM) o/third_party/qemu/qemu-i486 $<
	@touch $@
o/$(MODE)/m68k/%.runs: o/$(MODE)/m68k/% o/third_party/qemu/qemu-m68k
	$(VM) o/third_party/qemu/qemu-m68k $<
	@touch $@
o/$(MODE)/x86_64/%.runs: o/$(MODE)/x86_64/% o/third_party/qemu/qemu-x86_64
	$(VM) o/third_party/qemu/qemu-x86_64 $<
	@touch $@
o/$(MODE)/arm/%.runs: o/$(MODE)/arm/% o/third_party/qemu/qemu-arm
	$(VM) o/third_party/qemu/qemu-arm $<
	@touch $@
o/$(MODE)/aarch64/%.runs: o/$(MODE)/aarch64/% o/third_party/qemu/qemu-aarch64
	$(VM) o/third_party/qemu/qemu-aarch64 $<
	@touch $@
o/$(MODE)/riscv64/%.runs: o/$(MODE)/riscv64/% o/third_party/qemu/qemu-riscv64
	$(VM) o/third_party/qemu/qemu-riscv64 $<
	@touch $@
o/$(MODE)/mips/%.runs: o/$(MODE)/mips/% o/third_party/qemu/qemu-mips
	$(VM) o/third_party/qemu/qemu-mips $<
	@touch $@
o/$(MODE)/mipsel/%.runs: o/$(MODE)/mipsel/% o/third_party/qemu/qemu-mipsel
	$(VM) o/third_party/qemu/qemu-mipsel $<
	@touch $@
o/$(MODE)/mips64/%.runs: o/$(MODE)/mips64/% o/third_party/qemu/qemu-mips64
	$(VM) o/third_party/qemu/qemu-mips64 $<
	@touch $@
o/$(MODE)/mips64el/%.runs: o/$(MODE)/mips64el/% o/third_party/qemu/qemu-mips64el
	$(VM) o/third_party/qemu/qemu-mips64el $<
	@touch $@
o/$(MODE)/s390x/%.runs: o/$(MODE)/s390x/% o/third_party/qemu/qemu-s390x
	$(VM) o/third_party/qemu/qemu-s390x $<
	@touch $@
o/$(MODE)/microblaze/%.runs: o/$(MODE)/microblaze/% o/third_party/qemu/qemu-microblaze
	$(VM) o/third_party/qemu/qemu-microblaze $<
	@touch $@
o/$(MODE)/powerpc/%.runs: o/$(MODE)/powerpc/% o/third_party/qemu/qemu-powerpc
	$(VM) o/third_party/qemu/qemu-powerpc $<
	@touch $@
o/$(MODE)/powerpc64le/%.runs: o/$(MODE)/powerpc64le/% o/third_party/qemu/qemu-powerpc64le
	$(VM) o/third_party/qemu/qemu-powerpc64le $<
	@touch $@

o/$(MODE)/i486/%.emulates: % o//i486/blink/blink o/third_party/qemu/qemu-i486
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-i486 o//i486/blink/blink $<
	@touch $@
o/$(MODE)/m68k/%.emulates: % o//m68k/blink/blink o/third_party/qemu/qemu-m68k
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-m68k o//m68k/blink/blink $<
	@touch $@
o/$(MODE)/x86_64/%.emulates: % o//x86_64/blink/blink o/third_party/qemu/qemu-x86_64
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-x86_64 o//x86_64/blink/blink $<
	@touch $@
o/$(MODE)/arm/%.emulates: % o//arm/blink/blink o/third_party/qemu/qemu-arm
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-arm o//arm/blink/blink $<
	@touch $@
o/$(MODE)/aarch64/%.emulates: % o//aarch64/blink/blink o/third_party/qemu/qemu-aarch64
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-aarch64 o//aarch64/blink/blink $<
	@touch $@
o/$(MODE)/riscv64/%.emulates: % o//riscv64/blink/blink o/third_party/qemu/qemu-riscv64
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-riscv64 o//riscv64/blink/blink $<
	@touch $@
o/$(MODE)/mips/%.emulates: % o//mips/blink/blink o/third_party/qemu/qemu-mips
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-mips o//mips/blink/blink $<
	@touch $@
o/$(MODE)/mipsel/%.emulates: % o//mipsel/blink/blink o/third_party/qemu/qemu-mipsel
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-mipsel o//mipsel/blink/blink $<
	@touch $@
o/$(MODE)/mips64/%.emulates: % o//mips64/blink/blink o/third_party/qemu/qemu-mips64
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-mips64 o//mips64/blink/blink $<
	@touch $@
o/$(MODE)/mips64el/%.emulates: % o//mips64el/blink/blink o/third_party/qemu/qemu-mips64el
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-mips64el o//mips64el/blink/blink $<
	@touch $@
o/$(MODE)/s390x/%.emulates: % o//s390x/blink/blink o/third_party/qemu/qemu-s390x
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-s390x o//s390x/blink/blink $<
	@touch $@
o/$(MODE)/microblaze/%.emulates: % o//microblaze/blink/blink o/third_party/qemu/qemu-microblaze
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-microblaze o//microblaze/blink/blink $<
	@touch $@
o/$(MODE)/powerpc/%.emulates: % o//powerpc/blink/blink o/third_party/qemu/qemu-powerpc
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-powerpc o//powerpc/blink/blink $<
	@touch $@
o/$(MODE)/powerpc64le/%.emulates: % o//powerpc64le/blink/blink o/third_party/qemu/qemu-powerpc64le
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-powerpc64le o//powerpc64le/blink/blink $<
	@touch $@

o/$(MODE)/i486/%.emulates: o/$(MODE)/% o//i486/blink/blink o/third_party/qemu/qemu-i486
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-i486 o//i486/blink/blink $<
	@touch $@
o/$(MODE)/m68k/%.emulates: o/$(MODE)/% o//m68k/blink/blink o/third_party/qemu/qemu-m68k
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-m68k o//m68k/blink/blink $<
	@touch $@
o/$(MODE)/x86_64/%.emulates: o/$(MODE)/% o//x86_64/blink/blink o/third_party/qemu/qemu-x86_64
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-x86_64 o//x86_64/blink/blink $<
	@touch $@
o/$(MODE)/arm/%.emulates: o/$(MODE)/% o//arm/blink/blink o/third_party/qemu/qemu-arm
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-arm o//arm/blink/blink $<
	@touch $@
o/$(MODE)/aarch64/%.emulates: o/$(MODE)/% o//aarch64/blink/blink o/third_party/qemu/qemu-aarch64
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-aarch64 o//aarch64/blink/blink $<
	@touch $@
o/$(MODE)/riscv64/%.emulates: o/$(MODE)/% o//riscv64/blink/blink o/third_party/qemu/qemu-riscv64
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-riscv64 o//riscv64/blink/blink $<
	@touch $@
o/$(MODE)/mips/%.emulates: o/$(MODE)/% o//mips/blink/blink o/third_party/qemu/qemu-mips
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-mips o//mips/blink/blink $<
	@touch $@
o/$(MODE)/mipsel/%.emulates: o/$(MODE)/% o//mipsel/blink/blink o/third_party/qemu/qemu-mipsel
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-mipsel o//mipsel/blink/blink $<
	@touch $@
o/$(MODE)/mips64/%.emulates: o/$(MODE)/% o//mips64/blink/blink o/third_party/qemu/qemu-mips64
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-mips64 o//mips64/blink/blink $<
	@touch $@
o/$(MODE)/mips64el/%.emulates: o/$(MODE)/% o//mips64el/blink/blink o/third_party/qemu/qemu-mips64el
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-mips64el o//mips64el/blink/blink $<
	@touch $@
o/$(MODE)/s390x/%.emulates: o/$(MODE)/% o//s390x/blink/blink o/third_party/qemu/qemu-s390x
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-s390x o//s390x/blink/blink $<
	@touch $@
o/$(MODE)/microblaze/%.emulates: o/$(MODE)/% o//microblaze/blink/blink o/third_party/qemu/qemu-microblaze
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-microblaze o//microblaze/blink/blink $<
	@touch $@
o/$(MODE)/powerpc/%.emulates: o/$(MODE)/% o//powerpc/blink/blink o/third_party/qemu/qemu-powerpc
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-powerpc o//powerpc/blink/blink $<
	@touch $@
o/$(MODE)/powerpc64le/%.emulates: o/$(MODE)/% o//powerpc64le/blink/blink o/third_party/qemu/qemu-powerpc64le
	@mkdir -p $(@D)
	$(VM) o/third_party/qemu/qemu-powerpc64le o//powerpc64le/blink/blink $<
	@touch $@

o/$(MODE)/test:				\
	o/$(MODE)/test/blink

o/$(MODE)/test/emulates:		\
	o/$(MODE)/test/blink/emulates
