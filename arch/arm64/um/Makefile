# SPDX-License-Identifier: GPL-2.0
#
# arch/arm64/um -- subarch support for ARCH=um SUBARCH=arm64
#

obj-y = bugs.o cpuinfo.o delay.o fault.o mem.o ptrace.o \
	setjmp_aarch64.o signal.o stub_segv.o syscalls.o \
	sys_call_table.o sysrq.o tls.o \
	os-Linux/ vdso/

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
