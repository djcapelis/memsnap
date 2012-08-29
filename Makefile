all:
	gcc -c -Wall -Wextra -pedantic -std=c99 region_list.c -o region_list.o
	gcc -c -Wall -Wextra -pedantic -std=c99 memsnap.c -o memsnap.o
	gcc -Wall -Wextra -pedantic memsnap.o region_list.o -o memsnap -lrt -lpthread

debug:
	gcc -g -c -Wall -Wextra -pedantic -std=c99 region_list.c -o region_list.o
	gcc -g -c -Wall -Wextra -pedantic -std=c99 memsnap.c -o memsnap.o
	gcc -g -Wall -Wextra -pedantic memsnap.o region_list.o -o memsnap -lrt -lpthread

clean:
	rm memsnap memsnap.o region_list.o

cleansnaps:
	rm pid*_snap*
