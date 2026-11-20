#ifndef NOVUS_VECTOR_H
#define NOVUS_VECTOR_H
#include "__novus_rt.h"
#include <cstddef>
namespace std {

template <class T> class vector {
  T* a; novus_size_t n, cap;
  void grow(novus_size_t need) {
    if (cap >= need) return;
    novus_size_t nc = cap ? cap*2 : 4; while (nc < need) nc*=2;
    T* b = (T*)novus_alloc(nc*sizeof(T));
    for (novus_size_t i=0;i<n;i++) b[i]=a[i];
    novus_free(a); a=b; cap=nc;
  }
public:
  typedef T value_type;
  typedef T* iterator;
  vector():a(0),n(0),cap(0){}
  ~vector(){ novus_free(a); }
  vector(const vector& o):a(0),n(0),cap(0){ grow(o.n); for(novus_size_t i=0;i<o.n;i++) a[i]=o.a[i]; n=o.n; }
  vector& operator=(const vector& o){ if(this!=&o){ novus_free(a); a=0;n=0;cap=0; grow(o.n); for(novus_size_t i=0;i<o.n;i++)a[i]=o.a[i]; n=o.n;} return *this; }
  void push_back(const T& v){ grow(n+1); a[n++]=v; }
  void pop_back(){ if(n)n--; }
  T& back(){ return a[n-1]; }
  T& front(){ return a[0]; }
  T& operator[](novus_size_t i){ return a[i]; }
  const T& operator[](novus_size_t i) const { return a[i]; }
  novus_size_t size() const { return n; }
  bool empty() const { return n==0; }
  void clear(){ n=0; }
  iterator begin(){ return a; }
  iterator end(){ return a+n; }
  const iterator begin() const { return a; }
  const iterator end() const { return a+n; }
};

} /* namespace std */
#endif
