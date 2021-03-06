iphoneos-apple-support
======================

This little library, libiphoneossup, packages some useful iOS stuff derived
from http://www.opensource.apple.com.  In particular, it provides
support for thread local variables on arm/arm64 and i386/x86_64 (iOS
Simulator) which is needed to fully use the D programming language on
iOS.

Please read the APPLE_LICENSE before using.

To build, you must have the iPhoneOS platform SDK and Xcode command
line tools installed.

	$ cd iphone-apple-support
	$ make

Then add libiphoneossup.a to your Xcode project.

Currently the Makefile builds libiphoneossup.a for the armv7, armv7s, arm64, i386, and x86_6
architectures which supports iPhone 4 and up.

A fork of LDC, the LLVM-based D compiler, with iOS support can be
found at https://github.com/smolt/ldc, a modified version of LLVM
at https://github.com/smolt/llvm, and a repository to help glue it all
together at https://github.com/smolt/ldc-iphone-dev.

Background
----------

In 2014, I had fun hacking LDC into a cross-compiler to iOS.  The D
programming language relies heavily on thread local storage (TLS), but
iOS does not have such builtin
support.  Without TLS, the D runtime and standard library phobos are
unsafe to use in a multi-threaded environment.

After some Internet digging, I discovered that dyld, the iOS dynamic
loader, does have the TLS goods for 32-bit ARM but it is not enabled.
What would happen if I just recompiled the threadLocal parts of dyld
(the tlv_ code) and enabled it for `__arm__`?  Turns out it works!

Ideally the support for 32-bit ARM TLS would be conditionally included
in the D runtime but my understanding of the Apple License is that it
could add an extra burden on all users of the D.  So instead of
figuring it out, I though it would be safer to tuck the Apple Licensed
code away here.  All one needs to do is add this library when
targeting iOS and follow the provisions of the license.

Source here is from
http://www.opensource.apple.com/source/dyld/dyld-353.2.1/
- include/mach-o/dyld_priv.h
- src/threadLocalVariables.c

Dan Olson
