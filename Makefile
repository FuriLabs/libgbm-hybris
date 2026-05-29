CC = gcc

CFLAGS = -O2 -fPIC -I/usr/include/android `pkg-config --cflags glib-2.0`
LDFLAGS = -shared `pkg-config --libs glib-2.0 libgralloc`

SOURCES = src/gbm_hybris.c

TARGET = hybris_gbm.so

PREFIX ?= /usr
TRIPLET ?= $(shell $(CC) -dumpmachine)

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(SOURCES) -o $(TARGET) $(CFLAGS) $(LDFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/lib/$(TRIPLET)/gbm/
	install -m 0644 $(TARGET) $(DESTDIR)$(PREFIX)/lib/$(TRIPLET)/gbm/

clean:
	rm -f $(TARGET)
