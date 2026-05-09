BUILD_DIR ?= build
CONFIG ?= Release

.PHONY: all configure build test clean

all: build

configure:
	cmake -S . -B $(BUILD_DIR)

build: configure
	cmake --build $(BUILD_DIR) --config $(CONFIG)

test: build
	ctest --test-dir $(BUILD_DIR) -C $(CONFIG) --output-on-failure

clean:
	-cmake --build $(BUILD_DIR) --config $(CONFIG) --target clean
