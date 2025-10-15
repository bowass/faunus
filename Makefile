
CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -pthread -Iinclude -Irdma -g

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

KV_SRC = $(SRC_DIR)/kv_test.cpp \
         $(RDMA_DIR)/compute_server.cpp \
         $(RDMA_DIR)/memory_server.cpp \
         $(RDMA_DIR)/rdma_simulation.cpp \
         $(RDMA_DIR)/rdma_manager.cpp \
         $(KV_DIR)/faunus_index.cpp \
         $(KV_DIR)/index_cache.cpp \
         $(UTIL_DIR)/profiler.cpp \
         $(UTIL_DIR)/cpu_affinity.cpp
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
