#ifndef NOVUS_RT_H
#define NOVUS_RT_H
/* Novus self-hosted C++98 runtime core. Header-only; maps to a tiny set of
 * Win32 APIs the Novus kernel implements. G++ is used as a compiler only. */
typedef unsigned long long novus_size_t;
typedef long long          novus_ptrdiff_t;

extern "C" {
  void* __stdcall GetStdHandle(unsigned long which);
  unsigned long __stdcall WriteFile(void* h, const void* buf, unsigned long len,
                                    unsigned long* written, void* overlapped);
  int __stdcall SetConsoleTextAttribute(void* h, unsigned short attr);
  void* __stdcall GetProcessHeap(void);
  void* __stdcall HeapAlloc(void* heap, unsigned long flags, novus_size_t bytes);
  void* __stdcall HeapFree(void* heap, unsigned long flags, void* p);
  unsigned long long __stdcall GetTickCount64(void);
  void __stdcall Sleep(unsigned long ms);
}

inline void* novus_alloc(novus_size_t n) { return HeapAlloc(GetProcessHeap(), 0u, n ? n : 1u); }
inline void novus_free(void* p) { if (p) HeapFree(GetProcessHeap(), 0u, p); }
inline void* operator new(novus_size_t n) { return novus_alloc(n); }
inline void* operator new[](novus_size_t n) { return novus_alloc(n); }
inline void operator delete(void* p) { novus_free(p); }
inline void operator delete[](void* p) { novus_free(p); }

inline novus_size_t novus_strlen(const char* s) { novus_size_t n=0; while(s[n])n++; return n; }
inline int novus_strcmp(const char* a,const char* b){ while(*a&&*a==*b){a++;b++;} return (int)(unsigned char)*a-(int)(unsigned char)*b; }
inline int novus_strncmp(const char* a,const char* b,novus_size_t n){ while(n&&*a&&*a==*b){a++;b++;n--;} return n?(int)(unsigned char)*a-(int)(unsigned char)*b:0; }
inline char* novus_strcpy(char* d,const char* s){ char* r=d; while((*d++=*s++)){} return r; }
inline char* novus_strncpy(char* d,const char* s,novus_size_t n){ char* r=d; while(n&&(*d++=*s++))n--; while(n--)*d++=0; return r; }
inline void* novus_memcpy(void* d,const void* s,novus_size_t n){ unsigned char* x=(unsigned char*)d; const unsigned char* y=(const unsigned char*)s; while(n--)*x++=*y++; return d; }
inline void* novus_memmove(void* d,const void* s,novus_size_t n){ unsigned char* x=(unsigned char*)d; const unsigned char* y=(const unsigned char*)s; if(x<y){while(n--)*x++=*y++;}else{x+=n;y+=n;while(n--)*--x=*--y;} return d; }
inline void* novus_memset(void* d,int c,novus_size_t n){ unsigned char* x=(unsigned char*)d; while(n--)*x++=(unsigned char)c; return d; }
inline int novus_memcmp(const void* a,const void* b,novus_size_t n){ const unsigned char* x=(const unsigned char*)a; const unsigned char* y=(const unsigned char*)b; while(n){if(*x!=*y)return (int)*x-(int)*y;x++;y++;n--;} return 0; }
inline const char* novus_strchr(const char* s,int c){ while(*s){if(*s==(char)c)return s;s++;} return c==0?s:0; }

inline novus_size_t novus_utoa(unsigned long long v,char* b,novus_size_t cap){ char t[32]; novus_size_t n=0; if(!v){if(cap)b[0]='0';return 1;} while(v){t[n++]=char('0'+v%10);v/=10;} novus_size_t k=0; while(n&&k<cap)b[k++]=t[--n]; return k; }
inline novus_size_t novus_itoa(long long v,char* b,novus_size_t cap){ if(v<0){b[0]='-';return 1+novus_utoa((unsigned long long)(-v),b+1,cap?cap-1:0);} return novus_utoa((unsigned long long)v,b,cap); }

inline void novus_write(const char* s,novus_size_t n){ unsigned long w=0; void* h=GetStdHandle((unsigned long)-11); novus_size_t o=0; while(o<n){novus_size_t c=n-o; if(c>64)c=64; WriteFile(h,s+o,(unsigned long)c,&w,0); o+=c;} }
inline void novus_puts(const char* s){ novus_write(s,novus_strlen(s)); }
inline void novus_putc(char c){ novus_write(&c,1); }
inline void novus_puti(long long v){ char b[32]; novus_size_t n=novus_itoa(v,b,32); novus_write(b,n); }
inline void novus_putu(unsigned long long v){ char b[32]; novus_size_t n=novus_utoa(v,b,32); novus_write(b,n); }
inline void novus_putx(unsigned long long v){ static const char* H="0123456789abcdef"; char b[24]; novus_size_t n=0; if(!v){novus_putc('0');return;} while(v){b[n++]=H[v&15];v>>=4;} while(n)novus_putc(b[--n]); }

inline void novus_color(unsigned short a){ SetConsoleTextAttribute(GetStdHandle((unsigned long)-11),a); }
static unsigned long novus_rand_state = 1u;
inline unsigned long novus_rand(void){ novus_rand_state = novus_rand_state * 1103515245u + 12345u; return novus_rand_state & 0x7fffffffu; }
inline unsigned long long novus_ticks(void){ return GetTickCount64(); }

/* ---- temporary file staging (ofstream/ifstream test) -------------------- */
struct novus_staged_file { char name[64]; char data[256]; novus_size_t len; int used; };
static novus_staged_file novus_files[4];
inline void novus_file_write(const char* name, const char* data, novus_size_t n) {
  for (int i=0;i<4;i++) {
    if (novus_files[i].used && novus_strcmp(novus_files[i].name, name)==0) {
      novus_size_t k = n<255?n:255; novus_memcpy(novus_files[i].data,data,k); novus_files[i].data[k]=0; novus_files[i].len=k; return;
    }
  }
  for (int i=0;i<4;i++) if (!novus_files[i].used) {
    novus_files[i].used=1; novus_strncpy(novus_files[i].name,name,64);
    novus_size_t k=n<255?n:255; novus_memcpy(novus_files[i].data,data,k); novus_files[i].data[k]=0; novus_files[i].len=k; return;
  }
}
inline novus_size_t novus_file_read(const char* name, char* out, novus_size_t cap) {
  for (int i=0;i<4;i++) if (novus_files[i].used && novus_strcmp(novus_files[i].name,name)==0) {
    novus_size_t k=novus_files[i].len<cap?novus_files[i].len:cap; novus_memcpy(out,novus_files[i].data,k); out[k]=0; return k;
  }
  if (cap) out[0]=0; return 0;
}

/* sqrt via x64 SSE instruction, no libm. */
inline double novus_sqrt(double x){ double r; __asm__ __volatile__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }

#endif
