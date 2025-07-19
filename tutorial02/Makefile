# Makefile for tutorial02

CC = clang
CFLAGS = -g \
	-I/opt/homebrew/Cellar/ffmpeg/7.1_3/include \
	-I/opt/homebrew/Cellar/sdl2/2.30.8/include \
	-I/opt/homebrew/Cellar/sdl2/2.30.8/include/SDL2
LDFLAGS = \
	-L/opt/homebrew/Cellar/ffmpeg/7.1_3/lib \
	-L/opt/homebrew/Cellar/sdl2/2.30.8/lib \
	-lavcodec -lavformat -lavutil -lswscale -lavdevice -lswresample -lsdl2 -lm -lz

TARGET = tutorial02
SRC = $(TARGET).c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
