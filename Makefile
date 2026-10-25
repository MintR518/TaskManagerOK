APP      := taskmanagerok
PODIR    := po
LANGS    := $(notdir $(patsubst %.po,%, $(wildcard $(PODIR)/*.po)))
MOFILES  := $(foreach l,$(LANGS),locale/$(l)/LC_MESSAGES/$(APP).mo)
LOCALEDIR := $(PREFIX)/share/locale

PREFIX   ?= /usr/local
BINDIR   := $(DESTDIR)$(PREFIX)/bin
DATADIR  := $(DESTDIR)$(PREFIX)/share

CC       ?= cc
GTK_CFLAGS ?= $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   ?= $(shell pkg-config --libs gtk+-3.0)

CFLAGS   ?= -O2 -std=gnu11 -Wall -Wextra
CPPFLAGS += -DLOCALEDIR=\"$(LOCALEDIR)\"
LDLIBS   += -lm

SRCS := src/main.c src/stats.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean test install uninstall pot

all: $(APP) $(MOFILES)

$(APP): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(GTK_LIBS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(GTK_CFLAGS) -c $< -o $@

src/main.o: src/stats.h
src/stats.o: src/stats.h

locale/%/LC_MESSAGES/$(APP).mo: $(PODIR)/%.po
	@mkdir -p $(dir $@)
	msgfmt -c -o $@ $<

pot:
	xgettext --foreign-user --from-code=UTF-8 -k_ -kN_ -o $(PODIR)/$(APP).pot src/*.c
	@for l in $(LANGS); do msgmerge --update --backup=none $(PODIR)/$$l.po $(PODIR)/$(APP).pot; done

test: build/test_stats
	./build/test_stats

build/test_stats: tools/test_stats.c src/stats.c src/stats.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ tools/test_stats.c src/stats.c $(LDLIBS)

install: $(APP) $(MOFILES)
	install -Dm755 $(APP) $(BINDIR)/$(APP)
	install -Dm644 data/$(APP).desktop $(DATADIR)/applications/$(APP).desktop
	install -Dm644 data/$(APP).svg $(DATADIR)/icons/hicolor/scalable/apps/$(APP).svg
	@for l in $(LANGS); do \
	  install -Dm644 locale/$$l/LC_MESSAGES/$(APP).mo \
	    $(DATADIR)/locale/$$l/LC_MESSAGES/$(APP).mo; \
	done

uninstall:
	rm -f $(BINDIR)/$(APP)
	rm -f $(DATADIR)/applications/$(APP).desktop
	rm -f $(DATADIR)/icons/hicolor/scalable/apps/$(APP).svg
	@for l in $(LANGS); do \
	  rm -f $(DATADIR)/locale/$$l/LC_MESSAGES/$(APP).mo; \
	done

clean:
	rm -f $(OBJS) $(APP)
	rm -rf build locale
