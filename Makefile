CC=clang
CFLAGS=-Wall -g

SRCS = $(wildcard *.c)

all: $(SRCS:.c=)

clean:
	rm $(SRCS:.c=)
