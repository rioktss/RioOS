// Host test: RioOS libm (soft-float based) against the host libm. Reports max error in ulps.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdlib>
extern "C" {
double rio_fabs(double);
double rio_floor(double);
double rio_ceil(double);
double rio_trunc(double);
double rio_round(double);
double rio_rint(double);
double rio_exp(double);
double rio_log(double);
double rio_log2(double);
double rio_log10(double);
double rio_sqrt(double);
double rio_cbrt(double);
double rio_sin(double);
double rio_cos(double);
double rio_tan(double);
double rio_atan(double);
double rio_asin(double);
double rio_acos(double);
double rio_sinh(double);
double rio_cosh(double);
double rio_tanh(double);
double rio_expm1(double);
double rio_log1p(double);
double rio_pow(double,double); double rio_atan2(double,double); double rio_fmod(double,double); double rio_ldexp(double,int); double rio_frexp(double,int*); double rio_hypot(double,double);
}
static uint64_t bits(double d){uint64_t r;memcpy(&r,&d,8);return r;}
static double ulps(double a,double b){ if(std::isnan(a)&&std::isnan(b)) return 0; if(a==b) return 0; if(std::isinf(a)||std::isinf(b)||std::isnan(a)||std::isnan(b)) return 1e9;
 int64_t x=bits(a),y=bits(b); if(x<0) x=INT64_MIN-x; if(y<0) y=INT64_MIN-y; return (double)llabs(x-y); }
static uint64_t rng=88172645463325252ULL; static double u(){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (double)(rng>>11)/9007199254740992.0;}
static int fails=0;
static void chk(const char*n,double got,double want,double tol,double x){ double e=ulps(got,want); if(e>tol){ if(++fails<30) printf("FAIL %s(%.17g) got %.17g want %.17g (%g ulp)\n",n,x,got,want,e);} }
#define T1(N,LO,HI,TOL) for(int i=0;i<200000;++i){ double x=LO+(HI-LO)*u(); chk(#N,rio_##N(x),std::N(x),TOL,x);} 
int main(){
 T1(exp,-50,50,4) T1(exp,-700,700,8) T1(log,1e-5,1e5,4) T1(log,0.5,2,4) T1(log2,1e-3,1e3,6) T1(log10,1e-3,1e3,6)
 T1(sin,-100,100,6) T1(cos,-100,100,6) T1(tan,-1.5,1.5,10) T1(atan,-100,100,4) T1(asin,-1,1,8) T1(acos,-1,1,8)
 T1(sinh,-10,10,10) T1(cosh,-10,10,10) T1(tanh,-5,5,10) T1(cbrt,-1000,1000,4) T1(floor,-1e6,1e6,0) T1(ceil,-1e6,1e6,0) T1(trunc,-1e6,1e6,0) T1(round,-1e6,1e6,0) T1(sqrt,0,1e6,0)
 for(int i=0;i<200000;++i){ double x=u()*20+0.01,y=u()*20-10; chk("pow",rio_pow(x,y),std::pow(x,y),16,x);} 
 for(int i=0;i<2000;++i){ int a=(int)(u()*40)-20, b=(int)(u()*10)+1; chk("powint",rio_pow((double)a,(double)b),std::pow((double)a,(double)b),0,a);} 
 for(int i=0;i<200000;++i){ double y=u()*20-10,x=u()*20-10; chk("atan2",rio_atan2(y,x),std::atan2(y,x),6,x);} 
 for(int i=0;i<200000;++i){ double x=u()*2000-1000,y=u()*30+0.1; chk("fmod",rio_fmod(x,y),std::fmod(x,y),0,x);} 
 for(int i=0;i<100000;++i){ double x=u()*1e6-5e5; int k=(int)(u()*400)-200; chk("ldexp",rio_ldexp(x,k),std::ldexp(x,k),0,x);} 
 chk("ldexp-sub",rio_ldexp(1.5,-1074),std::ldexp(1.5,-1074),0,0); chk("ldexp-sub2",rio_ldexp(1.0,-1060),std::ldexp(1.0,-1060),0,0);
 chk("pow2",rio_pow(2,10),1024,0,2); chk("pow10",rio_pow(10,-2),0.01,0,10);
 printf("libm host test: %d failed\n",fails); return fails?1:0; }
