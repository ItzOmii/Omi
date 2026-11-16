/* The first shared library this system will load.
 *
 * It exists to be refused by nothing: a global with a relocation against it, and
 * a function that reads that global, so a linker cannot fold either away. When
 * the loader is finished, dynmain's call to greet() has to cross a real PLT
 * entry into this file, and the value it returns proves the GOT was filled.
 */
int greet_val = 41;

int greet(void) { return greet_val + 1; }