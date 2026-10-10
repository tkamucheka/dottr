#include "dottr/ast.h"

const char *predicateKindName(PredicateKind kind)
{
    switch (kind) {
    case PREDICATE_COMMAND: return "command";
    case PREDICATE_FILE: return "file";
    case PREDICATE_DIRECTORY: return "directory";
    }
    return "predicate";
}
