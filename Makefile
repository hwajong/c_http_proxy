all:
	rm -rf ./proxy ./proxy_chunked
	gcc proxy.c -o proxy -lpthread -O2
	gcc proxy_chunked.c -o proxy_chunked -lpthread -O2 
	./proxy_chunked 47590
