# Makefile for flexnetd

CC      = gcc
PREFIX  = /usr/local
SBINDIR = $(PREFIX)/sbin
CONFDIR = $(PREFIX)/etc/ax25

SRCS    = flexnetd.c config.c log.c util.c ax25sock.c \
          cf_proto.c ce_proto.c dtable.c output.c poll_cycle.c

TARGET  = flexnetd
CFLAGS_COMMON = -Wall -Wextra -Wshadow -Wformat=2 -std=c11 -D_GNU_SOURCE -I.
LDFLAGS = -lax25

all: $(TARGET)

$(TARGET): $(SRCS) flexnetd.h
	$(CC) $(CFLAGS_COMMON) -O2 -o $@ $(SRCS) $(LDFLAGS)
	@echo "Built $(TARGET) (release)" && ls -lh $(TARGET)

# Debug: symbols, no sanitisers — safe to run with sudo/root on aarch64
debug: $(SRCS) flexnetd.h
	$(CC) $(CFLAGS_COMMON) -g3 -O0 -DDEBUG -o $(TARGET)_debug $(SRCS) $(LDFLAGS)
	@echo "Built $(TARGET)_debug" && ls -lh $(TARGET)_debug

# ASan: memory checking — run WITHOUT sudo
asan: $(SRCS) flexnetd.h
	$(CC) $(CFLAGS_COMMON) -g3 -O0 -DDEBUG \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(TARGET)_asan $(SRCS) $(LDFLAGS) -fsanitize=address,undefined
	@echo "Built $(TARGET)_asan (run WITHOUT sudo)" && ls -lh $(TARGET)_asan

syntax: $(SRCS)
	@for src in $(SRCS); do \
	    printf "  %-20s ... " "$$src"; \
	    $(CC) $(CFLAGS_COMMON) -g3 -O0 -c -o /dev/null $$src \
	        && echo "OK" || echo "FAILED"; \
	done

check-deps:
	@echo "Checking build dependencies..."
	@pkg-config --exists libax25 \
	    && echo "  libax25: OK" \
	    || echo "  libax25: MISSING — run: apt install libax25-dev"
	@test -f /usr/include/netax25/axconfig.h \
	    && echo "  axconfig.h: OK" \
	    || echo "  axconfig.h: MISSING (part of libax25-dev)"

install: $(TARGET)
	install -m 755 $(TARGET) $(SBINDIR)/$(TARGET)
	@echo "Installed $(SBINDIR)/$(TARGET)"
	@if [ ! -f $(CONFDIR)/flexnetd.conf ]; then \
	    install -m 644 ../flexnetd.conf $(CONFDIR)/flexnetd.conf; \
	    echo "Installed $(CONFDIR)/flexnetd.conf"; \
	else echo "Skipped $(CONFDIR)/flexnetd.conf (exists)"; fi

clean:
	rm -f $(TARGET) $(TARGET)_debug $(TARGET)_asan *.o
	@echo "Cleaned"

.PHONY: all debug asan syntax check-deps install clean
