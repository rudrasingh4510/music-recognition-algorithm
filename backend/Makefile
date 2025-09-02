# Build instructions for backend
CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
INCLUDES ?= -I/opt/homebrew/include -I/usr/local/include
LDFLAGS  ?= -L/opt/homebrew/lib -L/usr/local/lib
LIBS     ?= -lfftw3 -lsndfile -lm

SRCS = server.cpp engine.cpp
OBJS = $(SRCS:.cpp=.o)

all: server

server: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) server
