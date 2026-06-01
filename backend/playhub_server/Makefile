CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -pthread -Wall -Wextra -DNDEBUG -DCPPHTTPLIB_OPENSSL_SUPPORT
LDLIBS ?= -lssl -lcrypto
TARGET := playhub_server
SRCS := main.cpp

all: $(TARGET)

$(TARGET): $(SRCS) httplib.h json.hpp
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
