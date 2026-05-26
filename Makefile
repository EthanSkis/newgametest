CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
TARGET   = dungeon

all: $(TARGET)

$(TARGET): game.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
