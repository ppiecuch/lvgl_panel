#
# Makefile
#

BIN = panel
DESTDIR = /usr
PREFIX = /local

LVGL_DIR = lvgl

CC = gcc
CXX = g++
CFLAGS = -Wall -Wshadow -Wundef
CFLAGS += -O3 -g3 -I./ -I./deps -I$(LVGL_DIR)
CFLAGS += -DPANEL_VERSION=\"0.1.0\" \
          -DPANEL_BUILD_DATE=\"$(shell date +%Y-%m-%d)\" \
          -DPANEL_BUILD_HASH=\"$(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)\"

# directory for local libs
LDFLAGS = -L$(DESTDIR)$(PREFIX)/lib
LIBS += -lstdc++ -lm -lcurl

ifeq ($(shell $(CC) -v 2>&1 | grep -c "clang version"), 1)
	CFLAGS += -Wuninitialized
else
	CFLAGS += -Wmaybe-uninitialized -Wno-format
endif

ifeq ($(OS),Windows_NT)
else
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		CFLAGS += -I/opt/local/include
		LIBS += -L/opt/local/lib -lSDL2 -lpthread
	else
		LIBS += -lbsd -lpthread
	endif
endif

VPATH =

$(info LDFLAGS ="$(LDFLAGS)")

include $(LVGL_DIR)/lvgl.mk
include $(LVGL_DIR)/lv_drivers/lv_drivers.mk
include $(LVGL_DIR)/lv_lib_png/lv_lib_png.mk

# folder for object files
OBJDIR = ./obj

CSRCS += $(wildcard *.c) $(wildcard assets/*.c) $(wildcard deps/*.c)

COBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(CSRCS))

SRCS = $(CSRCS)
OBJS = $(COBJS)

#.PHONY: clean

all: default

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC $< -> $@"
	@$(CC)  $(CFLAGS) -c $< -o $@

default: $(OBJS)
	$(CC) -o $(BIN) $(OBJS) $(LDFLAGS) $(LIBS)

# nothing to do but will print info
nothing:
	$(info OBJS ="$(OBJS)")
	$(info SRCS ="$(SRCS)")
	$(info DONE)


clean:
	rm -f $(OBJS)

