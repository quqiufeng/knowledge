CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -MMD -MP
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

# Unoptimized build with debug info (for gdb).
debug:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="-std=c++17 -O0 -g -Wall -Wextra -MMD -MP" all

# ASan+UBSan build (memory/UB regression runs).
asan:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -MMD -MP" \
	        LDLIBS="-ljansson -lpq -lpthread -fsanitize=address,undefined" all

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(BIN)

-include $(OBJ:.o=.d)

.PHONY: all clean test debug asan
