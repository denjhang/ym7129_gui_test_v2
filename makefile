# YM7129 Drum Pad GUI Makefile
CC      = gcc
CFLAGS  = -Wall -O2 -I. -mwindows
LDFLAGS = -L. -lftd2xx -lws2_32 -lgdi32 -mwindows

OBJS = drum_gui.o ym7129.o spfm_lite.o error_lite.o
TARGET = drum_gui.exe

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	del /Q *.o $(TARGET) 2>/dev/null

.PHONY: all clean
