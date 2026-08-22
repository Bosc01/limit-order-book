# Convenience wrapper; CMakeLists.txt is the build system of record
# (flag rationale lives there).
BUILD := build

.PHONY: all test bench smoke san clean

all:
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j

test: all
	./$(BUILD)/lob_tests

bench: all
	./$(BUILD)/bench final

smoke: all
	./scripts/net_smoke.sh $(BUILD)

san:
	cmake -B build-san -DLOB_SANITIZE=address,undefined
	cmake --build build-san --target lob_tests -j
	./build-san/lob_tests

clean:
	rm -rf $(BUILD) build-san
