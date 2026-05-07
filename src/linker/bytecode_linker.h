/*
 * Bytecode linker for resolving BC_COMPONENT_LINKED into internal function targets.
 */

#ifndef MOT_BYTECODE_LINKER_H
#define MOT_BYTECODE_LINKER_H

#include <stdbool.h>
#include <stddef.h>
#include "../compiler/bytecode.h"

typedef BytecodeModule *(*MotLinkedModuleResolverFn)(const char *component_path, void *userdata);

/*
 * Resolve linked component references for a module by recursively merging linked component
 * function chunks into the same module. The resolved mapping is encoded as internal
 * dependency markers: "@linked_ref:<ref_idx>:<func_idx>".
 */
bool mot_link_resolve_module(
    BytecodeModule *module,
    MotLinkedModuleResolverFn resolver,
    void *resolver_userdata,
    char *error_buf,
    size_t error_buf_len
);

#endif /* MOT_BYTECODE_LINKER_H */

