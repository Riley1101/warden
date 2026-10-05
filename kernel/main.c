#include <serial.h>
#include <io.h>

int kmain() {

    serial_init(COM1);

    serial_puts(COM1,"Hello, World!\n");

    return 0;
}
