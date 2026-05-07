CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -g -I../../src -D_POSIX_C_SOURCE=200809L
LDFLAGS = -L../../build -lmot -lpthread -lsqlite3

SERVER = server
LIB = ../../build/libmot.a

.PHONY: all clean run lib

all: $(SERVER)

lib:
	$(MAKE) -C ../.. lib

$(SERVER): server.c $(LIB)
	$(CC) $(CFLAGS) -o $@ server.c $(LDFLAGS)

$(LIB):
	$(MAKE) -C ../.. lib

run: $(SERVER)
	ORIGIN_PORT=$${ORIGIN_PORT:-8080} ./$(SERVER)

clean:
	rm -f $(SERVER)
