#ifndef STILL_DRIVER_H
#define STILL_DRIVER_H
#include "arena.h"
char *read_file(const char *path);
char *expand_imports(const char *entry_path,Arena *arena);
char *format_source(char *source,const char *filename,Arena *arena);
int driver_module_location(const char *text,size_t length,char **filename,int *line);
#endif
