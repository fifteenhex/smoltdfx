# smoltdfx - drive the 3dfx Voodoo3 (Avenger/SST) 3D engine from
# userspace through a /dev/tdfx3d misc device.
#
# This is a tiny freestanding (nolibc) program: no libc, static, it
# mmaps the card's register aperture and issues 3D commands directly,
# synchronised to the vertical blank.
#
# It builds against a Linux source tree that provides the /dev/tdfx3d
# misc device (added by out-of-tree patches): the kernel's in-tree
# nolibc, and the installed uapi header <linux/tdfx3d.h>.
#
#   # in the kernel tree, once:
#   make headers_install            # (or: make O=build headers_install)
#   # then here:
#   make KDIR=/path/to/linux
#
# HDRS defaults to $(KDIR)/usr/include; for an out-of-tree kernel build
# pass HDRS=$(KDIR)/build/usr/include.

KDIR   ?= /usr/src/linux
HDRS   ?= $(KDIR)/usr/include
NOLIBC ?= $(KDIR)/tools/include/nolibc
CC     ?= gcc
GCCINC := $(shell $(CC) -print-file-name=include)

CFLAGS := -Os -static -nostdlib -nostdinc -isystem $(GCCINC) \
	  -I$(HDRS) -I$(NOLIBC) -include nolibc.h -fno-stack-protector

# smolminigl (the OpenGL-1.x subset on smoltdfx) needs its cglm matrix/vector
# math and stb texture resampler; both are sibling checkouts.
CGLM   ?= $(CURDIR)/../cglm/include
STB    ?= $(CURDIR)/../stb
SMGCFLAGS := $(CFLAGS) -I. -I$(CGLM) -I$(STB)

# OpenGL Red Book-style demos, each rendered through smolminigl.
REDBOOK := redbook/smooth redbook/transform redbook/cube redbook/texquad redbook/blend \
	   redbook/light

tdfx3d_demo: tdfx3d_demo.c smoltdfx.h
	$(CC) $(CFLAGS) -o $@ $< -lgcc

redbook: $(REDBOOK)
$(REDBOOK): %: %.c smolminigl.c GL/gl.h smoltdfx.h smg_cglm.h smg_stb.h
	$(CC) $(SMGCFLAGS) -o $@ $< -lgcc

clean:
	rm -f tdfx3d_demo $(REDBOOK)

.PHONY: clean redbook
