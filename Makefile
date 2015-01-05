# Lib iphoneossup is a little library with some iOS arm support
# derived from source code at www.opensource.apple.com.  Include this
# library in a project to get iOS TLS support that is not enabled in
# the normal iOS runtime.  Note that this is not needed for other
# iphoneos arch such as arm64
#
# Just type make, then include libiphoneossup.a in a project that
# expects the tlv_ functions.

LIBNAME = libiphoneossup.a
OBJS = threadLocalVariables.o threadLocalHelpers.o

# For now, just make for iphone 4
ARCH = armv7

CC = xcrun --sdk iphoneos cc
CFLAGS = -arch $(ARCH) -Os -g
# default .s rule uses as, but we want cc
COMPILE.s = $(CC) -arch $(ARCH) -c

all: $(LIBNAME)

$(LIBNAME): $(OBJS)
	$(AR) $(ARFLAGS) $@ $?

clean:
	-$(RM) *.o $(LIBNAME)
