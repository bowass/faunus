
CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -pthread -O2 -g -I. -Isrc -Iexternals/skiplist/include
LDFLAGS := -lyaml-cpp

YAMLCPP ?= $(shell spack location -i yaml-cpp 2>/dev/null)

ifneq ($(wildcard $(YAMLCPP)),)
CXXFLAGS += -I$(YAMLCPP)/include
LDFLAGS  += -L$(firstword $(wildcard $(YAMLCPP)/lib64 $(YAMLCPP)/lib))
endif

ifdef KEY_SIZE
CXXFLAGS += -DKEY_SIZE=$(KEY_SIZE)
endif
ifdef VALUE_SIZE
CXXFLAGS += -DVALUE_SIZE=$(VALUE_SIZE)
endif
ifdef FAUNUS_BRANCH_FACTOR
CXXFLAGS += -DFAUNUS_BRANCH_FACTOR=$(FAUNUS_BRANCH_FACTOR)
endif
ifdef FAUNUS_SPLIT_WATERMARK
CXXFLAGS += -DFAUNUS_SPLIT_WATERMARK=$(FAUNUS_SPLIT_WATERMARK)
endif
ifdef FAUNUS_MAINTENANCE_ENABLED
ifneq ($(FAUNUS_MAINTENANCE_ENABLED),0)
CXXFLAGS += -DFAUNUS_MAINTENANCE_ENABLED
endif
endif

SRC_ROOT := src
BUILD_ROOT := build

CORE_DIRS := rdma kv_index util cache
CORE_SRCS := $(foreach dir,$(CORE_DIRS),$(wildcard $(SRC_ROOT)/$(dir)/*.cpp))
CORE_SRCS := $(filter-out $(SRC_ROOT)/cache/bench.cpp,$(CORE_SRCS))
CORE_OBJS := $(patsubst $(SRC_ROOT)/%.cpp,$(BUILD_ROOT)/%.o,$(CORE_SRCS))

APP_SRCS := $(SRC_ROOT)/apps/kv_test.cpp
APP_OBJS := $(patsubst $(SRC_ROOT)/apps/%.cpp,$(BUILD_ROOT)/apps/%.o,$(APP_SRCS))

BENCH_SRCS := $(SRC_ROOT)/cache/bench.cpp
BENCH_OBJS := $(patsubst $(SRC_ROOT)/cache/%.cpp,$(BUILD_ROOT)/cache/%.o,$(BENCH_SRCS))

THIRD_PARTY_SRCS := $(wildcard externals/skiplist/src/*.cc)
ifeq ($(THIRD_PARTY_SRCS),)
$(error Skiplist submodule not initialized. Run `git submodule update --init --recursive`.)
endif
THIRD_PARTY_OBJS := $(patsubst externals/skiplist/%.cc,$(BUILD_ROOT)/externals/skiplist/%.o,$(THIRD_PARTY_SRCS))

LIB_OBJS := $(CORE_OBJS) $(THIRD_PARTY_OBJS)

TARGETS := kv_test cache_bench

all: $(TARGETS)

kv_test: $(APP_OBJS) $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

cache_bench: $(BENCH_OBJS) $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_ROOT)/%.o: $(SRC_ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_ROOT)/externals/skiplist/%.o: externals/skiplist/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_ROOT) $(TARGETS)

.PHONY: all clean
