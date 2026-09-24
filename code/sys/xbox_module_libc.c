/* Copyright (C) 1999-2005 Id Software, Inc. Part of Quake III Arena source code. */

/* vsnprintf/sscanf for the native modules: nxdk's pdclib skips %f in both.
   xbox_module.h maps the libc names here; the QVMs used bg_lib.c instead. */

#include "../qcommon/q_shared.h"

#define XFL_LEFT	0x01
#define XFL_PLUS	0x02
#define XFL_SPACE	0x04
#define XFL_ALT		0x08
#define XFL_ZERO	0x10

#define XLEN_NONE	0
#define XLEN_HH		1
#define XLEN_H		2
#define XLEN_L		3
#define XLEN_LL		4
#define XLEN_LD		5
#define XLEN_SIZE	6

/* Significant digits of the approximate path for values of 2^63 and up; the rest print as zeros. */
#define XBOX_MAX_DIGITS 17

typedef struct
{
	char *buffer;
	size_t size;
	size_t length;
} xboxOut_t;

static const double xboxPow10Table[] =
{
	1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
	1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

static double XboxPow10(int exponent)
{
	double value = 1.0;

	while (exponent > 22)
	{
		value *= 1e22;
		exponent -= 22;
	}
	return value * xboxPow10Table[exponent];
}

/* Division by an exact power keeps more precision than multiplying by 10^-n. */
static double XboxScale10(double value, int exponent)
{
	if (exponent >= 0)
		return value / XboxPow10(exponent);
	return value * XboxPow10(-exponent);
}

static unsigned long long XboxPow10Int(int exponent)
{
	unsigned long long value = 1;

	while (exponent-- > 0)
		value *= 10;
	return value;
}

static qboolean XboxIsNan(double value)
{
	return value != value;
}

static qboolean XboxIsInf(double value)
{
	return !XboxIsNan(value) && XboxIsNan(value - value);
}

static qboolean XboxSignBit(double value)
{
	union { double d; unsigned long long u; } bits;

	bits.d = value;
	return (bits.u >> 63) != 0;
}

/* Writes the decimal digits of value (count >= 1) with leading zeros. */
static void XboxDigits(unsigned long long value, int count, char *out)
{
	while (count-- > 0)
	{
		out[count] = (char)('0' + (int)(value % 10));
		value /= 10;
	}
}

static int XboxCountDigits(unsigned long long value)
{
	int count = 1;

	while (value >= 10)
	{
		value /= 10;
		count++;
	}
	return count;
}

/* Binary fraction mantissa / 2^shift in [0, 1), read out one decimal digit at a time.
   Bits dropped to avoid overflow set sticky; digits past the 16th significant may drift. */
typedef struct
{
	unsigned long long mantissa;
	int shift;
	qboolean sticky;
} xboxFraction_t;

static void XboxFractionInit(xboxFraction_t *fraction, double value)
{
	union { double d; unsigned long long u; } bits;
	int exponent;

	bits.d = value;
	exponent = (int)((bits.u >> 52) & 0x7ff);
	fraction->mantissa = bits.u & 0xfffffffffffffULL;
	if (exponent)
		fraction->mantissa |= 1ULL << 52;
	else
		exponent = 1;
	fraction->shift = value == 0.0 ? 1 : 1075 - exponent;
	fraction->sticky = qfalse;
}

static char XboxFractionDigit(xboxFraction_t *fraction)
{
	int digit = 0;

	if (fraction->shift > 60)
	{
		/* Times 10 as times 5 with one less shift; stay below 2^61 so the product fits. */
		while (fraction->mantissa >= 1ULL << 61)
		{
			fraction->sticky |= (fraction->mantissa & 1) != 0;
			fraction->mantissa >>= 1;
			fraction->shift--;
		}
		fraction->mantissa *= 5;
		fraction->shift--;
	}
	else
	{
		fraction->mantissa *= 10;
	}

	if (fraction->shift < 64)
	{
		digit = (int)(fraction->mantissa >> fraction->shift);
		fraction->mantissa &= (1ULL << fraction->shift) - 1;
	}
	return (char)('0' + digit);
}

/* Compares the unread rest with one half: -1 below, 0 exact tie, 1 above. */
static int XboxFractionHalf(const xboxFraction_t *fraction)
{
	unsigned long long half;

	if (fraction->shift > 64)
		return -1;
	half = 1ULL << (fraction->shift - 1);
	if (fraction->mantissa > half || (fraction->mantissa == half && fraction->sticky))
		return 1;
	return fraction->mantissa == half ? 0 : -1;
}

/* Rounds count digits up when the rest is above half, or on a tie to even like the C library.
   Returns 1 when the carry leaves the first digit (all digits are then '0'). */
static int XboxRoundDigits(char *digits, int count, int half)
{
	if (half < 0 || (half == 0 && !(digits[count - 1] & 1)))
		return 0;
	while (count-- > 0)
	{
		if (digits[count] != '9')
		{
			digits[count]++;
			return 0;
		}
		digits[count] = '0';
	}
	return 1;
}

/* Approximate digits for values of 2^63 and up; digits past XBOX_MAX_DIGITS are zeros. */
static int XboxScaledExpDigits(double value, int precision, char *digits)
{
	union { double d; unsigned long long u; } bits;
	int exponent;
	int used;
	double scaled;
	unsigned long long rounded;

	bits.d = value;
	exponent = (int)((double)((int)((bits.u >> 52) & 0x7ff) - 1023) * 0.30102999566398);
	while (XboxScale10(value, exponent) >= 10.0)
		exponent++;
	while (XboxScale10(value, exponent) < 1.0)
		exponent--;

	used = precision < XBOX_MAX_DIGITS - 1 ? precision : XBOX_MAX_DIGITS - 1;
	scaled = XboxScale10(value, exponent - used);
	rounded = (unsigned long long)scaled;
	if (scaled - (double)rounded >= 0.5)
		rounded++;
	if (rounded >= XboxPow10Int(used + 1))
	{
		rounded /= 10;
		exponent++;
	}

	XboxDigits(rounded, used + 1, digits);
	memset(digits + used + 1, '0', precision - used);
	return exponent;
}

/* Rounds positive finite value to precision + 1 significant digits; returns the decimal exponent. */
static int XboxExpDigits(double value, int precision, char *digits)
{
	xboxFraction_t fraction;
	unsigned long long integer;
	char integerText[20];
	int needed = precision + 1;
	int count;
	int exponent;
	int half;
	int i;

	if (value == 0.0)
	{
		memset(digits, '0', needed);
		return 0;
	}
	if (value >= 9.2e18)
		return XboxScaledExpDigits(value, precision, digits);

	integer = (unsigned long long)value;
	XboxFractionInit(&fraction, value - (double)integer);

	if (integer)
	{
		count = XboxCountDigits(integer);
		XboxDigits(integer, count, integerText);
		exponent = count - 1;
		if (count > needed)
		{
			memcpy(digits, integerText, needed);
			half = integerText[needed] > '5' ? 1 : integerText[needed] < '5' ? -1 : 0;
			for (i = needed + 1; i < count && half == 0; i++)
			{
				if (integerText[i] != '0')
					half = 1;
			}
			if (half == 0 && (fraction.mantissa || fraction.sticky))
				half = 1;
		}
		else
		{
			memcpy(digits, integerText, count);
			while (count < needed)
				digits[count++] = XboxFractionDigit(&fraction);
			half = XboxFractionHalf(&fraction);
		}
	}
	else
	{
		exponent = -1;
		while ((digits[0] = XboxFractionDigit(&fraction)) == '0')
			exponent--;
		for (count = 1; count < needed; count++)
			digits[count] = XboxFractionDigit(&fraction);
		half = XboxFractionHalf(&fraction);
	}

	if (XboxRoundDigits(digits, needed, half))
	{
		digits[0] = '1';
		exponent++;
	}
	return exponent;
}

/* Fixed-point digits of positive finite value: integer part, then precision fraction digits.
   Returns the integer digit count. */
static int XboxFixedDigits(double value, int precision, char *digits)
{
	xboxFraction_t fraction;
	unsigned long long integer;
	int integerDigits;
	int i;

	if (value >= 9.2e18)
	{
		char mantissa[XBOX_MAX_DIGITS];
		int exponent = XboxScaledExpDigits(value, XBOX_MAX_DIGITS - 1, mantissa);

		integerDigits = exponent + 1;
		memcpy(digits, mantissa, XBOX_MAX_DIGITS);
		memset(digits + XBOX_MAX_DIGITS, '0', integerDigits - XBOX_MAX_DIGITS + precision);
		return integerDigits;
	}

	integer = (unsigned long long)value;
	XboxFractionInit(&fraction, value - (double)integer);
	integerDigits = XboxCountDigits(integer);
	XboxDigits(integer, integerDigits, digits);
	for (i = 0; i < precision; i++)
		digits[integerDigits + i] = XboxFractionDigit(&fraction);

	if (XboxRoundDigits(digits, integerDigits + precision, XboxFractionHalf(&fraction)))
	{
		memmove(digits + 1, digits, integerDigits + precision);
		digits[0] = '1';
		integerDigits++;
	}
	return integerDigits;
}

static void XboxPutChar(xboxOut_t *out, char c)
{
	if (out->length + 1 < out->size)
		out->buffer[out->length] = c;
	out->length++;
}

static void XboxPutRepeat(xboxOut_t *out, char c, int count)
{
	while (count-- > 0)
		XboxPutChar(out, c);
}

static void XboxPutText(xboxOut_t *out, const char *text, int count)
{
	while (count-- > 0)
		XboxPutChar(out, *text++);
}

/* Emits prefix, zeros, then body inside a width-wide field. */
static void XboxPutField(xboxOut_t *out, const char *prefix, int prefixLength,
	int zeros, const char *body, int bodyLength, int width, int flags)
{
	int pad = width - prefixLength - zeros - bodyLength;

	if (pad < 0)
		pad = 0;
	if (!(flags & (XFL_LEFT | XFL_ZERO)))
		XboxPutRepeat(out, ' ', pad);
	XboxPutText(out, prefix, prefixLength);
	if ((flags & XFL_ZERO) && !(flags & XFL_LEFT))
		XboxPutRepeat(out, '0', pad);
	XboxPutRepeat(out, '0', zeros);
	XboxPutText(out, body, bodyLength);
	if (flags & XFL_LEFT)
		XboxPutRepeat(out, ' ', pad);
}

static int XboxSignPrefix(char *prefix, qboolean negative, int flags)
{
	if (negative)
		prefix[0] = '-';
	else if (flags & XFL_PLUS)
		prefix[0] = '+';
	else if (flags & XFL_SPACE)
		prefix[0] = ' ';
	else
		return 0;
	return 1;
}

static void XboxFormatInteger(xboxOut_t *out, unsigned long long value, qboolean negative,
	qboolean isSigned, int base, qboolean upper, int flags, int width, int precision)
{
	const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char body[24];
	char prefix[3];
	int prefixLength = 0;
	int bodyLength = 0;
	int zeros;
	int i;
	qboolean isZero = value == 0;

	if (precision >= 0)
		flags &= ~XFL_ZERO;
	else
		precision = 1;

	while (value)
	{
		body[bodyLength++] = set[value % base];
		value /= base;
	}
	for (i = 0; i < bodyLength / 2; i++)
	{
		char c = body[i];

		body[i] = body[bodyLength - 1 - i];
		body[bodyLength - 1 - i] = c;
	}

	if (isSigned)
		prefixLength = XboxSignPrefix(prefix, negative, flags);
	if ((flags & XFL_ALT) && base == 8 && precision <= bodyLength)
		precision = bodyLength + 1;
	if ((flags & XFL_ALT) && base == 16 && !isZero)
	{
		prefix[prefixLength++] = '0';
		prefix[prefixLength++] = upper ? 'X' : 'x';
	}

	zeros = precision > bodyLength ? precision - bodyLength : 0;
	XboxPutField(out, prefix, prefixLength, zeros, body, bodyLength, width, flags);
}

static void XboxFormatFloat(xboxOut_t *out, double value, char conversion,
	int flags, int width, int precision)
{
	/* %f of DBL_MAX needs 309 integer digits plus up to 300 fraction digits. */
	char digits[620];
	char body[640];
	char prefix[2];
	int prefixLength;
	int bodyLength = 0;
	int exponent;
	int count;
	int i;
	qboolean upper = conversion >= 'A' && conversion <= 'Z';
	qboolean negative = XboxSignBit(value);
	qboolean useExp;
	qboolean trim = qfalse;
	char lower = (char)(upper ? conversion - 'A' + 'a' : conversion);

	if (negative)
		value = -value;
	prefixLength = XboxSignPrefix(prefix, negative, flags);

	if (XboxIsNan(value) || XboxIsInf(value))
	{
		XboxPutField(out, prefix, prefixLength, 0,
			XboxIsNan(value) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"), 3,
			width, flags & ~XFL_ZERO);
		return;
	}

	if (precision < 0)
		precision = 6;
	if (precision > 300)
		precision = 300;

	useExp = lower == 'e' || lower == 'a';
	if (lower == 'g')
	{
		if (precision == 0)
			precision = 1;
		exponent = XboxExpDigits(value, precision - 1, digits);
		useExp = exponent < -4 || exponent >= precision;
		precision = useExp ? precision - 1 : precision - 1 - exponent;
		trim = !(flags & XFL_ALT);
	}

	if (useExp)
	{
		exponent = XboxExpDigits(value, precision, digits);
		body[bodyLength++] = digits[0];
		count = precision;
		while (trim && count > 0 && digits[count] == '0')
			count--;
		if (count > 0 || (flags & XFL_ALT))
			body[bodyLength++] = '.';
		memcpy(body + bodyLength, digits + 1, count);
		bodyLength += count;
		body[bodyLength++] = upper ? 'E' : 'e';
		body[bodyLength++] = exponent < 0 ? '-' : '+';
		if (exponent < 0)
			exponent = -exponent;
		if (exponent >= 100)
			body[bodyLength++] = (char)('0' + exponent / 100);
		body[bodyLength++] = (char)('0' + exponent / 10 % 10);
		body[bodyLength++] = (char)('0' + exponent % 10);
	}
	else
	{
		int integerDigits = XboxFixedDigits(value, precision, digits);

		memcpy(body, digits, integerDigits);
		bodyLength = integerDigits;
		count = precision;
		while (trim && count > 0 && digits[integerDigits + count - 1] == '0')
			count--;
		if (count > 0 || (flags & XFL_ALT))
			body[bodyLength++] = '.';
		for (i = 0; i < count; i++)
			body[bodyLength++] = digits[integerDigits + i];
	}

	XboxPutField(out, prefix, prefixLength, 0, body, bodyLength, width, flags);
}

int XboxModule_vsnprintf(char *buffer, size_t size, const char *format, va_list args)
{
	xboxOut_t out;

	out.buffer = buffer;
	out.size = size;
	out.length = 0;

	while (*format)
	{
		int flags = 0;
		int width = 0;
		int precision = -1;
		int length = XLEN_NONE;
		char conversion;

		if (*format != '%')
		{
			XboxPutChar(&out, *format++);
			continue;
		}
		format++;

		for (;; format++)
		{
			if (*format == '-')
				flags |= XFL_LEFT;
			else if (*format == '+')
				flags |= XFL_PLUS;
			else if (*format == ' ')
				flags |= XFL_SPACE;
			else if (*format == '#')
				flags |= XFL_ALT;
			else if (*format == '0')
				flags |= XFL_ZERO;
			else
				break;
		}

		if (*format == '*')
		{
			width = va_arg(args, int);
			if (width < 0)
			{
				flags |= XFL_LEFT;
				width = -width;
			}
			format++;
		}
		else
		{
			while (*format >= '0' && *format <= '9')
				width = width * 10 + (*format++ - '0');
		}

		if (*format == '.')
		{
			format++;
			precision = 0;
			if (*format == '*')
			{
				precision = va_arg(args, int);
				format++;
			}
			else
			{
				while (*format >= '0' && *format <= '9')
					precision = precision * 10 + (*format++ - '0');
			}
		}

		switch (*format)
		{
		case 'h':
			length = format[1] == 'h' ? XLEN_HH : XLEN_H;
			format += length == XLEN_HH ? 2 : 1;
			break;
		case 'l':
			length = format[1] == 'l' ? XLEN_LL : XLEN_L;
			format += length == XLEN_LL ? 2 : 1;
			break;
		case 'L':
		case 'q':
			length = XLEN_LD;
			format++;
			break;
		case 'j':
			length = XLEN_LL;
			format++;
			break;
		case 'z':
		case 't':
			length = XLEN_SIZE;
			format++;
			break;
		case 'I':
			if (format[1] == '6' && format[2] == '4')
			{
				length = XLEN_LL;
				format += 3;
			}
			break;
		}

		conversion = *format;
		if (!conversion)
		{
			XboxPutChar(&out, '%');
			break;
		}
		format++;

		switch (conversion)
		{
		case 'd':
		case 'i':
		{
			long long value;

			if (length == XLEN_LL || length == XLEN_LD)
				value = va_arg(args, long long);
			else if (length == XLEN_L)
				value = va_arg(args, long);
			else if (length == XLEN_SIZE)
				value = va_arg(args, ptrdiff_t);
			else
				value = va_arg(args, int);
			if (length == XLEN_HH)
				value = (signed char)value;
			else if (length == XLEN_H)
				value = (short)value;

			XboxFormatInteger(&out, value < 0 ? 0ULL - (unsigned long long)value :
				(unsigned long long)value, value < 0, qtrue, 10, qfalse, flags, width, precision);
			break;
		}
		case 'u':
		case 'o':
		case 'x':
		case 'X':
		{
			unsigned long long value;

			if (length == XLEN_LL || length == XLEN_LD)
				value = va_arg(args, unsigned long long);
			else if (length == XLEN_L)
				value = va_arg(args, unsigned long);
			else if (length == XLEN_SIZE)
				value = va_arg(args, size_t);
			else
				value = va_arg(args, unsigned int);
			if (length == XLEN_HH)
				value = (unsigned char)value;
			else if (length == XLEN_H)
				value = (unsigned short)value;

			XboxFormatInteger(&out, value, qfalse, qfalse,
				conversion == 'u' ? 10 : conversion == 'o' ? 8 : 16,
				conversion == 'X', flags, width, precision);
			break;
		}
		case 'p':
			XboxFormatInteger(&out, (uintptr_t)va_arg(args, void *), qfalse, qfalse, 16, qfalse,
				flags | XFL_ALT, width, precision);
			break;
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
		case 'a':
		case 'A':
		{
			double value;

			if (length == XLEN_LD)
				value = (double)va_arg(args, long double);
			else
				value = va_arg(args, double);
			XboxFormatFloat(&out, value, conversion, flags, width, precision);
			break;
		}
		case 'c':
		{
			char c = (char)va_arg(args, int);

			XboxPutField(&out, "", 0, 0, &c, 1, width, flags & ~XFL_ZERO);
			break;
		}
		case 's':
		{
			const char *text = va_arg(args, const char *);
			int textLength = 0;

			if (!text)
				text = "(null)";
			while (text[textLength] && (precision < 0 || textLength < precision))
				textLength++;
			XboxPutField(&out, "", 0, 0, text, textLength, width, flags & ~XFL_ZERO);
			break;
		}
		case 'n':
		{
			void *target = va_arg(args, void *);

			if (length == XLEN_HH)
				*(signed char *)target = (signed char)out.length;
			else if (length == XLEN_H)
				*(short *)target = (short)out.length;
			else if (length == XLEN_LL || length == XLEN_LD)
				*(long long *)target = (long long)out.length;
			else if (length == XLEN_L)
				*(long *)target = (long)out.length;
			else
				*(int *)target = (int)out.length;
			break;
		}
		case '%':
			XboxPutChar(&out, '%');
			break;
		default:
			XboxPutChar(&out, '%');
			XboxPutChar(&out, conversion);
			break;
		}
	}

	if (size > 0)
		buffer[out.length < size ? out.length : size - 1] = '\0';
	return (int)out.length;
}

static qboolean XboxScanSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int XboxScanDigit(char c, int base)
{
	int value;

	if (c >= '0' && c <= '9')
		value = c - '0';
	else if (c >= 'a' && c <= 'z')
		value = c - 'a' + 10;
	else if (c >= 'A' && c <= 'Z')
		value = c - 'A' + 10;
	else
		return -1;
	return value < base ? value : -1;
}

/* Parses an integer from at most limit chars; returns chars used, 0 on no digits. */
static int XboxScanInteger(const char *text, int limit, int base, unsigned long long *value,
	qboolean *negative)
{
	int used = 0;
	int digits = 0;

	*value = 0;
	*negative = qfalse;
	if (used < limit && (text[used] == '+' || text[used] == '-'))
		*negative = text[used++] == '-';

	if ((base == 0 || base == 16) && used + 2 < limit && text[used] == '0' &&
		(text[used + 1] == 'x' || text[used + 1] == 'X') && XboxScanDigit(text[used + 2], 16) >= 0)
	{
		base = 16;
		used += 2;
	}
	else if (base == 0)
	{
		base = used < limit && text[used] == '0' ? 8 : 10;
	}

	while (used < limit && XboxScanDigit(text[used], base) >= 0)
	{
		*value = *value * base + XboxScanDigit(text[used], base);
		used++;
		digits++;
	}
	return digits ? used : 0;
}

static qboolean XboxScanWord(const char *text, int limit, const char *word)
{
	int i;

	for (i = 0; word[i]; i++)
	{
		if (i >= limit || (text[i] | 0x20) != word[i])
			return qfalse;
	}
	return qtrue;
}

/* Parses a decimal float from at most limit chars; returns chars used, 0 on no number. */
static int XboxScanFloat(const char *text, int limit, double *value)
{
	unsigned long long mantissa = 0;
	int used = 0;
	int digits = 0;
	int exponent = 0;
	qboolean negative = qfalse;

	if (used < limit && (text[used] == '+' || text[used] == '-'))
		negative = text[used++] == '-';

	if (XboxScanWord(text + used, limit - used, "inf"))
	{
		used += XboxScanWord(text + used, limit - used, "infinity") ? 8 : 3;
		*value = negative ? -HUGE_VAL : HUGE_VAL;
		return used;
	}
	if (XboxScanWord(text + used, limit - used, "nan"))
	{
		*value = negative ? -(HUGE_VAL - HUGE_VAL) : HUGE_VAL - HUGE_VAL;
		return used + 3;
	}

	for (; used < limit && text[used] >= '0' && text[used] <= '9'; used++, digits++)
	{
		if (mantissa < 100000000000000000ULL)
			mantissa = mantissa * 10 + (text[used] - '0');
		else
			exponent++;
	}
	if (used < limit && text[used] == '.')
	{
		for (used++; used < limit && text[used] >= '0' && text[used] <= '9'; used++, digits++)
		{
			if (mantissa < 100000000000000000ULL)
			{
				mantissa = mantissa * 10 + (text[used] - '0');
				exponent--;
			}
		}
	}
	if (!digits)
		return 0;

	if (used < limit && (text[used] == 'e' || text[used] == 'E'))
	{
		int start = used + 1;
		int expValue = 0;
		qboolean expNegative = qfalse;

		if (start < limit && (text[start] == '+' || text[start] == '-'))
			expNegative = text[start++] == '-';
		if (start < limit && text[start] >= '0' && text[start] <= '9')
		{
			for (used = start; used < limit && text[used] >= '0' && text[used] <= '9'; used++)
			{
				if (expValue < 10000)
					expValue = expValue * 10 + (text[used] - '0');
			}
			exponent += expNegative ? -expValue : expValue;
		}
	}

	if (exponent < -340 || mantissa == 0)
		*value = 0.0;
	else if (exponent > 310)
		*value = HUGE_VAL;
	else if (exponent < -300)
		*value = XboxScale10(XboxScale10((double)mantissa, 300), -exponent - 300);
	else
		*value = XboxScale10((double)mantissa, -exponent);
	if (negative)
		*value = -*value;
	return used;
}

static void XboxStoreInteger(void *target, int length, unsigned long long value)
{
	if (length == XLEN_HH)
		*(char *)target = (char)value;
	else if (length == XLEN_H)
		*(short *)target = (short)value;
	else if (length == XLEN_LL || length == XLEN_LD)
		*(long long *)target = (long long)value;
	else if (length == XLEN_L)
		*(long *)target = (long)value;
	else
		*(int *)target = (int)value;
}

static int XboxModule_vsscanf(const char *input, const char *format, va_list args)
{
	const char *text = input;
	int assigned = 0;
	qboolean converted = qfalse;

	while (*format)
	{
		qboolean suppress = qfalse;
		int width = 0;
		int length = XLEN_NONE;
		char conversion;

		if (XboxScanSpace(*format))
		{
			while (XboxScanSpace(*format))
				format++;
			while (XboxScanSpace(*text))
				text++;
			continue;
		}

		if (*format != '%' || format[1] == '%')
		{
			if (*format == '%')
			{
				format++;
				while (XboxScanSpace(*text))
					text++;
			}
			if (*text != *format)
				return !*text && !converted ? EOF : assigned;
			text++;
			format++;
			continue;
		}
		format++;

		if (*format == '*')
		{
			suppress = qtrue;
			format++;
		}
		while (*format >= '0' && *format <= '9')
			width = width * 10 + (*format++ - '0');

		switch (*format)
		{
		case 'h':
			length = format[1] == 'h' ? XLEN_HH : XLEN_H;
			format += length == XLEN_HH ? 2 : 1;
			break;
		case 'l':
			length = format[1] == 'l' ? XLEN_LL : XLEN_L;
			format += length == XLEN_LL ? 2 : 1;
			break;
		case 'L':
		case 'q':
		case 'j':
			length = format[0] == 'L' ? XLEN_LD : XLEN_LL;
			format++;
			break;
		case 'z':
		case 't':
			length = XLEN_SIZE;
			format++;
			break;
		}

		conversion = *format;
		if (!conversion)
			break;
		format++;

		if (conversion == 'n')
		{
			if (!suppress)
				XboxStoreInteger(va_arg(args, void *), length, (unsigned long long)(text - input));
			continue;
		}

		if (conversion != 'c' && conversion != '[')
		{
			while (XboxScanSpace(*text))
				text++;
		}
		if (!*text)
			return converted ? assigned : EOF;
		if (width <= 0)
			width = conversion == 'c' ? 1 : INT_MAX;

		switch (conversion)
		{
		case 'd':
		case 'i':
		case 'u':
		case 'o':
		case 'x':
		case 'X':
		{
			unsigned long long value;
			qboolean negative;
			int base = conversion == 'i' ? 0 : conversion == 'o' ? 8 :
				(conversion == 'x' || conversion == 'X') ? 16 : 10;
			int used = XboxScanInteger(text, width, base, &value, &negative);

			if (!used)
				return assigned;
			text += used;
			if (negative)
				value = 0ULL - value;
			if (!suppress)
			{
				XboxStoreInteger(va_arg(args, void *), length, value);
				assigned++;
			}
			break;
		}
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
		case 'a':
		case 'A':
		{
			double value;
			int used = XboxScanFloat(text, width, &value);

			if (!used)
				return assigned;
			text += used;
			if (!suppress)
			{
				if (length == XLEN_LD)
					*va_arg(args, long double *) = value;
				else if (length == XLEN_L)
					*va_arg(args, double *) = value;
				else
					*va_arg(args, float *) = (float)value;
				assigned++;
			}
			break;
		}
		case 's':
		{
			char *target = suppress ? NULL : va_arg(args, char *);

			while (*text && !XboxScanSpace(*text) && width-- > 0)
			{
				if (target)
					*target++ = *text;
				text++;
			}
			if (target)
			{
				*target = '\0';
				assigned++;
			}
			break;
		}
		case 'c':
		{
			char *target = suppress ? NULL : va_arg(args, char *);

			while (width-- > 0)
			{
				if (!*text)
					return assigned;
				if (target)
					*target++ = *text;
				text++;
			}
			if (target)
				assigned++;
			break;
		}
		case '[':
		{
			char *target = suppress ? NULL : va_arg(args, char *);
			const char *set;
			const char *setEnd;
			qboolean invert = qfalse;
			int matched = 0;

			if (*format == '^')
			{
				invert = qtrue;
				format++;
			}
			set = format;
			if (*format == ']')
				format++;
			while (*format && *format != ']')
				format++;
			setEnd = format;
			if (*format)
				format++;

			while (*text && width > 0)
			{
				const char *p;
				qboolean inSet = qfalse;

				for (p = set; p < setEnd; p++)
				{
					if (p + 2 < setEnd && p[1] == '-')
					{
						if ((unsigned char)*text >= (unsigned char)p[0] &&
							(unsigned char)*text <= (unsigned char)p[2])
							inSet = qtrue;
						p += 2;
					}
					else if (*p == *text)
					{
						inSet = qtrue;
					}
				}
				if (inSet == invert)
					break;
				if (target)
					*target++ = *text;
				text++;
				width--;
				matched++;
			}
			if (!matched)
				return assigned;
			if (target)
			{
				*target = '\0';
				assigned++;
			}
			break;
		}
		default:
			return assigned;
		}
		converted = qtrue;
	}

	return assigned;
}

int XboxModule_sscanf(const char *input, const char *format, ...)
{
	va_list args;
	int result;

	va_start(args, format);
	result = XboxModule_vsscanf(input, format, args);
	va_end(args);
	return result;
}
