# Сборка: make            — все программы в build/
#         make asan       — сборка с AddressSanitizer/LeakSanitizer/UBSan в build-asan/
#         make tsan       — сборка с ThreadSanitizer в build-tsan/
#         make valgrind   — запуск сервера под valgrind (Ctrl+C для остановки)
CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wvla
LDLIBS  += -pthread

BUILD ?= build
PROGS := client server loadtest
BINS  := $(addprefix $(BUILD)/,$(PROGS))

.PHONY: all clean asan tsan valgrind load
all: $(BINS)

$(BUILD)/%: %.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

$(BUILD):
	mkdir -p $@

asan:
	$(MAKE) BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

tsan:
	$(MAKE) BUILD=build-tsan CFLAGS="-O1 -g -fsanitize=thread"

valgrind: $(BUILD)/server
	valgrind --trace-children=yes --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=1 $(BUILD)/server -q

load: $(BUILD)/loadtest
	$(BUILD)/loadtest -n 1000 -c 1000 -S 5

clean:
	rm -rf build build-asan build-tsan
