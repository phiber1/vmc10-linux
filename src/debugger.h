#ifndef DEBUGGER_H
#define DEBUGGER_H
#include "mc10.h"
/* Interactive monitor on stdin/stdout.  Returns false if the user quit. */
bool debugger_run(mc10 *m);
#endif
