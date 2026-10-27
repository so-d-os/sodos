/* calcops.h - operacoes da calculadora (calcops.c), usadas por calc.c */
#ifndef CALCOPS_H
#define CALCOPS_H

typedef long long num;

num calc_eval(const char* expr, const char** err);
int calc_ops_used(void);

#endif
