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
CFLAGS += -O3 -g3 -I./ -I$(LVGL_DIR)

ifeq ($(shell $(CC) -v 2>&1 | grep -c "clang version"), 1)
	CFLAGS += -Wuninitialized
else
	CFLAGS += -Wmaybe-uninitialized
endif

# directory for local libs
LDFLAGS = -L$(DESTDIR)$(PREFIX)/lib
LIBS += -lstdc++ -lm

VPATH =

$(info LDFLAGS ="$(LDFLAGS)")

include $(LVGL_DIR)/lvgl.mk
include $(LVGL_DIR)/lv_drivers/lv_drivers.mk

# folder for object files
OBJDIR = ./obj

CSRCS += $(wildcard *.c) $(wildcard assets/*.c)

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

