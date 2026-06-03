# YM7129 Drum Pad GUI Makefile (ImGui + DX11)
CC      = g++
CFLAGS  = -Wall -O2 -I. -Iimgui -mwindows
LDFLAGS = -L. -lftd2xx -lws2_32 -ld3d11 -ldxgi -ld3dcompiler -mwindows

IMGUI_SRCS = imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp \
             imgui/imgui_widgets.cpp imgui/imgui_impl_win32.cpp imgui/imgui_impl_dx11.cpp
IMGUI_OBJS = $(IMGUI_SRCS:.cpp=.o)

OBJS = drum_gui.o ym7129.o spfm_lite.o error_lite.o $(IMGUI_OBJS)
TARGET = drum_gui.exe

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

imgui/%.o: imgui/%.cpp
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	del /Q *.o $(TARGET) imgui\*.o 2>/dev/null

.PHONY: all clean
