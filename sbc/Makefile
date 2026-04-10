# Makefile for ICU C++ extensions

CXX = g++
CXXFLAGS = -O3 -shared -fPIC
SRC_DIR = modules/src
OUT_DIR = modules/src

all: $(OUT_DIR)/libcrc16.so $(OUT_DIR)/libmetadata_injector.so

$(OUT_DIR)/libcrc16.so: $(SRC_DIR)/crc16.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

$(OUT_DIR)/libmetadata_injector.so: $(SRC_DIR)/metadata_injector.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ -lexif

clean:
	rm -f $(OUT_DIR)/*.so
