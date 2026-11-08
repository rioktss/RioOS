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

// Host test: RioOS deflate/inflate/gzip against zlib/python.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <zlib.h>
#include <initializer_list>
extern "C" { int rio_deflate(const uint8_t*,unsigned long,uint8_t**,unsigned long*,int); int rio_inflate(const uint8_t*,unsigned long,uint8_t**,unsigned long*,unsigned long*);
 int rio_gzip_compress(const uint8_t*,unsigned long,const char*,uint32_t,uint8_t**,unsigned long*,int); int rio_gzip_decompress(const uint8_t*,unsigned long,uint8_t**,unsigned long*,char*,int); uint32_t rio_crc32(uint32_t,const uint8_t*,unsigned long); }
static uint64_t rng=88172645463325252ULL; static uint64_t rnd(){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
int main(){ int fails=0,total=0;
 for(int t=0;t<400;++t){
  unsigned long n = t<10 ? t : (rnd()%(t<300?20000:300000)); uint8_t* d=(uint8_t*)malloc(n+1);
  int kind=t%5;
  for(unsigned long i=0;i<n;++i){ switch(kind){case 0:d[i]=rnd();break;case 1:d[i]="abcdefgh"[rnd()%8];break;case 2:d[i]=(i%97)+(rnd()%3==0);break;case 3:d[i]=i<n/2?'x':rnd();break;default:{ const char* w[]={"the ","quick ","brown ","fox ","jumps ","over ","lazy ","dog\n"}; static unsigned long wp=0; static const char* cur=w[0]; if(!*cur){cur=w[rnd()%8];} d[i]=*cur++; (void)wp;}}}
  for(int level: {1,6,9}){
   uint8_t* c; unsigned long cl; ++total; if(rio_deflate(d,n,&c,&cl,level)){++fails;printf("deflate fail\n");continue;}
   // zlib inflates ours
   z_stream z; memset(&z,0,sizeof z); inflateInit2(&z,-15); uint8_t* o=(uint8_t*)malloc(n+16); z.next_in=c; z.avail_in=cl; z.next_out=o; z.avail_out=n+16; int r=inflate(&z,Z_FINISH);
   if(r!=Z_STREAM_END||z.total_out!=n||memcmp(o,d,n)){++fails;printf("FAIL zlib-inflates-ours t=%d level=%d kind=%d n=%lu r=%d\n",t,level,kind,n,r);} inflateEnd(&z);
   // ours inflates ours
   uint8_t* o2; unsigned long ol; unsigned long used; ++total; if(rio_inflate(c,cl,&o2,&ol,&used)||ol!=n||memcmp(o2,d,n)){++fails;printf("FAIL self-roundtrip t=%d\n",t);} else free(o2);
   free(c); free(o);
  }
  // zlib deflates, ours inflates
  for(int lv : {0,1,9}){ unsigned long bound=compressBound(n); uint8_t* zc=(uint8_t*)malloc(bound); z_stream z; memset(&z,0,sizeof z); deflateInit2(&z,lv,Z_DEFLATED,-15,8,Z_DEFAULT_STRATEGY); z.next_in=d; z.avail_in=n; z.next_out=zc; z.avail_out=bound; deflate(&z,Z_FINISH); unsigned long zl=z.total_out; deflateEnd(&z);
   uint8_t* o2; unsigned long ol; ++total; if(rio_inflate(zc,zl,&o2,&ol,0)||ol!=n||memcmp(o2,d,n)){++fails;printf("FAIL ours-inflates-zlib t=%d lv=%d\n",t,lv);} else free(o2); free(zc);}
  // gzip container
  { uint8_t* g; unsigned long gl; ++total; rio_gzip_compress(d,n,"file.txt",12345,&g,&gl,6); uint8_t* o2; unsigned long ol; char nm[64]; if(rio_gzip_decompress(g,gl,&o2,&ol,nm,64)||ol!=n||memcmp(o2,d,n)||strcmp(nm,"file.txt")){++fails;printf("FAIL gzip self t=%d\n",t);} else free(o2);
    { /* optional interop check against the system gzip(1); skipped when there is no writable temp dir or no gzip */
      const char* td=getenv("TMPDIR"); if(!td||!*td) td="."; char pg[300],pr[300]; snprintf(pg,sizeof pg,"%s/rio_t.gz",td); snprintf(pr,sizeof pr,"%s/rio_t.raw",td);
      FILE*f=fopen(pg,"wb"); if(f){ fwrite(g,1,gl,f); fclose(f); }
      if(f && t%20==0){ FILE*o=fopen(pr,"wb"); if(o){ fwrite(d,1,n,o); fclose(o); char cmd[700]; snprintf(cmd,sizeof cmd,"gzip -dc '%s' | cmp -s - '%s'",pg,pr);
        if(system("command -v gzip >/dev/null 2>&1")==0){ ++total; if(system(cmd)){++fails;printf("FAIL gzip(1) rejects ours t=%d\n",t);} } } }
      if(f){ remove(pg); remove(pr); } }
    free(g); }
  free(d);
 }
 printf("deflate host test: %d checks, %d failed\n",total,fails); return fails?1:0; }
