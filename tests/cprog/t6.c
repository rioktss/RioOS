#include <stdio.h>
int main() { int n, s = 0; printf("enter: "); scanf("%d", &n); for (int i = 1; i <= n; i++) s += i; printf("sum 1..%d = %d\n", n, s);
  char name[20]; scanf("%s", name); printf("hi %s\n", name); return 0; }
