#include <assert.h>
#include <math.h>

float nondet_float (void);

int main (void) {
	float f = nondet_float();
	__ESBMC_assume(f < +INFINITY);
	__ESBMC_assume(f > 1.0f);

	float g = nondet_float();
	__ESBMC_assume(g < 1.0f);
	__ESBMC_assume(g >= 0x1.0p-126f);
	__ESBMC_assume(g > 0.0f);

	float div = f / g;

	assert(!(div == 0.0));

	return 1;
}
