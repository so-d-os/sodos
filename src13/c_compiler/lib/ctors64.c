/* ctors64.c - roda .init_array/.fini_array no SOX2 (LP64). */
typedef void (*ctor_fn)(void);

extern ctor_fn __init_array_start[] __attribute__((weak));
extern ctor_fn __init_array_end[] __attribute__((weak));
extern ctor_fn __fini_array_start[] __attribute__((weak));
extern ctor_fn __fini_array_end[] __attribute__((weak));

void __sodos_ctors(void) {
  ctor_fn* p;
  if (!__init_array_start || !__init_array_end) return;
  for (p = __init_array_start; p < __init_array_end; p++)
    if (*p) (*p)();
}

void __sodos_dtors(void) {
  ctor_fn* p;
  if (!__fini_array_start || !__fini_array_end) return;
  for (p = __fini_array_end; p > __fini_array_start; p--)
    if (p[-1]) p[-1]();
}
