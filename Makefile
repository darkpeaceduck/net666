

CXX ?= g++

CFLAGS=-Werror -std=c++14 -pthread 
LDFLAGS=-lrt

SRC_DIR=src
BIN_DIR=bin
OBJ_DIR=obj

BIN64=$(BIN_DIR)/bin64
BIN32=$(BIN_DIR)/bin32
BOOST_TEST=$(BIN_DIR)/tcp_test_boost

DEPS_SRC=\
	$(SRC_DIR)/tcp_engine.cpp\
	$(SRC_DIR)/transfer.cpp\
	$(SRC_DIR)/newimpl.cpp\
	
	
DEPS_OBJS=$(DEPS_SRC:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

BIN64_DEPS=
BIN32_DEPS=

BIN64_SRC=$(SRC_DIR)/test.cpp
BIN32_SRC=$(SRC_DIR)/test.cpp
BOOST_TEST_SRC=$(SRC_DIR)/tcp_test_boost.cpp

BIN64_OBJS=$(BIN64_SRC:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
BIN32_OBJS=$(BIN32_SRC:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
BOOST_TEST_OBJS=$(BOOST_TEST_SRC:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

all:$(BOOST_TEST)
	
$(BIN64):$(BIN_DIR) $(BIN64_OBJS) $(DEPS_OBJS)
	$(CXX) $(CFLAGS) $(BIN64_OBJS) $(DEPS_OBJS) $(LDFLAGS) -o $(BIN64)
	
$(BIN32):$(BIN_DIR) $(BIN32_OBJS) $(DEPS_OBJS)
	$(CXX) $(CFLAGS) -m32 $(BIN32_OBJS) $(DEPS_OBJS) $(LDFLAGS) -o $(BIN32)
	
$(BOOST_TEST):$(BIN_DIR)  $(DEPS_OBJS) $(BOOST_TEST_OBJS)
	$(CXX) $(CFLAGS) $(BOOST_TEST_OBJS) $(DEPS_OBJS) $(LDFLAGS) -lboost_system -lboost_unit_test_framework -o $(BOOST_TEST)
	


$(OBJ_DIR)/%.o:$(SRC_DIR)/%.cpp
	$(CXX) -c $(CFLAGS) $< -o $@
	$(CXX) -MM -MT $@ -MF $(OBJ_DIR)/$*.d $<


-include $(DEPS_OBJS:.o=.d)
-include $(BIN64_OBJS:.o=.d)
-include $(BIN32_OBJS:.o=.d)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)
	mkdir -p $(OBJ_DIR)
	
clean:
	rm -rf $(BIN_DIR) $(OBJ_DIR)
	
.PHONY:clean
	

	
