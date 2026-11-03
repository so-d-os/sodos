#ifndef _SODOS_GETOPT_H
#define _SODOS_GETOPT_H
extern char* optarg;
extern int optind, opterr, optopt;
struct option {
  const char* name;
  int has_arg;
  int* flag;
  int val;
};
#define no_argument 0
#define required_argument 1
#define optional_argument 2
int getopt(int argc, char* const argv[], const char* opts);
int getopt_long(int argc, char* const argv[], const char* opts, const struct option* lo,
                int* idx);
#endif
