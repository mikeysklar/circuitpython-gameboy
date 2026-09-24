# Native module build for peanutgb. OPT is appended after the dynruntime.mk
# include so the last -O wins over its -Os default.
MPY_DIR ?= ../../circuitpython
ARCH ?= armv7emsp
MOD = peanutgb
SRC = peanutgb.c
BUILD = build/$(ARCH)
include $(MPY_DIR)/py/dynruntime.mk
OPT ?= -O2 -funroll-loops
CFLAGS += $(OPT)
