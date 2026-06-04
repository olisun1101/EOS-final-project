# 設定編譯器與編譯參數
CC = gcc
CFLAGS = -Wall
LIBS = -lpthread

all: bomb_server bomb_client

bomb_server: bomb_server.c
	$(CC) $(CFLAGS) bomb_server.c -o bomb_server $(LIBS)

bomb_client: bomb_client.c
	$(CC) $(CFLAGS) bomb_client.c -o bomb_client $(LIBS)

# 清除編譯產出的檔案
clean:
	rm -f bomb_server bomb_client