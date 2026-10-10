RAYLIB := $(shell brew --prefix raylib)
PORTMIDI := $(shell brew --prefix portmidi)

SRC := main.c midi.c

DEPS := $(abspath build/deps)
DEPLOYMENT_TARGET := 14.0
RAYLIB_VERSION := 6.0
PORTMIDI_VERSION := v2.0.7

compile: | build
	gcc -Wall -Werror -Wpedantic -o build/midi $(SRC) -isystem . \
		-I$(RAYLIB)/include -L$(RAYLIB)/lib -lraylib \
		-I$(PORTMIDI)/include -L$(PORTMIDI)/lib -lportmidi -lpthread

release: deps | build
	gcc -Wall -Werror -Wpedantic -O2 -mmacosx-version-min=$(DEPLOYMENT_TARGET) -o build/midi $(SRC) -isystem . \
		-I$(DEPS)/include $(DEPS)/lib/libraylib.a $(DEPS)/lib/libportmidi.a \
		-framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL \
		-framework CoreMIDI -framework CoreAudio -framework CoreFoundation -framework CoreServices \
		-lpthread

deps: $(DEPS)/lib/libraylib.a $(DEPS)/lib/libportmidi.a

$(DEPS)/lib/libraylib.a:
	rm -rf build/src/raylib
	git clone --depth 1 --branch $(RAYLIB_VERSION) https://github.com/raysan5/raylib.git build/src/raylib
	cmake -S build/src/raylib -B build/src/raylib/build \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=$(DEPLOYMENT_TARGET) -DCMAKE_INSTALL_PREFIX=$(DEPS) \
		-DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF
	cmake --build build/src/raylib/build --target install

$(DEPS)/lib/libportmidi.a:
	rm -rf build/src/portmidi
	git clone --depth 1 --branch $(PORTMIDI_VERSION) https://github.com/PortMidi/portmidi.git build/src/portmidi
	cmake -S build/src/portmidi -B build/src/portmidi/build \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=$(DEPLOYMENT_TARGET) -DCMAKE_INSTALL_PREFIX=$(DEPS) \
		-DBUILD_SHARED_LIBS=OFF
	cmake --build build/src/portmidi/build --target install

run: compile
	./build/midi

build:
	mkdir -p build

format:
	clang-format -i *.c *.h

.PHONY: compile release deps run format
