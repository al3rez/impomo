EXE = pomodoro
IMGUI_DIR = ../imgui

SOURCES  = main.cpp
SOURCES += $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
SOURCES += $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

OBJS = $(addsuffix .o, $(basename $(notdir $(SOURCES))))

CXX ?= g++
CXXFLAGS  = -std=c++11 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends
CXXFLAGS += -O2 -Wall -Wformat

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)
	CXXFLAGS += `pkg-config --cflags glfw3`
	LIBS      = -lGL `pkg-config --static --libs glfw3`
endif

ifeq ($(UNAME_S), Darwin)
	CXXFLAGS += -I/usr/local/include -I/opt/local/include -I/opt/homebrew/include
	LIBS      = -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
	LIBS     += -L/usr/local/lib -L/opt/local/lib -L/opt/homebrew/lib -lglfw
endif

ifeq ($(OS), Windows_NT)
	EXE      := $(EXE).exe
	CXXFLAGS += `pkg-config --cflags glfw3`
	# -static pulls libgcc/libstdc++/libwinpthread into the .exe so it runs on a
	# plain Windows box without MSYS2 DLLs.
	LIBS      = -static `pkg-config --static --libs glfw3` -lopengl32 -lgdi32 -limm32
endif

%.o:%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o:$(IMGUI_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o:$(IMGUI_DIR)/backends/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

all: $(EXE)

$(EXE): $(OBJS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LIBS)

clean:
	rm -f $(EXE) $(OBJS)
