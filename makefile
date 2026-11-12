# YM7129 Drum Pad GUI Makefile (ImGui + DX11)
BUILDDIR = build
CC       = g++
CFLAGS   = -Wall -O2 -I. -Iimgui -mwindows
LDFLAGS  = -L. -lftd2xx -lws2_32 -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -mwindows -static

IMGUI_SRCS = imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp \
             imgui/imgui_widgets.cpp imgui/imgui_impl_win32.cpp imgui/imgui_impl_dx11.cpp
IMGUI_OBJS = $(patsubst %.cpp,$(BUILDDIR)/%.o,$(IMGUI_SRCS))

OBJS = $(BUILDDIR)/drum_gui.o $(BUILDDIR)/ym7129.o $(BUILDDIR)/spfm_lite.o $(BUILDDIR)/error_lite.o $(IMGUI_OBJS)
TARGET = $(BUILDDIR)/drum_gui.exe

all: ensure_dirs $(TARGET)

ensure_dirs:
	@mkdir -p $(BUILDDIR)/imgui 2>/dev/null; test -d $(BUILDDIR)/imgui || mkdir $(BUILDDIR) $(BUILDDIR)/imgui

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@if command -v objdump >/dev/null 2>&1; then \
		bad=$$(objdump -p $(TARGET) | grep -i "DLL Name" \
			| grep -iE "libgcc_|libstdc\+\+|libwinpthread|libssp-"); \
		if [ -n "$$bad" ]; then \
			echo ""; \
			echo "ERROR: $(TARGET) imports MinGW runtime DLLs and will fail on machines without MinGW:"; \
			echo "$$bad" | sed 's/^/  /'; \
			echo "Add -static to LDFLAGS."; \
			exit 1; \
		fi; \
	fi

$(BUILDDIR)/%.o: %.cpp
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/%.o: imgui/%.cpp
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	del /Q $(BUILDDIR)\\*.o $(TARGET) 2>/dev/null

.PHONY: all clean
