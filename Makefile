
CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -pthread -Iinclude -Irdma -g

# Compile-time configuration flags (can be overridden on command line)
# Example: make FAUNUS_BRANCH_FACTOR=128 KEY_SIZE=16
# - KEY_SIZE: Key size in bytes (default: 8)
# - VALUE_SIZE: Value size in bytes (default: 8)
# - FAUNUS_BRANCH_FACTOR: Branch factor for Faunus B+Tree (default: 64)
# - FAUNUS_SPLIT_WATERMARK: Split threshold 0.0-1.0 (default: 0.75)
# - FAUNUS_MAINTENANCE_ENABLED: Enable background maintenance thread for Faunus B+Tree (default: disabled)
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

SRC_DIR = src
RDMA_DIR = rdma
KV_DIR = kv_index
UTIL_DIR = util
SRC = $(SRC_DIR)/main.cpp \
      $(RDMA_DIR)/compute_server.cpp \
      $(RDMA_DIR)/memory_server.cpp \
      $(RDMA_DIR)/rdma_simulation.cpp \
      $(RDMA_DIR)/rdma_manager.cpp \
      $(UTIL_DIR)/profiler.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = faunus_sim

SKIPLIST_DIR = externals/skiplist
SKIPLIST_INCLUDE = -I$(SKIPLIST_DIR)/include
CXXFLAGS += $(SKIPLIST_INCLUDE)
SKIPLIST_SRC = $(SKIPLIST_DIR)/src/skiplist.cc

KV_SRC = $(SRC_DIR)/kv_test.cpp \
         $(RDMA_DIR)/compute_server.cpp \
         $(RDMA_DIR)/memory_server.cpp \
         $(RDMA_DIR)/rdma_simulation.cpp \
         $(RDMA_DIR)/rdma_manager.cpp \
         $(KV_DIR)/sherman_index.cpp \
         $(KV_DIR)/faunus_index.cpp \
         $(UTIL_DIR)/profiler.cpp \
         $(UTIL_DIR)/cpu_affinity.cpp \
         $(SKIPLIST_SRC)
KV_OBJ = $(KV_SRC:.cpp=.o)
KV_TARGET = kv_test

all: $(TARGET) $(KV_TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) -lyaml-cpp

$(KV_TARGET): $(KV_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(KV_OBJ) -lyaml-cpp

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(UTIL_DIR)/*.o $(SRC_DIR)/*.o $(RDMA_DIR)/*.o $(TARGET) $(KV_DIR)/*.o $(KV_TARGET)

.PHONY: all clean
