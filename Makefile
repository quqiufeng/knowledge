CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
PG_INC := $(shell pg_config --includedir 2>/dev/null)
ifeq ($(PG_INC),)
PG_INC := /usr/include/postgresql
endif
CPPFLAGS += -I$(PG_INC) -Ithird_party

LDLIBS := -ljansson -lpq -lpthread

BIN := knowledge
SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:.cpp=.o)

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

test: $(BIN)
	./tests/smoke.sh

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean test
