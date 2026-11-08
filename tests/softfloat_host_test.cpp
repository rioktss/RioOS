/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// Host test: soft-float core vs hardware doubles (bit exact for + - * / sqrt, conversions, comparisons).
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdlib>
typedef uint64_t u64;
extern "C" { u64 sf_add(u64,u64); u64 sf_sub(u64,u64); u64 sf_mul(u64,u64); u64 sf_div(u64,u64); u64 sf_sqrt(u64); int sf_cmp(u64,u64);
 u64 sf_from_u64(u64,int); uint32_t sf_f64_to_f32(u64); u64 sf_f32_to_f64(uint32_t);
 long long __fixdfdi(double); unsigned long long __fixunsdfdi(double); }
static u64 b(double d){u64 r;memcpy(&r,&d,8);return r;}
static double d(u64 r){double x;memcpy(&x,&r,8);return x;}
static u64 rng=88172645463325252ULL; static u64 rnd(){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
static u64 pick(){ u64 r=rnd(); switch(r%8){
  case 0: return rnd();                       // anything incl. NaN/Inf/subnormal
  case 1: return rnd()&0x800FFFFFFFFFFFFFULL; // subnormal
  case 2: return (rnd()&0x800FFFFFFFFFFFFFULL)|((u64)(1+rnd()%40)<<52);
  case 3: return b((double)(int)(rnd()%2000)-1000);
  case 4: return b((double)(rnd()%100000)/ (double)(1+rnd()%1000));
  case 5: return (rnd()&0x800FFFFFFFFFFFFFULL)|((u64)(1000+rnd()%48)<<52);
  case 6: return (rnd()&0x800FFFFFFFFFFFFFULL)|((u64)(2000+rnd()%47)<<52);
  default: return (rnd()&0x8000000000000000ULL)|((u64)(1023-30+rnd()%60)<<52)|(rnd()&0xFFFFFFFFFFFFFULL);} }
static bool same(u64 x,u64 y){ if(std::isnan(d(x))&&std::isnan(d(y))) return true; return x==y; }
int main(){ long fails=0,total=0;
 for(int i=0;i<3000000;++i){ u64 x=pick(),y=pick(); double a=d(x),c=d(y);
  struct{const char*n;u64 got,want;} t[]={{"add",sf_add(x,y),b(a+c)},{"sub",sf_sub(x,y),b(a-c)},{"mul",sf_mul(x,y),b(a*c)},{"div",sf_div(x,y),b(a/c)},{"sqrt",sf_sqrt(x),b(std::sqrt(a))}};
  for(auto&e:t){ ++total; if(!same(e.got,e.want)){ if(++fails<20) printf("FAIL %s %016llx %016llx got %016llx want %016llx\n",e.n,(unsigned long long)x,(unsigned long long)y,(unsigned long long)e.got,(unsigned long long)e.want);} }
  int cw = (a<c)?-1:(a>c)?1:(a==c)?0:2; ++total; if(sf_cmp(x,y)!=cw){ if(++fails<20) printf("FAIL cmp\n"); }
  long long iv=(long long)(rnd()>>(rnd()%64)); if(rnd()&1) iv=-iv; ++total; if(sf_from_u64(iv<0?(u64)0-(u64)iv:(u64)iv,iv<0)!=b((double)iv)){ if(++fails<20) printf("FAIL i2d %lld\n",iv);} 
  if(std::fabs(a)<9e18&&!std::isnan(a)){ ++total; if(__fixdfdi(a)!=(long long)a){ if(++fails<20) printf("FAIL d2i %g\n",a);} }
  ++total; float f=(float)a; if(!std::isnan(a)&&sf_f64_to_f32(x)!=*(uint32_t*)&f){ if(++fails<20) printf("FAIL d2f %016llx\n",(unsigned long long)x);} 
  uint32_t fb=(uint32_t)rnd(); float ff; memcpy(&ff,&fb,4); if(!std::isnan(ff)){ ++total; if(sf_f32_to_f64(fb)!=b((double)ff)){ if(++fails<20) printf("FAIL f2d %08x\n",fb);} }
 }
 printf("%ld checks, %ld failed\n",total,fails); return fails?1:0; }
