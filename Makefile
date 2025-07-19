# Makefile for ffmpeg-tutorial

# List of tutorial directories and C files
tutorials = tutorial01 tutorial02 tutorial03 tutorial04 tutorial05 tutorial06 tutorial07

# Compiler and flags
CC = clang
CFLAGS = -g \
	-I/opt/homebrew/Cellar/ffmpeg/7.1_3/include \
	-I/opt/homebrew/Cellar/sdl2/2.30.8/include
LDFLAGS = \
	-L/opt/homebrew/Cellar/ffmpeg/7.1_3/lib \
	-L/opt/homebrew/Cellar/sdl2/2.30.8/lib \
	-lavcodec -lavformat -lavutil -lswscale -lavdevice -lswresample -lsdl2 -lm -lz

# Default target: build all tutorials
all: $(tutorials)

# Pattern rule to build each tutorial
$(tutorials):
	$(CC) $(CFLAGS) $@/$@.c -o $@/$@ $(LDFLAGS)

# Clean up all built binaries
clean:
	rm -f $(foreach t,$(tutorials),$t/$t)

.PHONY: all clean $(tutorials)
