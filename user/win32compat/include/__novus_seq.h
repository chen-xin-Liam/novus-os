#ifndef NOVUS_SEQ_H
#define NOVUS_SEQ_H
#include "__novus_rt.h"
#include <cstddef>
namespace std {

/* list: dynamic array with push_back/front */
template <class T> class list {
  T* a; novus_size_t n, cap;
  void grow(novus_size_t need){ if(cap>=need)return; novus_size_t nc=cap?cap*2:4; while(nc<need)nc*=2; T* b=(T*)novus_alloc(nc*sizeof(T)); for(novus_size_t i=0;i<n;i++)b[i]=a[i]; novus_free(a); a=b; cap=nc; }
public:
  typedef T value_type; typedef T* iterator;
  list():a(0),n(0),cap(0){}
  ~list(){ novus_free(a); }
  void push_back(const T& v){ grow(n+1); a[n++]=v; }
  void push_front(const T& v){ grow(n+1); for(novus_size_t j=n;j>0;j--)a[j]=a[j-1]; a[0]=v; n++; }
  T& front(){ return a[0]; }
  T& back(){ return a[n-1]; }
  novus_size_t size() const { return n; }
  bool empty() const { return n==0; }
  iterator begin(){ return a; } iterator end(){ return a+n; }
};

/* deque: array with push_front/push_back and operator[] */
template <class T> class deque {
  T* a; novus_size_t n, cap;
  void grow(novus_size_t need){ if(cap>=need)return; novus_size_t nc=cap?cap*2:4; while(nc<need)nc*=2; T* b=(T*)novus_alloc(nc*sizeof(T)); for(novus_size_t i=0;i<n;i++)b[i]=a[i]; novus_free(a); a=b; cap=nc; }
public:
  typedef T value_type; typedef T* iterator;
  deque():a(0),n(0),cap(0){}
  ~deque(){ novus_free(a); }
  void push_back(const T& v){ grow(n+1); a[n++]=v; }
  void push_front(const T& v){ grow(n+1); for(novus_size_t j=n;j>0;j--)a[j]=a[j-1]; a[0]=v; n++; }
  T& operator[](novus_size_t i){ return a[i]; }
  const T& operator[](novus_size_t i) const { return a[i]; }
  T& front(){ return a[0]; } T& back(){ return a[n-1]; }
  novus_size_t size() const { return n; }
  iterator begin(){ return a; } iterator end(){ return a+n; }
};

/* queue adapter over deque */
template <class T, class C = deque<T> > class queue {
  C c;
public:
  typedef T value_type;
  void push(const T& v){ c.push_back(v); }
  void pop(){ /* simplified */ }
  T& front(){ return c[0]; }
  const T& front() const { return c[0]; }
  novus_size_t size() const { return c.size(); }
  bool empty() const { return c.size()==0; }
};

/* stack adapter over deque */
template <class T, class C = deque<T> > class stack {
  C c;
public:
  typedef T value_type;
  void push(const T& v){ c.push_back(v); }
  void pop(){ }
  T& top(){ return c[c.size()-1]; }
  const T& top() const { return c[c.size()-1]; }
  novus_size_t size() const { return c.size(); }
  bool empty() const { return c.size()==0; }
};

} /* namespace std */
#endif
