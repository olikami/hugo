# The library version to use.
# Keep in sync with libjxlVersion in ../jxl.go.
LIBJXL_VERSION = v0.12.0

# The path to the WebAssembly SDK. Can be overridden from the command line.
# e.g., make WASI_SDK_PATH=/path/to/wasi-sdk
# Download releases from https://github.com/WebAssembly/wasi-sdk/releases.
# Note that on MacOS, you may need to add some whitelistings in Privacy & Security > Security.
WASI_SDK_PATH ?= /opt/wasi-sdk

LIBJXL_SRC := $(PWD)/libjxl
BUILD_DIR := $(PWD)/build
BUILD_WASM := $(BUILD_DIR)/jxl.wasm
TARGET_WASM_DIR := ../wasm

# Set the compiler and toolchain for WASI
CC := $(WASI_SDK_PATH)/bin/clang
CXX := $(WASI_SDK_PATH)/bin/clang++
CMAKE_TOOLCHAIN_FILE := $(WASI_SDK_PATH)/share/cmake/wasi-sdk-p1.cmake

# Include paths
C_INCLUDES := -I$(LIBJXL_SRC)/lib/include -I$(BUILD_DIR)/lib/include -I.

# Libraries to link against
LD_LIBS := \
	$(BUILD_DIR)/lib/libjxl.a \
	$(BUILD_DIR)/lib/libjxl_cms.a \
	$(BUILD_DIR)/third_party/brotli/libbrotlienc.a \
	$(BUILD_DIR)/third_party/brotli/libbrotlidec.a \
	$(BUILD_DIR)/third_party/brotli/libbrotlicommon.a \
	$(BUILD_DIR)/third_party/highway/libhwy.a

# SIMD + non-trapping FP/int are part of wazero's CoreFeaturesV2.
# Highway has hand-tuned wasm-simd128 code paths.
LIB_CFLAGS := -msimd128 -mnontrapping-fptoint

# Compiler and linker flags for the WASM build
WASM_CFLAGS := -O3 $(LIB_CFLAGS) -Wall
WASM_LDFLAGS := \
	-mexec-model=command \
	-Wl,--export=malloc \
	-Wl,--export=free \
	-Wl,-z,stack-size=1048576 \
	-Wl,--strip-debug

.PHONY: all clean

all: $(LIBJXL_SRC) $(BUILD_DIR)
	@echo ">>> Configuring and building C++ libraries in $(BUILD_DIR)"
	cd $(BUILD_DIR) && \
	cmake $(LIBJXL_SRC) \
		-DCMAKE_TOOLCHAIN_FILE=$(CMAKE_TOOLCHAIN_FILE) \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=0 \
		-DBUILD_TESTING=0 \
		-DJPEGXL_STATIC=1 \
		-DJPEGXL_ENABLE_TOOLS=0 \
		-DJPEGXL_ENABLE_DOXYGEN=0 \
		-DJPEGXL_ENABLE_MANPAGES=0 \
		-DJPEGXL_ENABLE_BENCHMARK=0 \
		-DJPEGXL_ENABLE_EXAMPLES=0 \
		-DJPEGXL_ENABLE_JNI=0 \
		-DJPEGXL_ENABLE_SJPEG=0 \
		-DJPEGXL_ENABLE_OPENEXR=0 \
		-DJPEGXL_ENABLE_SKCMS=1 \
		-DJPEGXL_BUNDLE_LIBPNG=0 \
		-DJPEGXL_ENABLE_TRANSCODE_JPEG=0 \
		-DJPEGXL_ENABLE_WASM_THREADS=0 \
		-DJPEGXL_ENABLE_TCMALLOC=0 \
		-DCMAKE_C_FLAGS="$(LIB_CFLAGS)" \
		-DCMAKE_CXX_FLAGS="$(LIB_CFLAGS) -fno-exceptions" && \
	make jxl jxl_cms VERBOSE=1
	@echo ">>> Building WebAssembly binary: $(BUILD_WASM)"
	$(CC) $(WASM_CFLAGS) --sysroot=$(WASI_SDK_PATH)/share/wasi-sysroot $(C_INCLUDES) -c jxl.c -o $(BUILD_DIR)/jxl.o
	$(CC) $(WASM_CFLAGS) --sysroot=$(WASI_SDK_PATH)/share/wasi-sysroot -c ../deps/parson/parson.c -o $(BUILD_DIR)/parson.o
	$(CXX) $(WASM_CFLAGS) $(WASM_LDFLAGS) --sysroot=$(WASI_SDK_PATH)/share/wasi-sysroot -o $(BUILD_WASM) $(BUILD_DIR)/jxl.o $(BUILD_DIR)/parson.o $(LD_LIBS)
	@echo ">>> Moving the WebAssembly binary: $(TARGET_WASM_DIR)"
	mv $(BUILD_WASM) $(TARGET_WASM_DIR)

# Rule to create the build directory
$(BUILD_DIR):
	@echo ">>> Creating build directory: $(BUILD_DIR)"
	@mkdir -p $@

# Rule to clone the libjxl source code
$(LIBJXL_SRC):
	@echo ">>> Cloning libjxl source code (version: $(LIBJXL_VERSION))"
	git clone -b $(LIBJXL_VERSION) --depth 1 --recursive --shallow-submodules https://github.com/libjxl/libjxl.git $(LIBJXL_SRC)
	test -d $@

# Rule to clean up build artifacts
clean:
	@echo ">>> Cleaning build artifacts"
	@rm -rf $(BUILD_DIR)
	@rm -rf $(LIBJXL_SRC)
