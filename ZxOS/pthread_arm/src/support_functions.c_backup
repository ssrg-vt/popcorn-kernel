#include <sys/types.h>
typedef int                int32_t;
typedef uint32_t              u_int32_t;
typedef uint64_t              u_int64_t;

typedef union
{
  double value;
  struct
  {
    u_int32_t msw;
    u_int32_t lsw;
  } parts;
  struct
  {
    u_int64_t w;
  } xparts;
} ieee_double_shape_type;

#define GET_LOW_WORD(i,d)                                       \
do {                                                            \
  ieee_double_shape_type gl_u;                                  \
  gl_u.value = (d);                                             \
  (i) = gl_u.parts.lsw;                                         \
} while (0)

#define GET_HIGH_WORD(i,d)                                      \
do {                                                            \
  ieee_double_shape_type gh_u;                                  \
  gh_u.value = (d);                                             \
  (i) = gh_u.parts.msw;                                         \
} while (0)

#define SET_HIGH_WORD(d,v)                                      \
do {                                                            \
  ieee_double_shape_type sh_u;                                  \
  sh_u.value = (d);                                             \
  sh_u.parts.msw = (v);                                         \
  (d) = sh_u.value;                                             \
} while (0)

#define EXTRACT_WORDS(ix0,ix1,d)                                \
do {                                                            \
  ieee_double_shape_type ew_u;                                  \
  ew_u.value = (d);                                             \
  (ix0) = ew_u.parts.msw;                                       \
  (ix1) = ew_u.parts.lsw;                                       \
} while (0)

#define SET_LOW_WORD(d,v)                                       \
do {                                                            \
  ieee_double_shape_type sl_u;                                  \
  sl_u.value = (d);                                             \
  sl_u.parts.lsw = (v);                                         \
  (d) = sl_u.value;                                             \
} while (0)

#define INSERT_WORDS(d,ix0,ix1)                                 \
do {                                                            \
  ieee_double_shape_type iw_u;                                  \
  iw_u.parts.msw = (ix0);                                       \
  iw_u.parts.lsw = (ix1);                                       \
  (d) = iw_u.value;                                             \
} while (0)

#define STRICT_ASSIGN(type, lval, rval) do {    \
        volatile type __lval;                   \
                                                \
        if (sizeof(type) >= sizeof(long double))        \
                (lval) = (rval);                \
        else {                                  \
                __lval = (rval);                \
                (lval) = __lval;                \
        }                                       \
} while (0)


double
copysign(double x, double y)
{
        u_int32_t hx,hy;
        GET_HIGH_WORD(hx,x);
        GET_HIGH_WORD(hy,y);
        SET_HIGH_WORD(x,(hx&0x7fffffff)|(hy&0x80000000));
        return x;
}

double
scalbn (double x, int n)
{
static const double
two54   =  1.80143985094819840000e+16, /* 0x43500000, 0x00000000 */
twom54  =  5.55111512312578270212e-17, /* 0x3C900000, 0x00000000 */
huge   = 1.0e+300,
tiny   = 1.0e-300;
        int32_t k,hx,lx;
        EXTRACT_WORDS(hx,lx,x);
        k = (hx&0x7ff00000)>>20;                /* extract exponent */
        if (k==0) {                             /* 0 or subnormal x */
            if ((lx|(hx&0x7fffffff))==0) return x; /* +-0 */
            x *= two54;
            GET_HIGH_WORD(hx,x);
            k = ((hx&0x7ff00000)>>20) - 54;
            if (n< -50000) return tiny*x;       /*underflow*/
            }
        if (k==0x7ff) return x+x;               /* NaN or Inf */
        k = k+n;
        if (k >  0x7fe) return huge*copysign(huge,x); /* overflow  */
        if (k > 0)                              /* normal result */
            {SET_HIGH_WORD(x,(hx&0x800fffff)|(k<<20)); return x;}
        if (k <= -54) {
            if (n > 50000)      /* in case integer overflow in n+k */
                return huge*copysign(huge,x);   /*overflow*/
            else return tiny*copysign(tiny,x);  /*underflow*/
        }
        k += 54;                                /* subnormal result */
        SET_HIGH_WORD(x,(hx&0x800fffff)|(k<<20));
        return x*twom54;
}

