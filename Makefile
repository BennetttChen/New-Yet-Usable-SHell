CC=gcc
CFLAGS=-Wall -Wextra -std=gnu17 -g
TARGET=nyush

all: $(TARGET)

$(TARGET): nyush.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET) *.o
