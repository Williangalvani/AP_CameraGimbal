# Host build of apcamsrc and its test feed; build_musl.sh cross-builds the
# plugin for the camera. `make test` runs test_apcamsrc.py.
CAMERA_APP := ../../../camera_app
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-missing-field-initializers -Werror
GST := $(shell pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0)

all: build/libgstapcam.so build/test_feed

build/libgstapcam.so: gstapcamsrc.c
	mkdir -p build
	$(CC) $(CFLAGS) -std=gnu11 -fPIC -shared -o $@ $< $(GST)

build/test_feed: test_feed.cpp $(CAMERA_APP)/src/streaming/frame_socket.cpp \
                 $(CAMERA_APP)/include/camera_app/frame_socket.h $(CAMERA_APP)/src/log.cpp
	mkdir -p build
	$(CXX) -O2 -g -Wall -Wextra -std=gnu++17 -I$(CAMERA_APP)/include -I$(CAMERA_APP)/../include \
		-o $@ $(filter %.cpp,$^) -lpthread

build/test.h264:
	mkdir -p build
	ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=1280x720:rate=30 -t 4 \
		-c:v libx264 -g 30 -bf 0 -x264-params aud=1 -pix_fmt yuv420p -f h264 -y $@

test: all build/test.h264
	python3 test_apcamsrc.py

clean:
	rm -rf build

.PHONY: all test clean
