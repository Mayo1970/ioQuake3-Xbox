/* Small nxdk ABI shims that are not supplied by the Xbox C runtime. */
#include <stdlib.h>

/* Keep atof out of the nxdk strtod assertion path used by this target. */
double atof(const char *text)
{
	const char *p = text;
	double value = 0.0;
	double fraction = 0.1;
	int sign = 1;
	int exponent = 0;
	int exponentSign = 1;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '+' || *p == '-')
	{
		if (*p == '-')
			sign = -1;
		p++;
	}
	while (*p >= '0' && *p <= '9')
		value = value * 10.0 + (*p++ - '0');
	if (*p == '.')
	{
		p++;
		while (*p >= '0' && *p <= '9')
		{
			value += (*p++ - '0') * fraction;
			fraction *= 0.1;
		}
	}
	if (*p == 'e' || *p == 'E')
	{
		p++;
		if (*p == '+' || *p == '-')
		{
			if (*p == '-')
				exponentSign = -1;
			p++;
		}
		while (*p >= '0' && *p <= '9')
			exponent = exponent * 10 + (*p++ - '0');
		while (exponent-- > 0)
			value *= exponentSign < 0 ? 0.1 : 10.0;
	}
	return sign * value;
}

void __xbox_assert(const char *expression, const char *file, int line)
{
	(void)expression;
	(void)file;
	(void)line;
}
