CC ?= cc
TARGET := i3-scratchpad-switcher
SRC := main.c
PKGS := gtk+-3.0 gio-2.0 json-glib-1.0 x11

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=

INSTALL ?= install
RM ?= rm -f

CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDLIBS += $(shell pkg-config --libs $(PKGS))

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

check-deps:
	@pkg-config --exists $(PKGS) || \
		(printf "Missing dependencies. Install: $(PKGS)\n" >&2; exit 1)

install: check-deps $(TARGET)
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 0755 "$(TARGET)" "$(DESTDIR)$(BINDIR)/$(TARGET)"

uninstall:
	$(RM) "$(DESTDIR)$(BINDIR)/$(TARGET)"

clean:
	$(RM) "$(TARGET)"

.PHONY: all check-deps install uninstall clean
