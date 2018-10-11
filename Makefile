
BINS=kryoflux1 kryoflux2 kryoflux3 kryoflux4 kryoflux5

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


kryoflux3: kryoflux3.o kryocomm.o
	g++ -o $@ kryoflux3.o kryocomm.o

kryoflux3.o: kryoflux3.cpp
	g++ -std=gnu++0x -c -o $@ $<


kryoflux4: kryoflux4.o kryocomm.o
	g++ -o $@ kryoflux4.o kryocomm.o

kryoflux4.o: kryoflux4.cpp
	g++ -std=gnu++0x -c -o $@ $<


kryoflux5: kryoflux5.o kryocomm.o
	g++ -o $@ kryoflux5.o kryocomm.o

kryoflux5.o: kryoflux5.cpp
	g++ -std=gnu++0x -c -o $@ $<


kryocomm.o: kryocomm.cpp
	g++ -std=gnu++0x -c -o $@ $<

