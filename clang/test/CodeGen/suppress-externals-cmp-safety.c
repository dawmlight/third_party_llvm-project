// XFAIL: *
// RUN: %clang_cc1 -triple armv4t-none-unknown-eabi -emit-llvm -O2 \
// RUN:   -mllvm -suppress-externals-cmp-safety -o - -x c %s | FileCheck %s

struct symbol {
  int (*initcall)(void);
};

extern struct symbol start;
extern struct symbol end;

int do_initcall(struct symbol *s, struct symbol *e)
{
  struct symbol *i;

  for (i = s; i != e; i++)
    i->initcall();

  return 0;
}

// CHECK: define void @do_initcalls() {{.*}}
void do_initcalls() {
  struct symbol *ss = &start;
  struct symbol *ee = &end;

  // CHECK: br i1 icmp eq (%struct.symbol* @start, %struct.symbol* @end)

  do_initcall(ss, ee);
}