static const double
bp[] = {1.0, 1.5,},
dp_h[] = { 0.0, 5.84962487220764160156e-01,}, /* 0x3FE2B803, 0x40000000 */
dp_l[] = { 0.0, 1.35003920212974897128e-08,}, /* 0x3E4CFDEB, 0x43CFD006 */
zero    =  0.0,
one	=  1.0,
two	=  2.0,
two53	=  9007199254740992.0,	/* 0x43400000, 0x00000000 */
huge	=  1.0e300,
tiny    =  1.0e-300,
	/* poly coefs for (3/2)*(log(x)-2s-2/3*s**3 */
L1  =  5.99999999999994648725e-01, /* 0x3FE33333, 0x33333303 */
L2  =  4.28571428578550184252e-01, /* 0x3FDB6DB6, 0xDB6FABFF */
L3  =  3.33333329818377432918e-01, /* 0x3FD55555, 0x518F264D */
L4  =  2.72728123808534006489e-01, /* 0x3FD17460, 0xA91D4101 */
L5  =  2.30660745775561754067e-01, /* 0x3FCD864A, 0x93C9DB65 */
L6  =  2.06975017800338417784e-01, /* 0x3FCA7E28, 0x4A454EEF */
P1   =  1.66666666666666019037e-01, /* 0x3FC55555, 0x5555553E */
P2   = -2.77777777770155933842e-03, /* 0xBF66C16C, 0x16BEBD93 */
P3   =  6.61375632143793436117e-05, /* 0x3F11566A, 0xAF25DE2C */
P4   = -1.65339022054652515390e-06, /* 0xBEBBBD41, 0xC5D26BF1 */
P5   =  4.13813679705723846039e-08, /* 0x3E663769, 0x72BEA4D0 */
lg2  =  6.93147180559945286227e-01, /* 0x3FE62E42, 0xFEFA39EF */
lg2_h  =  6.93147182464599609375e-01, /* 0x3FE62E43, 0x00000000 */
lg2_l  = -1.90465429995776804525e-09, /* 0xBE205C61, 0x0CA86C39 */
ovt =  8.0085662595372944372e-0017, /* -(1024-log2(ovfl+.5ulp)) */
cp    =  9.61796693925975554329e-01, /* 0x3FEEC709, 0xDC3A03FD =2/(3ln2) */
cp_h  =  9.61796700954437255859e-01, /* 0x3FEEC709, 0xE0000000 =(float)cp */
cp_l  = -7.02846165095275826516e-09, /* 0xBE3E2FE0, 0x145B01F5 =tail of cp_h*/
ivln2    =  1.44269504088896338700e+00, /* 0x3FF71547, 0x652B82FE =1/ln2 */
ivln2_h  =  1.44269502162933349609e+00, /* 0x3FF71547, 0x60000000 =24b 1/ln2*/
ivln2_l  =  1.92596299112661746887e-08; /* 0x3E54AE0B, 0xF85DDF44 =1/ln2 tail*/

static const double
ln2_hi  =  6.93147180369123816490e-01,	/* 3fe62e42 fee00000 */
ln2_lo  =  1.90821492927058770002e-10,	/* 3dea39ef 35793c76 */
two54   =  1.80143985094819840000e+16,  /* 43500000 00000000 */
Lg1 = 6.666666666666735130e-01,  /* 3FE55555 55555593 */
Lg2 = 3.999999999940941908e-01,  /* 3FD99999 9997FA04 */
Lg3 = 2.857142874366239149e-01,  /* 3FD24924 94229359 */
Lg4 = 2.222219843214978396e-01,  /* 3FCC71C5 1D8E78AF */
Lg5 = 1.818357216161805012e-01,  /* 3FC74664 96CB03DE */
Lg6 = 1.531383769920937332e-01,  /* 3FC39A09 D078C69F */
Lg7 = 1.479819860511658591e-01;  /* 3FC2F112 DF3E5244 */

