#ifndef DOTTR_TEST_H
#define DOTTR_TEST_H

#include "dottr/diag.h"

extern int testFailures;
extern int testCount;

void testFail(const char *file, int line, const char *format, ...)
    DOTTR_PRINTF(3, 4);

#define TEST_FAIL(...) testFail(__FILE__, __LINE__, __VA_ARGS__)

void runLexerTests(void);
void runFormatTests(void);

#endif
