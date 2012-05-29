all:
	gcc -Wall -Wextra -pedantic -std=c99 region_list.c -o region_list

clean:
	rm region_list