double
fabs(double x)
{
        u_int32_t high;
        GET_HIGH_WORD(high,x);
        SET_HIGH_WORD(x,high&0x7fffffff);
        return x;
}

double sqrt(double x)
{
	double z;
	int32_t sign = (int)0x80000000;
	int32_t ix0,s0,q,m,t,i;
	u_int32_t r,t1,s1,ix1,q1;

	EXTRACT_WORDS(ix0,ix1,x);

    /* take care of Inf and NaN */
	if((ix0&0x7ff00000)==0x7ff00000) {			
	    return x*x+x;		/* sqrt(NaN)=NaN, sqrt(+inf)=+inf
					   sqrt(-inf)=sNaN */
	} 
    /* take care of zero */
	if(ix0<=0) {
	    if(((ix0&(~sign))|ix1)==0) return x;/* sqrt(+-0) = +-0 */
	    else if(ix0<0)
		return (x-x)/(x-x);		/* sqrt(-ve) = sNaN */
	}
    /* normalize x */
	m = (ix0>>20);
	if(m==0) {				/* subnormal x */
	    while(ix0==0) {
		m -= 21;
		ix0 |= (ix1>>11); ix1 <<= 21;
	    }
	    for(i=0;(ix0&0x00100000)==0;i++) ix0<<=1;
	    m -= i-1;
	    ix0 |= (ix1>>(32-i));
	    ix1 <<= i;
	}
	m -= 1023;	/* unbias exponent */
	ix0 = (ix0&0x000fffff)|0x00100000;
	if(m&1){	/* odd m, double x to make it even */
	    ix0 += ix0 + ((ix1&sign)>>31);
	    ix1 += ix1;
	}
	m >>= 1;	/* m = [m/2] */

    /* generate sqrt(x) bit by bit */
	ix0 += ix0 + ((ix1&sign)>>31);
	ix1 += ix1;
	q = q1 = s0 = s1 = 0;	/* [q,q1] = sqrt(x) */
	r = 0x00200000;		/* r = moving bit from right to left */

	while(r!=0) {
	    t = s0+r; 
	    if(t<=ix0) { 
		s0   = t+r; 
		ix0 -= t; 
		q   += r; 
	    } 
	    ix0 += ix0 + ((ix1&sign)>>31);
	    ix1 += ix1;
	    r>>=1;
	}

	r = sign;
	while(r!=0) {
	    t1 = s1+r; 
	    t  = s0;
	    if((t<ix0)||((t==ix0)&&(t1<=ix1))) { 
		s1  = t1+r;
		if(((t1&sign)==sign)&&(s1&sign)==0) s0 += 1;
		ix0 -= t;
		if (ix1 < t1) ix0 -= 1;
		ix1 -= t1;
		q1  += r;
	    }
	    ix0 += ix0 + ((ix1&sign)>>31);
	    ix1 += ix1;
	    r>>=1;
	}

    /* use floating add to find out rounding direction */
	if((ix0|ix1)!=0) {
	    z = one-tiny; /* trigger inexact flag */
	    if (z>=one) {
	        z = one+tiny;
	        if (q1==(u_int32_t)0xffffffff) { q1=0; q += 1;}
		else if (z>one) {
		    if (q1==(u_int32_t)0xfffffffe) q+=1;
		    q1+=2; 
		} else
	            q1 += (q1&1);
	    }
	}
	ix0 = (q>>1)+0x3fe00000;
	ix1 =  q1>>1;
	if ((q&1)==1) ix1 |= sign;
	ix0 += (m <<20);
	INSERT_WORDS(z,ix0,ix1);
	return z;
}

