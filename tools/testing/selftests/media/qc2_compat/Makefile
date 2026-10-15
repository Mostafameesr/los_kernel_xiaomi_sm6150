# SPDX-License-Identifier: GPL-2.0-only
ROOT := $(abspath $(CURDIR)/../../../../..)
VIDC := $(ROOT)/drivers/media/platform/msm/vidc
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c99
CPPFLAGS += -I$(VIDC) -include $(ROOT)/include/uapi/linux/v4l2-controls.h

ifeq ($(SANITIZE),1)
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

.PHONY: all check clean
all: qc2_compat_test

qc2_compat_test: qc2_compat_test.c $(VIDC)/msm_vidc_qc2_compat.c $(VIDC)/msm_vidc_qc2_compat.h
	$(CC) $(CPPFLAGS) $(CFLAGS) qc2_compat_test.c $(VIDC)/msm_vidc_qc2_compat.c $(LDFLAGS) -o $@

check: qc2_compat_test
	./qc2_compat_test

clean:
	rm -f qc2_compat_test
