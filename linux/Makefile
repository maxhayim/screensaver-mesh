# Builds the XScreenSaver hack. Needs: libx11-dev libcairo2-dev libcurl4-openssl-dev
# (Debian/Ubuntu) or libX11-devel cairo-devel libcurl-devel (Fedora).
PREFIX ?= /usr/local
HACKDIR ?= $(PREFIX)/libexec/xscreensaver
CONFDIR ?= $(PREFIX)/share/xscreensaver/config
VERSION := $(shell cat ../VERSION)

CFLAGS ?= -O2
CFLAGS += -std=gnu99 -Wall -Wextra -I../core $(shell pkg-config --cflags x11 cairo libcurl)
LDLIBS += $(shell pkg-config --libs x11 cairo libcurl) -lpthread -lm

SRC = ../core/mesh.c ../core/json.c ../core/meshmonitor.c mesh_x11.c

../build/screensaver-mesh: $(SRC) ../core/*.h
	mkdir -p ../build
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS) $(LDLIBS)

install: ../build/screensaver-mesh
	install -D -m 755 ../build/screensaver-mesh $(DESTDIR)$(HACKDIR)/screensaver-mesh
	install -D -m 644 screensaver-mesh.xml $(DESTDIR)$(CONFDIR)/screensaver-mesh.xml

tarball: ../build/screensaver-mesh
	rm -rf ../build/screensaver-mesh-$(VERSION)-linux-x86_64
	mkdir -p ../build/screensaver-mesh-$(VERSION)-linux-x86_64
	cp ../build/screensaver-mesh screensaver-mesh.xml install.sh config.example INSTALL.txt ../LICENSE \
	   ../build/screensaver-mesh-$(VERSION)-linux-x86_64/
	tar -C ../build -czf ../build/screensaver-mesh-$(VERSION)-linux-x86_64.tar.gz screensaver-mesh-$(VERSION)-linux-x86_64

.PHONY: install tarball
