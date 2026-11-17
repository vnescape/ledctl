CC := /usr/bin/xcrun clang
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
          -fstack-protector-strong
LDLIBS := -framework IOKit

.PHONY: all clean

all: ledctl

ledctl: ledctl.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

clean:
	$(RM) ledctl
