CC ?= aarch64-linux-android-clang
CFLAGS ?= -O2 -static -Wall

all: injector

injector: injector.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f injector
