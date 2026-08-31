# Makefile for ah_parser
#
# Usage:
#   make            # builds ./ah_parser
#   make run        # builds (if needed) and runs it against ./scans
#   make clean      # removes the built binary and auctionsim.dat

CXX      := g++
CXXFLAGS := -O2 -std=c++17 -Wall
LDFLAGS  := -pthread

TARGET  := compile-data
SRC     := compile-data.cpp

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) auctionsim.dat