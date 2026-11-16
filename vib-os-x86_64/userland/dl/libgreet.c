/* The first shared library this system will load.
 *
 * It exists to be refused by nothing: a global with a relocation against it, and
 * a function that reads that global, so a linker cannot fold either away. When
 * the loader is finished, dynmain's call to greet() has to cross a real PLT
 * entry into this file, and the value it returns proves the GOT was filled.
 *
 * greet_slot() and greet_bump() answer the question a copy relocation makes
 * unanswerable from outside: where this library's own copy of greet_val lives,
 * and whether the code that runs is reading it. A program holding a copy of a
 * data symbol cannot tell its copy from the original; code inside the library
 * can.
 */
int greet_val = 41;

int greet(void) { return greet_val + 1; }

/* Where this library's code actually lives. A data symbol cannot answer this
 * from outside: the executable's copy relocation moves the object into the
 * executable, and the loader points the library at that copy, so both sides see
 * one address. A function symbol has no such treatment. */
void *greet_addr(void) { return (void *)&greet; }

int *greet_slot(void) { return &greet_val; }

void greet_bump(void) { greet_val++; }
