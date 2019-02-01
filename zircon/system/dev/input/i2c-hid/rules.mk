# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := driver

MODULE_SRCS := $(LOCAL_DIR)/i2c-hid.c

MODULE_STATIC_LIBS := system/ulib/ddk system/ulib/hid system/ulib/sync

MODULE_LIBS := system/ulib/driver system/ulib/zircon system/ulib/c

MODULE_BANJO_LIBS := \
    system/banjo/ddk-protocol-hidbus \
    system/banjo/ddk-protocol-i2c \

ifeq ($(call TOBOOL,$(ENABLE_DRIVER_TRACING)),true)
MODULE_STATIC_LIBS += system/ulib/trace.driver
endif
MODULE_HEADER_DEPS += system/ulib/trace system/ulib/trace-engine

include make/module.mk