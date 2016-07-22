include $(SRC_DIR)/Makefile.versions

ccflags-y		+= -iquote compel/src/lib/arch/$(ARCH)/include
ccflags-y		+= -iquote compel/include
ccflags-y		+= -iquote $(SRC_DIR)/criu/include
ccflags-y		+= -DCOMPEL_VERSION=\"$(COMPEL_SO_VERSION_MAJOR).$(COMPEL_SO_VERSION_MINOR)\"
ccflags-y		+= -fPIC

lib-y			+= src/lib/handle-elf.o
lib-y			+= src/lib/arch/$(ARCH)/handle-elf.o
lib-y			+= src/lib/handle-elf-32.o

obj-y			+= src/main.o

CFLAGS_handle-elf-32.o += -UCONFIG_X86_64 -DCONFIG_X86_32
CFLAGS_handle-elf-32.d += -UCONFIG_X86_64 -DCONFIG_X86_32

cleanup-y		+= compel/compel
cleanup-y		+= compel/libcompel.so
