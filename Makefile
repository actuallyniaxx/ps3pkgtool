CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra

ps3pkgtool: ps3pkgtool.c
	$(CC) $(CFLAGS) $< -o $@ -lpthread

test: ps3pkgtool
	tests/roundtrip.sh ./ps3pkgtool

clean:
	rm -f ps3pkgtool ps3pkgtool.exe

.PHONY: test clean
