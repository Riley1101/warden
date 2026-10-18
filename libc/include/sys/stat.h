#pragma once

#include <abi/fs.h> /* struct stat, FT_FILE, FT_DIR */

int stat(const char *path, struct stat *st);
int mkdir(const char *path);
