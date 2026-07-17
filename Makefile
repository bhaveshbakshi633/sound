CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -march=native -ffast-math -Wall -Wextra -Wpedantic
LDLIBS   := -lasound -lpthread -lssl -lcrypto

SRC := src/dsp.cpp src/rs.cpp src/audio.cpp src/http.cpp src/main.cpp
OBJ := $(SRC:.cpp=.o)

all: uchat

uchat: $(OBJ)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

selftest: src/selftest.cpp src/dsp.cpp src/rs.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/dsp.o src/audio.o src/http.o src/main.o: src/dsp.h
src/audio.o src/main.o: src/audio.h
src/http.o src/main.o: src/http.h

clean:
	rm -f $(OBJ) uchat selftest

.PHONY: all clean selftest tool

tool: src/tool.cpp src/dsp.cpp src/rs.cpp
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

selftest: LDLIBS+=-lpthread

# Browser build. Same dsp.cpp/rs.cpp as the native binary — see BENCHMARKS.md §5 for the
# bit-for-bit comparison proving the two do not drift.
EMCC ?= emcc
EMFLAGS := -O3 -std=c++17 \
  -s EXPORTED_FUNCTIONS='["_uc_modulate","_uc_push","_uc_pop","_uc_last_score","_uc_band_energy","_uc_sample_rate","_uc_max_payload","_uc_n_tones","_uc_tone_hz","_malloc","_free"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","getValue","UTF8ToString","stringToUTF8","HEAPF32","HEAPU8","HEAP32"]' \
  -s ALLOW_MEMORY_GROWTH=1 -s MODULARIZE=1 -s EXPORT_NAME=UChatDSP \
  -s ENVIRONMENT=web,worker,node

wasm: web/uchat.js

web/uchat.js: src/wasm.cpp src/dsp.cpp src/rs.cpp src/dsp.h src/rs.h
	$(EMCC) $(EMFLAGS) src/wasm.cpp src/dsp.cpp src/rs.cpp -o $@

.PHONY: wasm
