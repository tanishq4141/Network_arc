CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2

server: server.cpp
	$(CXX) $(CXXFLAGS) -o server server.cpp

clean:
	rm -f server

.PHONY: clean
