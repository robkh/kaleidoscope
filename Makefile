all: kaleidoscope

kaleidoscope: kaleidoscope.cpp
	clang++ -std=c++14 -g kaleidoscope.cpp -o kaleidoscope

clean:
	rm kaleidoscope.o
