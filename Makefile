all:
	rm -rf ./proxy
	gcc proxy.c -o proxy -lpthread 
	./proxy 47590
