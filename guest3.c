#include <stdint.h>

static void outb(uint16_t port, uint8_t value) {
	asm("outb %0,%1" : /* empty */ : "a" (value), "Nd" (port) : "memory");
}

static void outl(uint16_t port, uint32_t value) {
    asm("outl %0,%1" : : "a"(value), "Nd"(port) : "memory");
}

void
__attribute__((noreturn))
__attribute__((section(".start")))
_start(void) {
	const char *p;
	uint16_t port = 0xE9;

	for (p = "Hello, world 3!\n"; *p; ++p)
		outb(0xE9, *p);

	for (;;)
		asm("hlt");
}
