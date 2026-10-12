# Built by tools/build_guest_tests.sh with the pspdev toolchain.
TARGET = coroutines
OBJS = main.o switch.o
CFLAGS = -O2 -G0 -Wall
LIBS =
BUILD_PRX = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
