#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#define Q 14
#define F (1 << Q)

typedef int fixed_point;


static inline fixed_point
int_to_fixed_point (int n) 
{
	return n * F;
}

static inline int
fixed_point_down_to_int (fixed_point x) 
{
	return x / F;
}

static inline int
fixed_point_to_nearest_int (fixed_point x) 
{
	return x >= 0 ? x + F / 2 : x - F / 2;
}

static inline fixed_point 
fixed_point_add (fixed_point x, fixed_point y) 
{
	 return x + y;
}

static inline fixed_point 
fixed_point_subtract (fixed_point x, fixed_point y) 
{
	 return x - y;
}

static inline fixed_point 
fixed_point_add_int (fixed_point x, int n) 
{
	 return x + n * F;
}

static inline fixed_point 
fixed_point_subtract_int (fixed_point x, int n) 
{
	 return x - n * F;
}

static inline fixed_point
fixed_point_multiply (fixed_point x, fixed_point y)
{
	return ((int64_t) x) * y / F;
}

static inline fixed_point
fixed_point_multiply_int (fixed_point x, int n)
{
	return x * n;
}

static inline fixed_point
fixed_point_divide (fixed_point x, fixed_point y)
{
	return ((int64_t) x) * F / y;
}

static inline fixed_point
fixed_point_divide_int (fixed_point x, int n)
{
	return x / n;
}

#endif /* threads/fixed-point.h */
