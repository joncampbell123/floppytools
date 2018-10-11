
BINS=kryoflux1

all: $(BINS)

clean:
	rm -f $(BINS)

kryoflux1: kryoflux1.cpp
	g++ -std=gnu++0x -o $@ $<

