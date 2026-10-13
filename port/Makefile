CC      = clang
CXXFLAGS = -std=c++17 -O2 -Wall $(shell pkg-config --cflags adplug)
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags unicorn sdl2)
LDFLAGS = $(shell pkg-config --libs unicorn sdl2 adplug) -lc++
SRC     = $(wildcard src/*.c)
OBJ     = $(SRC:src/%.c=build/%.o) build/opl.o

build/bifi2: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c src/host.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/opl.o: src/opl.cpp src/opl.h | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: clean
