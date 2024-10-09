CC=mpic++

all: test

test: test.c
	$(CC) -o test -O3 test.c -Wall -pthread -lrt -g

clean:
	rm test
