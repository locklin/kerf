CXX = clang++
CXXFLAGS = -std=c++2b -pthread -O0 -DDEBUG -g -fsanitize=address -ferror-limit=5
LDFLAGS = -lgtest -lgtest_main -ledit -levent

UNAME := $(shell uname)

ifeq ($(UNAME),Linux)
  # Find a gcc install dir that has C++ headers
  GCC_DIR := $(shell for d in $$(ls -rd /usr/lib/gcc/x86_64-linux-gnu/1* 2>/dev/null); do \
    test -d "$$d/../../../../include/x86_64-linux-gnu/c++/$$(basename $$d)" && echo "$$d" && break; \
  done)
  ifneq ($(GCC_DIR),)
    CXXFLAGS += --gcc-install-dir=$(GCC_DIR)
  endif
endif

ifeq ($(UNAME),Darwin)
  # Homebrew: Apple Silicon = /opt/homebrew, Intel = /usr/local
  BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
  CXXFLAGS += -I$(BREW_PREFIX)/include
  LDFLAGS  += -L$(BREW_PREFIX)/lib
endif

a.out: main.cc
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS)

test: a.out
ifeq ($(UNAME),Darwin)
	./a.out
else
	timeout 60 ./a.out
endif

clean:
	rm -f a.out

.PHONY: test clean
