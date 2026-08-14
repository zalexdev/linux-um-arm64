# SPDX-License-Identifier: GPL-2.0
#
# Building the vDSO image for UML/arm64.

# Objects that go into the vDSO itself
vobjs-y := vdso-note.o sigreturn.o

# Objects linked into the UML kernel
obj-y += vdso.o vma.o

vobjs := $(foreach F,$(vobjs-y),$(obj)/$F)

targets += vdso.so vdso.so.dbg vdso.lds vdso-offsets.h $(vobjs-y)

CPPFLAGS_vdso.lds += -P -C

# The vDSO is one guest page. A 16K- or 64K-page host needs the segment
# alignment to match, so this is derived from the configuration rather than
# hardcoded to 4096 the way x86 UML does it.
vdso-pagesize-$(CONFIG_PAGE_SIZE_4KB)  := 4096
vdso-pagesize-$(CONFIG_PAGE_SIZE_16KB) := 16384
vdso-pagesize-$(CONFIG_PAGE_SIZE_64KB) := 65536
vdso-pagesize := $(vdso-pagesize-y)

VDSO_LDFLAGS_vdso.lds = -Wl,-soname=linux-vdso.so.1 \
	-Wl,-z,max-page-size=$(vdso-pagesize)

VDSO_LDFLAGS = -fPIC -shared -Wl,--hash-style=sysv -z noexecstack \
	-Wl,--no-undefined -Wl,--build-id=sha1

# $(CLANG_FLAGS) carries --target=; x86's equivalent rule gets away with -m64
# instead. Without it clang picks the build machine's linker emulation and fails
# with "cannot represent machine `aarch64'".
quiet_cmd_vdso = VDSO    $@
      cmd_vdso = $(CC) -nostdlib -o $@ $(filter --target=%,$(CLANG_FLAGS)) \
		       $(CC_FLAGS_LTO) $(VDSO_LDFLAGS) \
		       $(VDSO_LDFLAGS_$(filter %.lds,$(^F))) \
		       -Wl,-T,$(filter %.lds,$^) $(filter %.o,$^)

$(obj)/vdso.so.dbg: $(obj)/vdso.lds $(vobjs) FORCE
	$(call if_changed,vdso)

$(obj)/%.so: OBJCOPYFLAGS := -S
$(obj)/%.so: $(obj)/%.so.dbg FORCE
	$(call if_changed,objcopy)

$(obj)/vdso.o: $(src)/vdso.S $(obj)/vdso.so

# Symbol offsets, so signal delivery can find __kernel_rt_sigreturn.
gen-vdsosym := $(src)/gen_vdso_offsets.sh
quiet_cmd_vdsosym = VDSOSYM $@
      cmd_vdsosym = $(NM) $< | $(gen-vdsosym) | LC_ALL=C sort > $@

$(obj)/vdso-offsets.h: $(obj)/vdso.so.dbg FORCE
	$(call if_changed,vdsosym)

$(obj)/vma.o: $(obj)/vdso-offsets.h
CFLAGS_vma.o += -I$(objtree)/$(obj)

# vDSO code runs in guest userspace: no profiling, no instrumentation, and no
# stack protector (there is no guest TLS set up when the trampoline runs).
CFL := -fPIC -O2 -fasynchronous-unwind-tables -fno-stack-protector \
       -fno-omit-frame-pointer -foptimize-sibling-calls \
       $(filter -g%,$(KBUILD_CFLAGS))

$(vobjs): KBUILD_CFLAGS += $(CFL)

CFLAGS_REMOVE_vdso-note.o = -pg -fprofile-arcs -ftest-coverage
CFLAGS_REMOVE_sigreturn.o = -pg -fprofile-arcs -ftest-coverage

KCOV_INSTRUMENT := n
KASAN_SANITIZE  := n
UBSAN_SANITIZE  := n
