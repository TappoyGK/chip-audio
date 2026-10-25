CC ?= cc
AR ?= ar
BUILD_DIR ?= build
CPPFLAGS += -Iinclude
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-function
LDLIBS += -lm
SOURCES := src/chip-audio.c src/chip-drums.c
HEADERS := $(wildcard include/*.h src/*.h)
OBJECTS := $(BUILD_DIR)/chip-audio.o $(BUILD_DIR)/chip-drums.o

.PHONY: all test clean
all: $(BUILD_DIR)/libchip-audio.a
$(BUILD_DIR):
	mkdir -p $@
$(BUILD_DIR)/%.o: src/%.c $(HEADERS) Makefile | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/libchip-audio.a: $(OBJECTS)
	$(AR) rcs $@ $^
$(BUILD_DIR)/audio-test: tests/audio-test.c $(BUILD_DIR)/libchip-audio.a $(HEADERS) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $< $(BUILD_DIR)/libchip-audio.a $(LDLIBS) -o $@
test: $(BUILD_DIR)/audio-test
	$(BUILD_DIR)/audio-test
clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/waveform-test: tests/waveform-test.c $(HEADERS) Makefile | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffp-contract=off -UNDEBUG $< $(LDLIBS) -o $@
$(BUILD_DIR)/cxx-link-test: tests/cxx-link-test.cpp $(BUILD_DIR)/libchip-audio.a $(HEADERS) Makefile
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -UNDEBUG $< $(BUILD_DIR)/libchip-audio.a $(LDLIBS) -o $@
.PHONY: test-interfaces
test: test-interfaces
test-interfaces: $(BUILD_DIR)/waveform-test $(BUILD_DIR)/cxx-link-test
	$(BUILD_DIR)/waveform-test
	$(BUILD_DIR)/cxx-link-test
$(BUILD_DIR)/echo-test: tests/echo-test.c $(BUILD_DIR)/libchip-audio.a $(HEADERS) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $< $(BUILD_DIR)/libchip-audio.a $(LDLIBS) -o $@
.PHONY: test-echo
test: test-echo
test-echo: $(BUILD_DIR)/echo-test
	$(BUILD_DIR)/echo-test

$(BUILD_DIR)/contract-test: tests/contract-test.c $(BUILD_DIR)/libchip-audio.a $(HEADERS) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $< $(BUILD_DIR)/libchip-audio.a $(LDLIBS) -o $@
$(BUILD_DIR)/echo-plan-test: tests/echo-plan-test.c $(HEADERS) Makefile | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $< $(LDLIBS) -o $@
.PHONY: test-contracts
test: test-contracts
test-contracts: $(BUILD_DIR)/contract-test $(BUILD_DIR)/echo-plan-test
	$(BUILD_DIR)/contract-test
	$(BUILD_DIR)/echo-plan-test
