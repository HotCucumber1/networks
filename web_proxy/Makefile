CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude
LDFLAGS ?=
LDLIBS ?= -pthread

TARGET := proxy_server
BUILD_DIR := build
SOURCES := \
	src/main.cpp \
	src/HttpParser.cpp \
	src/SocketUtils.cpp \
	src/CacheManager.cpp \
	src/ThreadPool.cpp \
	src/Logger.cpp \
	src/ProxyStatistics.cpp \
	src/HttpProxyServer.cpp
OBJECTS := $(SOURCES:src/%.cpp=$(BUILD_DIR)/%.o)
DEPS := $(OBJECTS:.o=.d)

.PHONY: all clean run test

all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: all
	./$(BUILD_DIR)/$(TARGET) 8888 8 proxy_cache

test: all
	python3 tests/integration_test.py

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
