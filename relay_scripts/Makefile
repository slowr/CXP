all: server

poll: poll_server.c
	gcc -o server.out poll_server.c -lpthread

server: server.c
	gcc -o server.out server.c -lpthread

clean:
	rm *.out
