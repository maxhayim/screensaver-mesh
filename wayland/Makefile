# Builds the Wayland live wallpaper. Needs wayland-scanner and the dev packages for
# wayland-client, wayland-egl, wayland-protocols, EGL, OpenGL ES 2, cairo, and libcurl:
#   Debian/Ubuntu: libwayland-dev wayland-protocols libegl-dev libgles-dev libcairo2-dev libcurl4-openssl-dev
#   Fedora:        wayland-devel wayland-protocols-devel libglvnd-devel cairo-devel libcurl-devel
#   Arch:          wayland wayland-protocols libglvnd cairo curl
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
VERSION := $(shell cat ../VERSION)
OUT = ../build/wayland

PKGS = wayland-client wayland-egl egl glesv2 cairo
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
ifeq ($(CURL_LIBS),)
# libcurl's headers aren't installed: declare the few functions we call and link the runtime library.
CURL_CFLAGS = -DMESH_CURL_COMPAT
CURL_LIBS = -l:libcurl.so.4
endif

CFLAGS ?= -O2
CFLAGS += -std=gnu99 -Wall -Wextra -I../core -I$(OUT) -DMESH_VERSION='"$(VERSION)"' \
          $(shell pkg-config --cflags $(PKGS)) $(CURL_CFLAGS)
LDLIBS += $(shell pkg-config --libs $(PKGS)) $(CURL_LIBS) -lpthread -lm

SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || echo wayland-scanner)
WP := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
# wlr-layer-shell isn't in wayland-protocols, so its XML is kept here. It refers to xdg-shell's popups.
XML = protocols/wlr-layer-shell-unstable-v1.xml $(WP)/stable/xdg-shell/xdg-shell.xml \
      $(WP)/stable/viewporter/viewporter.xml $(WP)/staging/fractional-scale/fractional-scale-v1.xml
PROTO = $(basename $(notdir $(XML)))
PROTO_C = $(PROTO:%=$(OUT)/%-protocol.c)
PROTO_H = $(PROTO:%=$(OUT)/%-client-protocol.h)
vpath %.xml $(sort $(dir $(XML)))

SRC = ../core/mesh.c ../core/json.c ../core/meshmonitor.c mesh_wayland.c render_gl.c live.c hyprland.c

../build/mesh-wallpaper: $(SRC) $(PROTO_C) $(PROTO_H) ../core/*.h *.h
	$(CC) $(CFLAGS) $(SRC) $(PROTO_C) -o $@ $(LDFLAGS) $(LDLIBS)

$(OUT)/%-protocol.c: %.xml
	mkdir -p $(OUT)
	$(SCANNER) private-code $< $@

$(OUT)/%-client-protocol.h: %.xml
	mkdir -p $(OUT)
	$(SCANNER) client-header $< $@

install: ../build/mesh-wallpaper
	install -D -m 755 ../build/mesh-wallpaper $(DESTDIR)$(BINDIR)/mesh-wallpaper

TARBALL = screensaver-mesh-$(VERSION)-wayland-x86_64

tarball: ../build/mesh-wallpaper
	rm -rf ../build/$(TARBALL)
	mkdir -p ../build/$(TARBALL)
	cp ../build/mesh-wallpaper mesh-wallpaper.service install.sh config.example INSTALL.txt ../LICENSE \
	   ../build/$(TARBALL)/
	tar -C ../build -czf ../build/$(TARBALL).tar.gz $(TARBALL)

.PHONY: install tarball
.SECONDARY: $(PROTO_C) $(PROTO_H)