double pow(double x, double y)
{
	double z,ax,z_h,z_l,p_h,p_l;
	double y1,t1,t2,r,s,t,u,v,w;
	int32_t i,j,k,yisint,n;
	int32_t hx,hy,ix,iy;
	u_int32_t lx,ly;

	EXTRACT_WORDS(hx,lx,x);
	EXTRACT_WORDS(hy,ly,y);
	ix = hx&0x7fffffff;  iy = hy&0x7fffffff;

    /* y==zero: x**0 = 1 */
	if((iy|ly)==0) return one; 	

    /* x==1: 1**y = 1, even if y is NaN */
	if (hx==0x3ff00000 && lx == 0) return one;

    /* y!=zero: result is NaN if either arg is NaN */
	if(ix > 0x7ff00000 || ((ix==0x7ff00000)&&(lx!=0)) ||
	   iy > 0x7ff00000 || ((iy==0x7ff00000)&&(ly!=0))) 
		return (x+0.0)+(y+0.0);

    /* determine if y is an odd int when x < 0
     * yisint = 0	... y is not an integer
     * yisint = 1	... y is an odd int
     * yisint = 2	... y is an even int
     */
	yisint  = 0;
	if(hx<0) {	
	    if(iy>=0x43400000) yisint = 2; /* even integer y */
	    else if(iy>=0x3ff00000) {
		k = (iy>>20)-0x3ff;	   /* exponent */
		if(k>20) {
		    j = ly>>(52-k);
		    if((j<<(52-k))==ly) yisint = 2-(j&1);
		} else if(ly==0) {
		    j = iy>>(20-k);
		    if((j<<(20-k))==iy) yisint = 2-(j&1);
		}
	    }		
	} 

    /* special value of y */
	if(ly==0) { 	
	    if (iy==0x7ff00000) {	/* y is +-inf */
	        if(((ix-0x3ff00000)|lx)==0)
		    return  one;	/* (-1)**+-inf is NaN */
	        else if (ix >= 0x3ff00000)/* (|x|>1)**+-inf = inf,0 */
		    return (hy>=0)? y: zero;
	        else			/* (|x|<1)**-,+inf = inf,0 */
		    return (hy<0)?-y: zero;
	    } 
	    if(iy==0x3ff00000) {	/* y is  +-1 */
		if(hy<0) return one/x; else return x;
	    }
            if(hy==0x40000000) return x*x;   /* y is  2 */
            if(hy==0x40080000) return x*x*x; /* y is  3 */
            if(hy==0x40100000) {             /* y is  4 */
                u = x*x;
                return u*u;
            }
	    if(hy==0x3fe00000) {             /* y is  0.5 */
		if(hx>=0)	/* x >= +0 */
                    return sqrt(x);
	    }
	}

	ax   = fabs(x);
    /* special value of x */
	if(lx==0) {
	    if(ix==0x7ff00000||ix==0||ix==0x3ff00000){
		z = ax;			/*x is +-0,+-inf,+-1*/
		if(hy<0) z = one/z;	/* z = (1/|x|) */
		if(hx<0) {
		    if(((ix-0x3ff00000)|yisint)==0) {
			z = (z-z)/(z-z); /* (-1)**non-int is NaN */
		    } else if(yisint==1) 
			z = -z;		/* (x<0)**odd = -(|x|**odd) */
		}
		return z;
	    }
	}
    
    /* CYGNUS LOCAL + fdlibm-5.3 fix: This used to be
	n = (hx>>31)+1;
       but ANSI C says a right shift of a signed negative quantity is
       implementation defined.  */
	n = ((u_int32_t)hx>>31)-1;

    /* (x<0)**(non-int) is NaN */
	if((n|yisint)==0) return (x-x)/(x-x);

	s = one; /* s (sign of result -ve**odd) = -1 else = 1 */
	if((n|(yisint-1))==0) s = -one;/* (-ve)**(odd int) */

    /* |y| is huge */
	if(iy>0x41e00000) { /* if |y| > 2**31 */
	    if(iy>0x43f00000){	/* if |y| > 2**64, must o/uflow */
		if(ix<=0x3fefffff) return (hy<0)? huge*huge:tiny*tiny;
		if(ix>=0x3ff00000) return (hy>0)? huge*huge:tiny*tiny;
	    }
	/* over/underflow if x is not close to one */
	    if(ix<0x3fefffff) return (hy<0)? s*huge*huge:s*tiny*tiny;
	    if(ix>0x3ff00000) return (hy>0)? s*huge*huge:s*tiny*tiny;
	/* now |1-x| is tiny <= 2**-20, suffice to compute 
	   log(x) by x-x^2/2+x^3/3-x^4/4 */
	    t = ax-one;		/* t has 20 trailing zeros */
	    w = (t*t)*(0.5-t*(0.3333333333333333333333-t*0.25));
	    u = ivln2_h*t;	/* ivln2_h has 21 sig. bits */
	    v = t*ivln2_l-w*ivln2;
	    t1 = u+v;
	    SET_LOW_WORD(t1,0);
	    t2 = v-(t1-u);
	} else {
	    double ss,s2,s_h,s_l,t_h,t_l;
	    n = 0;
	/* take care subnormal number */
	    if(ix<0x00100000)
		{ax *= two53; n -= 53; GET_HIGH_WORD(ix,ax); }
	    n  += ((ix)>>20)-0x3ff;
	    j  = ix&0x000fffff;
	/* determine interval */
	    ix = j|0x3ff00000;		/* normalize ix */
	    if(j<=0x3988E) k=0;		/* |x|<sqrt(3/2) */
	    else if(j<0xBB67A) k=1;	/* |x|<sqrt(3)   */
	    else {k=0;n+=1;ix -= 0x00100000;}
	    SET_HIGH_WORD(ax,ix);

	/* compute ss = s_h+s_l = (x-1)/(x+1) or (x-1.5)/(x+1.5) */
	    u = ax-bp[k];		/* bp[0]=1.0, bp[1]=1.5 */
	    v = one/(ax+bp[k]);
	    ss = u*v;
	    s_h = ss;
	    SET_LOW_WORD(s_h,0);
	/* t_h=ax+bp[k] High */
	    t_h = zero;
	    SET_HIGH_WORD(t_h,((ix>>1)|0x20000000)+0x00080000+(k<<18));
	    t_l = ax - (t_h-bp[k]);
	    s_l = v*((u-s_h*t_h)-s_h*t_l);
	/* compute log(ax) */
	    s2 = ss*ss;
	    r = s2*s2*(L1+s2*(L2+s2*(L3+s2*(L4+s2*(L5+s2*L6)))));
	    r += s_l*(s_h+ss);
	    s2  = s_h*s_h;
	    t_h = 3.0+s2+r;
	    SET_LOW_WORD(t_h,0);
	    t_l = r-((t_h-3.0)-s2);
	/* u+v = ss*(1+...) */
	    u = s_h*t_h;
	    v = s_l*t_h+t_l*ss;
	/* 2/(3log2)*(ss+...) */
	    p_h = u+v;
	    SET_LOW_WORD(p_h,0);
	    p_l = v-(p_h-u);
	    z_h = cp_h*p_h;		/* cp_h+cp_l = 2/(3*log2) */
	    z_l = cp_l*p_h+p_l*cp+dp_l[k];
	/* log2(ax) = (ss+..)*2/(3*log2) = n + dp_h + z_h + z_l */
	    t = (double)n;
	    t1 = (((z_h+z_l)+dp_h[k])+t);
	    SET_LOW_WORD(t1,0);
	    t2 = z_l-(((t1-t)-dp_h[k])-z_h);
	}

    /* split up y into y1+y2 and compute (y1+y2)*(t1+t2) */
	y1  = y;
	SET_LOW_WORD(y1,0);
	p_l = (y-y1)*t1+y*t2;
	p_h = y1*t1;
	z = p_l+p_h;
	EXTRACT_WORDS(j,i,z);
	if (j>=0x40900000) {				/* z >= 1024 */
	    if(((j-0x40900000)|i)!=0)			/* if z > 1024 */
		return s*huge*huge;			/* overflow */
	    else {
		if(p_l+ovt>z-p_h) return s*huge*huge;	/* overflow */
	    }
	} else if((j&0x7fffffff)>=0x4090cc00 ) {	/* z <= -1075 */
	    if(((j-0xc090cc00)|i)!=0) 		/* z < -1075 */
		return s*tiny*tiny;		/* underflow */
	    else {
		if(p_l<=z-p_h) return s*tiny*tiny;	/* underflow */
	    }
	}
    /*
     * compute 2**(p_h+p_l)
     */
	i = j&0x7fffffff;
	k = (i>>20)-0x3ff;
	n = 0;
	if(i>0x3fe00000) {		/* if |z| > 0.5, set n = [z+0.5] */
	    n = j+(0x00100000>>(k+1));
	    k = ((n&0x7fffffff)>>20)-0x3ff;	/* new k for n */
	    t = zero;
	    SET_HIGH_WORD(t,n&~(0x000fffff>>k));
	    n = ((n&0x000fffff)|0x00100000)>>(20-k);
	    if(j<0) n = -n;
	    p_h -= t;
	} 
	t = p_l+p_h;
	SET_LOW_WORD(t,0);
	u = t*lg2_h;
	v = (p_l-(t-p_h))*lg2+t*lg2_l;
	z = u+v;
	w = v-(z-u);
	t  = z*z;
	t1  = z - t*(P1+t*(P2+t*(P3+t*(P4+t*P5))));
	r  = (z*t1)/(t1-two)-(w+z*w);
	z  = one-(r-z);
	GET_HIGH_WORD(j,z);
	j += (n<<20);
	if((j>>20)<=0) z = scalbn(z,n);	/* subnormal output */
	else SET_HIGH_WORD(z,j);
	return s*z;
}

