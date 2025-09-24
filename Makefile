
CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -pthread -Iinclude -Irdma

SRC_DIR = src
RDMA_DIR = rdma
SRC = $(SRC_DIR)/main.cpp \
      $(RDMA_DIR)/compute_server.cpp \
      $(RDMA_DIR)/memory_server.cpp \
      $(RDMA_DIR)/rdma_simulation.cpp \
      $(RDMA_DIR)/rdma_manager.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = faunus_sim

KV_SRC = $(SRC_DIR)/kv_test.cpp \
         $(RDMA_DIR)/compute_server.cpp \
         $(RDMA_DIR)/memory_server.cpp \
         $(RDMA_DIR)/rdma_simulation.cpp \
         $(RDMA_DIR)/rdma_manager.cpp
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
	rm -f $(SRC_DIR)/*.o $(RDMA_DIR)/*.o $(TARGET) $(KV_TARGET)

.PHONY: all clean
