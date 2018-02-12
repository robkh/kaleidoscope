all: kaleidoscope.o

kaleidoscope.o: kaleidoscope.cpp
	g++ -c kaleidoscope.cpp

clean:
	rm kaleidoscope.o
