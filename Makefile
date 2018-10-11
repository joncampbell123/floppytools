
BINS=kryoflux1 kryoflux2

all: $(BINS)

clean:
	rm -f $(BINS) *.o

kryoflux1: kryoflux1.o kryocomm.o
	g++ -o $@ kryoflux1.o kryocomm.o

kryoflux1.o: kryoflux1.cpp
	g++ -std=gnu++0x -c -o $@ $<


kryoflux2: kryoflux2.o kryocomm.o
	g++ -o $@ kryoflux2.o kryocomm.o

kryoflux2.o: kryoflux2.cpp
	g++ -std=gnu++0x -c -o $@ $<


kryocomm.o: kryocomm.cpp
	g++ -std=gnu++0x -c -o $@ $<

