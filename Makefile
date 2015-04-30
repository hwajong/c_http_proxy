all:
	rm -rf ./proxy
	gcc proxy.c -o proxy -lpthread -g
	./proxy 47590
