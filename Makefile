CFLAGS=-Wall -I./include/ $(shell pkg-config libusb-1.0 fuse --cflags)
LIBS=$(shell pkg-config libusb-1.0 fuse --libs)

all:
	gcc -o rockfuse $(CFLAGS) src/*.c $(LIBS)
