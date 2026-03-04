CXX = clang++
CXXFLAGS = -std=c++2b --gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13 -pthread -O0 -DDEBUG -g -fsanitize=address -ferror-limit=5
LDFLAGS = -lgtest -lgtest_main -ledit -levent

a.out: main.cc
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS)

test: a.out
	timeout 60 ./a.out

clean:
	rm -f a.out

.PHONY: test clean
