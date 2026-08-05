# libs/AssetStoreLib -- MatterStore, the asset storage subsystem.
#
# Builds build/libasset_store.a. Depends only on libs/MemoryLib, per the house
# rule in CLAUDE.md: consumers add -I to the sibling's include/ and compile the
# sibling's sources from where they live. Nothing is copied.
#
# The archive deliberately does NOT contain MemoryLib's objects. A consumer that
# already compiles mem_arena.c (MatterEngine3 and MatterEditor both do) would
# otherwise get duplicate symbols. Consumers link this .a and keep compiling
# MemoryLib themselves; tests/Makefile does exactly that.

CXX = g++
AR = ar

MEMLIB_DIR = ../MemoryLib

CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -Iinclude -I$(MEMLIB_DIR)/include

SRC_DIR = src
OBJ_DIR = build/obj

SRCS = $(SRC_DIR)/store_hash.cpp $(SRC_DIR)/store_os.cpp \
       $(SRC_DIR)/blob_store.cpp $(SRC_DIR)/ref_table.cpp
OBJS = $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

TARGET = build/libasset_store.a

all: $(TARGET)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

test: $(TARGET)
	$(MAKE) -C tests test

bench: $(TARGET)
	$(MAKE) -C tests bench

clean:
	rm -rf build
	$(MAKE) -C tests clean

-include $(OBJS:.o=.d)

.PHONY: all test bench clean
