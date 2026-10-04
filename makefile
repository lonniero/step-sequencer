RAYLIB := $(shell brew --prefix raylib)
PORTMIDI := $(shell brew --prefix portmidi)

SRC := main.c

compile: | build
	gcc -Wall -Werror -Wpedantic -o build/midi $(SRC) -isystem . \
		-I$(RAYLIB)/include -L$(RAYLIB)/lib -lraylib \
		-I$(PORTMIDI)/include -L$(PORTMIDI)/lib -lportmidi -lpthread

run: compile
	./build/midi

build:
	mkdir -p build
