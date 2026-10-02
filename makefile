RAYLIB := $(shell brew --prefix raylib)

compile: | build
	gcc -Wall -Werror -Wpedantic -o build/midi timer.c main.c -isystem . -I$(RAYLIB)/include -L$(RAYLIB)/lib -lraylib

build:
	mkdir -p build
