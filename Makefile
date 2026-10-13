CC      ?= cc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter
INCS     = -I/usr/include/SDL2 -I/usr/include/freetype2
LIBS     = -lSDL2 -lfreetype -lfontconfig -lm -lpthread

diskmap: diskmap.c
	$(CC) $(CFLAGS) $(INCS) -o $@ $< $(LIBS)
	strip $@

clean:
	rm -f diskmap

.PHONY: clean
