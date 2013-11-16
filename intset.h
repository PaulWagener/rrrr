/* intset.h : a set of integers using a hashtable. only does dynamic allocation on collisions. */

#include <stdbool.h>
#include <stdint.h>

typedef struct intset IntSet;

void IntSet_clear (IntSet *is);

IntSet *IntSet_new (int n);

void IntSet_destroy (IntSet **is);

bool IntSet_contains (IntSet *is, uint32_t value);

void IntSet_add (IntSet *is, uint32_t value);

