# sit - suckless iptv
# See LICENSE file for copyright and license details.

include config.mk

SRC = sit.c gfx.c ui.c player.c util.c json.c net.c xtream.c m3u.c xmltv.c img.c store.c epg.c
OBJ = ${SRC:.c=.o}

all: sit

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk util.h gfx.h ui.h player.h json.h net.h playlist.h xtream.h m3u.h img.h store.h epg.h xmltv.h
gfx.o: vendor/stb_truetype.h
img.o: vendor/stb_image.h

config.h:
	cp config.def.h $@

sit: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f sit sit.exe ${OBJ}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f sit ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/sit

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/sit

.PHONY: all clean install uninstall
