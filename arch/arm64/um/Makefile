# SPDX-License-Identifier: GPL-2.0
#
# arch/arm64/um -- subarch support for ARCH=um SUBARCH=arm64
#

obj-y = bugs.o cpuinfo.o delay.o fault.o mem.o ptrace.o \
	setjmp_aarch64.o signal.o stub_segv.o syscalls.o \
	sys_call_table.o sysrq.o tls.o \
	subarch.o os-Linux/ vdso/

# Module loading is the parent architecture's, reused rather than reimplemented.
# The relocation types, the PLT veneers and the immediate-field encoders are
# properties of the aarch64 ELF ABI and of the instruction set, not of how the
# kernel is hosted, so a UML copy of them would be 900 lines that have to be
# kept in step with arch/arm64 forever. x86 UML has taken exactly this approach
# to its own module.c for years -- see arch/x86/um/Makefile.
#
# insn.o comes along because module.c encodes branch and move-wide immediates
# through aarch64_insn_encode_immediate().
subarch-$(CONFIG_MODULES) += ../kernel/module.o ../kernel/module-plts.o ../lib/insn.o

# patching.c supplies aarch64_insn_copy(), which module.c uses to write
# relocated instructions. arch/arm64's version manufactures a writable alias of
# read-only kernel text; a UML guest has no such mapping, so this one is a
# memcpy plus the I-cache maintenance that a real CPU still requires.
obj-$(CONFIG_MODULES) += patching.o

# Built with USER_CFLAGS against host headers rather than kernel headers:
# these speak to the host kernel, not to the guest.
USER_OBJS := bugs.o fault.o

$(obj)/user-offsets.s: c_flags = -Wp,-MD,$(depfile) $(USER_CFLAGS) \
	-Iarch/arm64/include/generated
targets += user-offsets.s

include/generated/user_constants.h: $(obj)/user-offsets.s FORCE
	$(call filechk,offsets,__USER_CONSTANT_H__)

# The SIGSEGV stub handler runs on the guest's stack inside the stub process
# with no libc and no stack protector available.
UNPROFILE_OBJS := stub_segv.o
CFLAGS_stub_segv.o := $(CFLAGS_NO_HARDENING)

include $(srctree)/arch/um/scripts/Makefile.rules
