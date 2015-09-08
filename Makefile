# Lib iphoneossup is a little library with some iOS arm support
# derived from source code at www.opensource.apple.com.  Include this
# library in a project to get iOS TLS support that is not enabled in
# the normal iOS runtime.
#
# Just type make, then include libiphoneossup.a in a project that
# expects the tlv_ functions.

LIBNAME = libiphoneossup.a
OBJS = threadLocalVariables.o

# For now, just make basic CPUs for iPhone4 through iPhone6
ARCHS = i386 x86_64 armv7 armv7s arm64

CFLAGS = -Os -g -miphoneos-version-min=5.1.1
LIBS = $(ARCHS:%=%-$(LIBNAME))

all: $(LIBNAME)

$(LIBNAME): $(LIBS)
	lipo -create -output $@ $(LIBS)

clean:
	-$(RM) *.o $(LIBS) $(LIBNAME)

# % is arch
%-libiphoneossup.a: $(addprefix %-,$(OBJS))
	$(AR) $(ARFLAGS) $@ $?

# compile for specific iOS arch
armv7-%.o: %.c
	xcrun --sdk iphoneos cc -arch armv7 $(CFLAGS) -c -o $@ $<
armv7s-%.o: %.c
	xcrun --sdk iphoneos cc -arch armv7s $(CFLAGS) -c -o $@ $<
arm64-%.o: %.c
	xcrun --sdk iphoneos cc -arch arm64 $(CFLAGS) -c -o $@ $<
i386-%.o: %.c
	xcrun --sdk iphonesimulator cc -arch i386 $(CFLAGS) -c -o $@ $<
x86_64-%.o: %.c
	xcrun --sdk iphonesimulator cc -arch x86_64 $(CFLAGS) -c -o $@ $<
