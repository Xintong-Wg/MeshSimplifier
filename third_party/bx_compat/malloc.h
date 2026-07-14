// Compatibility shim: macOS doesn't have <malloc.h>
// The standard malloc functions are declared in <stdlib.h>
#include <stdlib.h>
#include <malloc/malloc.h>
