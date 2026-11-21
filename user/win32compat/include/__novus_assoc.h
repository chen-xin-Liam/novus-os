#ifndef NOVUS_ASSOC_H
#define NOVUS_ASSOC_H
#include "__novus_rt.h"
#include <cstddef>
namespace std {

template <class K, class V> struct pair {
  K first; V second;
  pair():first(),second(){}
  pair(const K& k,const V& v):first(k),second(v){}
  template <class K2, class V2> pair(const pair<K2,V2>& o):first(o.first),second(o.second){}
};
/* Force decay of string literals to std::string-like value via a helper. */
template <class K, class V> inline pair<K,V> make_pair(K k, V v){ return pair<K,V>(k,v); }

template <class T> struct less { bool operator()(const T& a,const T& b) const { return a<b; } };

/* Simple sorted-array set. Insert keeps ascending order; count finds. */
template <class T, class Cmp = less<T> > class set {
  T* a; novus_size_t n, cap;
  Cmp cmp;
  void grow(novus_size_t need){ if(cap>=need)return; novus_size_t nc=cap?cap*2:4; while(nc<need)nc*=2; T* b=(T*)novus_alloc(nc*sizeof(T)); for(novus_size_t i=0;i<n;i++)b[i]=a[i]; novus_free(a); a=b; cap=nc; }
  novus_size_t lower(const T& v) const { novus_size_t i=0; while(i<n && cmp(a[i],v)) i++; return i; }
public:
  typedef T value_type; typedef T* iterator;
  set():a(0),n(0),cap(0){}
  ~set(){ novus_free(a); }
  void insert(const T& v){ novus_size_t i=lower(v); if(i<n&&!cmp(v,a[i])) return; grow(n+1); for(novus_size_t j=n;j>i;j--)a[j]=a[j-1]; a[i]=v; n++; }
  novus_size_t count(const T& v) const { novus_size_t i=lower(v); return (i<n&&!cmp(v,a[i]))?1:0; }
  bool contains(const T& v) const { return count(v)!=0; }
  novus_size_t size() const { return n; }
  iterator begin(){ return a; } iterator end(){ return a+n; }
};

/* Simple sorted-array map with operator[]. */
template <class K, class V, class Cmp = less<K> > class map {
  struct ent { K k; V v; };
  ent* a; novus_size_t n, cap;
  Cmp cmp;
  void grow(novus_size_t need){ if(cap>=need)return; novus_size_t nc=cap?cap*2:4; while(nc<need)nc*=2; ent* b=(ent*)novus_alloc(nc*sizeof(ent)); for(novus_size_t i=0;i<n;i++)b[i]=a[i]; novus_free(a); a=b; cap=nc; }
  novus_size_t lower(const K& k) const { novus_size_t i=0; while(i<n && cmp(a[i].k,k)) i++; return i; }
public:
  typedef K key_type; typedef V mapped_type; typedef pair<K,V> value_type;
  map():a(0),n(0),cap(0){}
  ~map(){ novus_free(a); }
  V& operator[](const K& k){ novus_size_t i=lower(k); if(i>=n || cmp(k,a[i].k)){ grow(n+1); for(novus_size_t j=n;j>i;j--)a[j]=a[j-1]; a[i].k=k; a[i].v=V(); n++; } return a[i].v; }
  novus_size_t count(const K& k) const { novus_size_t i=lower(k); return (i<n&&!cmp(k,a[i].k))?1:0; }
  novus_size_t size() const { return n; }
};

} /* namespace std */
#endif
