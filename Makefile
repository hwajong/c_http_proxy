all:
	rm -rf ./proxy
	gcc proxy.c -o proxy -lpthread -g
	gcc proxy_chunked.c -o proxy_chunked -lpthread -g
	./proxy_chunked 47590