double log(double x)
{
	double hfsq,f,s,z,R,w,t1,t2,dk;
	int32_t k,hx,i,j;
	u_int32_t lx;

	EXTRACT_WORDS(hx,lx,x);

	k=0;
	if (hx < 0x00100000) {			/* x < 2**-1022  */
	    if (((hx&0x7fffffff)|lx)==0) 
		return -two54/zero;		/* log(+-0)=-inf */
	    if (hx<0) return (x-x)/zero;	/* log(-#) = NaN */
	    k -= 54; x *= two54; /* subnormal number, scale up x */
	    GET_HIGH_WORD(hx,x);
	} 
	if (hx >= 0x7ff00000) return x+x;
	k += (hx>>20)-1023;
	hx &= 0x000fffff;
	i = (hx+0x95f64)&0x100000;
	SET_HIGH_WORD(x,hx|(i^0x3ff00000));	/* normalize x or x/2 */
	k += (i>>20);
	f = x-1.0;
	if((0x000fffff&(2+hx))<3) {	/* -2**-20 <= f < 2**-20 */
	    if(f==zero) {
		if(k==0) {
		    return zero;
		} else {
		    dk=(double)k;
		    return dk*ln2_hi+dk*ln2_lo;
		}
	    }
	    R = f*f*(0.5-0.33333333333333333*f);
	    if(k==0) return f-R; else {dk=(double)k;
	    	     return dk*ln2_hi-((R-dk*ln2_lo)-f);}
	}
 	s = f/(2.0+f); 
	dk = (double)k;
	z = s*s;
	i = hx-0x6147a;
	w = z*z;
	j = 0x6b851-hx;
	t1= w*(Lg2+w*(Lg4+w*Lg6)); 
	t2= z*(Lg1+w*(Lg3+w*(Lg5+w*Lg7))); 
	i |= j;
	R = t2+t1;
	if(i>0) {
	    hfsq=0.5*f*f;
	    if(k==0) return f-(hfsq-s*(hfsq+R)); else
		     return dk*ln2_hi-((hfsq-(s*(hfsq+R)+dk*ln2_lo))-f);
	} else {
	    if(k==0) return f-s*(f-R); else
		     return dk*ln2_hi-((s*(f-R)-dk*ln2_lo)-f);
	}
}

