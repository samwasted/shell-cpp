# compiler and Flags
CXX = g++
CXXFLAGS = -std=c++17 -Iinclude -Wall
LDFLAGS = -lreadline

# directories
SRC_DIR = src
OBJ_DIR = obj
INC_DIR = include

# files
SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS = $(SOURCES:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
TARGET = myshell

# default target
all: $(TARGET)

# link the executable
$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

# compile source files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# clean build files
clean:
	rm -rf $(OBJ_DIR) $(TARGET)

.PHONY: all clean