KDIR ?= /lib/modules/$(shell uname -r)/build
obj-m += swiftbaton_k.o
swiftbaton_k-y := sbk_core.o sbk_rdma.o sbk_dispatch.o
ccflags-y += -I$(src)/../include -Werror
ifneq ($(OFA_DIR),)
NOSTDINC_FLAGS += -I$(OFA_DIR)/include -I$(OFA_DIR)/include/uapi -include $(OFA_DIR)/include/linux/compat-2.6.h
KBUILD_EXTRA_SYMBOLS += $(OFA_DIR)/Module.symvers
endif
.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