double exp(double x)	/* default IEEE double exp */
{
	static const double
one	= 1.0,
halF[2]	= {0.5,-0.5,},
huge	= 1.0e+300,
o_threshold=  7.09782712893383973096e+02,  /* 0x40862E42, 0xFEFA39EF */
u_threshold= -7.45133219101941108420e+02,  /* 0xc0874910, 0xD52D3051 */
ln2HI[2]   ={ 6.93147180369123816490e-01,  /* 0x3fe62e42, 0xfee00000 */
	     -6.93147180369123816490e-01,},/* 0xbfe62e42, 0xfee00000 */
ln2LO[2]   ={ 1.90821492927058770002e-10,  /* 0x3dea39ef, 0x35793c76 */
	     -1.90821492927058770002e-10,},/* 0xbdea39ef, 0x35793c76 */
invln2 =  1.44269504088896338700e+00, /* 0x3ff71547, 0x652b82fe */
P1   =  1.66666666666666019037e-01, /* 0x3FC55555, 0x5555553E */
P2   = -2.77777777770155933842e-03, /* 0xBF66C16C, 0x16BEBD93 */
P3   =  6.61375632143793436117e-05, /* 0x3F11566A, 0xAF25DE2C */
P4   = -1.65339022054652515390e-06, /* 0xBEBBBD41, 0xC5D26BF1 */
P5   =  4.13813679705723846039e-08; /* 0x3E663769, 0x72BEA4D0 */

static volatile double
twom1000= 9.33263618503218878990e-302;     /* 2**-1000=0x01700000,0*/


	double y,hi=0.0,lo=0.0,c,t,twopk;
	int32_t k=0,xsb;
	u_int32_t hx;

	GET_HIGH_WORD(hx,x);
	xsb = (hx>>31)&1;		/* sign bit of x */
	hx &= 0x7fffffff;		/* high word of |x| */

    /* filter out non-finite argument */
	if(hx >= 0x40862E42) {			/* if |x|>=709.78... */
            if(hx>=0x7ff00000) {
	        u_int32_t lx;
		GET_LOW_WORD(lx,x);
		if(((hx&0xfffff)|lx)!=0)
		     return x+x; 		/* NaN */
		else return (xsb==0)? x:0.0;	/* exp(+-inf)={inf,0} */
	    }
	    if(x > o_threshold) return huge*huge; /* overflow */
	    if(x < u_threshold) return twom1000*twom1000; /* underflow */
	}

        /* this implementation gives 2.7182818284590455 for exp(1.0),
           which is well within the allowable error. however,
           2.718281828459045 is closer to the true value so we prefer that
           answer, given that 1.0 is such an important argument value. */
        if (x == 1.0)
            return 2.718281828459045235360;

    /* argument reduction */
	if(hx > 0x3fd62e42) {		/* if  |x| > 0.5 ln2 */ 
	    if(hx < 0x3FF0A2B2) {	/* and |x| < 1.5 ln2 */
		hi = x-ln2HI[xsb]; lo=ln2LO[xsb]; k = 1-xsb-xsb;
	    } else {
		k  = (int)(invln2*x+halF[xsb]);
		t  = k;
		hi = x - t*ln2HI[0];	/* t*ln2HI is exact here */
		lo = t*ln2LO[0];
	    }
	    STRICT_ASSIGN(double, x, hi - lo);
	} 
	else if(hx < 0x3e300000)  {	/* when |x|<2**-28 */
	    if(huge+x>one) return one+x;/* trigger inexact */
	}
	else k = 0;

    /* x is now in primary range */
	t  = x*x;
	if(k >= -1021)
	    INSERT_WORDS(twopk,0x3ff00000+(k<<20), 0);
	else
	    INSERT_WORDS(twopk,0x3ff00000+((k+1000)<<20), 0);
	c  = x - t*(P1+t*(P2+t*(P3+t*(P4+t*P5))));
	if(k==0) 	return one-((x*c)/(c-2.0)-x); 
	else 		y = one-((lo-(x*c)/(2.0-c))-hi);
	if(k >= -1021) {
	    if (k==1024) return y*2.0*0x1p1023;
	    return y*twopk;
	} else {
	    return y*twopk*twom1000;
	}
}
