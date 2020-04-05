
BINS=kryoflux1 kryoflux2 kryoflux3 kryoflux4 kryoflux5 kryoflux6 kryoflux7 kryofluxplot kryo_ibm

all: $(BINS)

clean:
	rm -f $(BINS) *.o graph.csv graph.gnuplot graph.png disk.img

kryo_ibm: kryo_ibm.o kryocomm.o
	g++ -o $@ kryo_ibm.o kryocomm.o

kryo_ibm.o: kryo_ibm.cpp
	g++ -std=gnu++0x -c -o $@ $<


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


kryoflux6: kryoflux6.o kryocomm.o
	g++ -o $@ kryoflux6.o kryocomm.o

kryoflux6.o: kryoflux6.cpp
	g++ -std=gnu++0x -c -o $@ $<


kryoflux7: kryoflux7.o kryocomm.o
	g++ -o $@ kryoflux7.o kryocomm.o

kryoflux7.o: kryoflux7.cpp
	g++ -std=gnu++0x -c -o $@ $<


kryofluxplot: kryofluxplot.o kryocomm.o
	g++ -o $@ kryofluxplot.o kryocomm.o

kryofluxplot.o: kryofluxplot.cpp
	g++ -std=gnu++0x -c -o $@ $<


kryocomm.o: kryocomm.cpp
	g++ -std=gnu++0x -c -o $@ $<

