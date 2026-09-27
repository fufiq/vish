#include <stdio.h>
#include <stdlib.h>
double dot(const double* a, const double* b, long n) { double s = 0; for (long i = 0; i < n; ++i) s += a[i] * b[i]; return s; }
int main() { long n = 1000000; double* a = malloc(n * 8); double* b = malloc(n * 8);
  for (long i = 0; i < n; ++i) { a[i] = 1.5; b[i] = 2.0; }
  double t = 0; for (int k = 0; k < 20; ++k) t += dot(a, b, n); printf("%.1f\n", t); }
