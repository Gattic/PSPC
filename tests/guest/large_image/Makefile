# Built by tools/build_guest_tests.sh with the pspdev toolchain.
TARGET = large_image
OBJS = main.o
CFLAGS = -O2 -G0 -Wall
LIBS = -lpspumd
BUILD_PRX = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
