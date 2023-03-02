#include <string.h>

int initialized;

char *a = "test";
char *b = "----";

int main(int argc, char *argv[]) {
  initialized = 1;
  return strcmp(a, b);
}