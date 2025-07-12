OUTPUT = fex

main:
	cc -Wall -Wextra main.c -o $(OUTPUT)

install:
	cp -f $(OUTPUT) /usr/bin

clean:
	rm -f $(OUTPUT)
