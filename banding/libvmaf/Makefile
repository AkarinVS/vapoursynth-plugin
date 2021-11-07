CFLAGS := -std=c99 -Wall -Wextra

test: test_cambi.c test.c mem.c picture.c ref.c
	cc -o $@ $(CFLAGS) -std=c99 $^ -lm
	./$@

.PHONY: clean
clean:
	rm -f *.o test
