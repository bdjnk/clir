clir_example: clir.h clir.c

clir_example: clir.c example.c
	$(CC) -Wall -W -Os -g -o example clir.c example.c

clean:
	rm -f example
