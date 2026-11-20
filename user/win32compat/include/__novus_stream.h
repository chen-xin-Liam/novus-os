#ifndef NOVUS_STREAM_H
#define NOVUS_STREAM_H
#include "__novus_rt.h"
#include "string"
namespace std {

struct ios_base {};
struct ostream;

/* endl is a manipulator (function pointer). */
inline ostream& endl(ostream& os);

struct ostream : ios_base {
  ostream& operator<<(const char* s){ novus_puts(s); return *this; }
  ostream& operator<<(const string& s){ novus_puts(s.c_str()); return *this; }
  ostream& operator<<(int v){ novus_puti(v); return *this; }
  ostream& operator<<(unsigned int v){ novus_putu(v); return *this; }
  ostream& operator<<(long v){ novus_puti(v); return *this; }
  ostream& operator<<(unsigned long v){ novus_putu(v); return *this; }
  ostream& operator<<(long long v){ novus_puti(v); return *this; }
  ostream& operator<<(double v){ novus_puti((long long)v); novus_putc('.'); novus_puti((long long)((v-(long long)v)*10)); return *this; }
  ostream& operator<<(char c){ novus_putc(c); return *this; }
  ostream& operator<<(ostream& (*f)(ostream&)){ return f(*this); }
  ostream& put(char c){ novus_putc(c); return *this; }
  ostream& flush(){ return *this; }
};
inline ostream& endl(ostream& os){ novus_putc('\n'); return os; }
inline ostream& ends(ostream& os){ novus_putc(0); return os; }

/* Minimal stringstream: holds a char buffer, supports << and >>. */
struct stringstream : ostream {
  char buf[512]; novus_size_t pos, rpos;
  stringstream():pos(0),rpos(0){ buf[0]=0; }
  using ostream::operator<<;
  stringstream& operator<<(const char* s){ novus_size_t n=novus_strlen(s); if(pos+n<sizeof(buf)){novus_memcpy(buf+pos,s,n);pos+=n;buf[pos]=0;} return *this; }
  stringstream& operator<<(int v){ char b[32]; novus_size_t n=novus_itoa(v,b,32); if(pos+n<sizeof(buf)){novus_memcpy(buf+pos,b,n);pos+=n;buf[pos]=0;} return *this; }
  stringstream& operator<<(ostream& (*f)(ostream&)){ f(*this); if(pos+1<sizeof(buf)){buf[pos++]='\n';buf[pos]=0;} return *this; }
  stringstream& operator>>(int& out){
    while(rpos<pos && (buf[rpos]==' '||buf[rpos]=='\n'))rpos++;
    long long v=0; bool neg=false; if(rpos<pos&&buf[rpos]=='-'){neg=true;rpos++;}
    while(rpos<pos&&buf[rpos]>='0'&&buf[rpos]<='9'){ v=v*10+(buf[rpos]-'0'); rpos++; }
    out=(int)(neg?-v:v); return *this;
  }
  stringstream& operator>>(string& out){ while(rpos<pos&&buf[rpos]==' ')rpos++; novus_size_t s=rpos; while(rpos<pos&&buf[rpos]!=' '&&buf[rpos]!='\n')rpos++; out=string(buf+s,rpos-s); return *this; }
  const char* str() const { return buf; }
};

/* fstream minimal: ofstream writes to a named file via a simple in-memory
 * staging buffer keyed by filename (Novus test writes then reads "_cpp98_test.tmp"). */
struct ofstream : ostream {
  const char* name; char buf[128]; novus_size_t pos;
  explicit ofstream(const char* n):name(n),pos(0){ buf[0]=0; }
  using ostream::operator<<;
  ofstream& operator<<(const char* s){ novus_size_t n=novus_strlen(s); if(pos+n<sizeof(buf)){novus_memcpy(buf+pos,s,n);pos+=n;buf[pos]=0;} return *this; }
  void close(){ novus_file_write(name, buf, pos); }
};
struct ifstream : ostream {
  const char* name; char buf[128]; novus_size_t rpos;
  explicit ifstream(const char* n):name(n),rpos(0){ buf[0]=0; novus_file_read(name,buf,sizeof(buf)-1); }
  using ostream::operator<<;
  ifstream& operator>>(string& out){ while(rpos<sizeof(buf)&&buf[rpos]==' ')rpos++; novus_size_t s=rpos; while(rpos<sizeof(buf)&&buf[rpos]&&buf[rpos]!=' '&&buf[rpos]!='\n')rpos++; out=string(buf+s,rpos-s); return *this; }
  void close(){}
};

static ostream cout_obj;
static ostream cerr_obj;
static ostream clog_obj;
static ostream& cout = cout_obj;
static ostream& cerr = cerr_obj;
static ostream& clog = clog_obj;

} /* namespace std */
#endif
