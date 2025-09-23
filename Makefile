CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -pthread -Iinclude

SRC_DIR = src/
INC_DIR = include/
SRC = $(SRC_DIR)main.cpp $(SRC_DIR)compute_server.cpp $(SRC_DIR)memory_server.cpp $(SRC_DIR)rdma_simulation.cpp $(SRC_DIR)rdma_manager.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = faunus_sim

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ)

$(SRC_DIR)%.o: $(SRC_DIR)%.cpp $(INC_DIR)%.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(SRC_DIR)*.o $(TARGET)

.PHONY: all clean
