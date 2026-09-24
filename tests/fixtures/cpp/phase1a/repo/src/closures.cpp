// Phase 1C fixture: closure member folding, hidden closure members, unnamed
// constructors, collision-safe local type atoms and namespace-scope lambdas.
// Header-free; analysed a second time from a copied root with inserted lines
// to prove identities carry no path or coordinate.

struct Payload {
  int value;
  Payload();
  Payload(const Payload&);
  Payload(Payload&&);
  Payload& operator=(const Payload&);
  Payload& operator=(Payload&&);
  ~Payload();
};

// --- namespace-scope declarator lambdas ---------------------------------------
auto g1 = [] { return 1; };
auto g2 = [] { return 2; };
namespace ns {
auto g3 = [](int v) { return v; };
}
auto pair_init = ([] { return 3; }, [] { return 4; });  // two closures in one declarator; only the second is pair_init

void consume(decltype(g1)) {}
void consume(decltype(g2)) {}

// --- unnamed records ------------------------------------------------------------
struct {
  Payload payload;
} a1;
struct {
  Payload payload;
} a2;
void consume_anon(decltype(a1)) {}
void consume_anon(decltype(a2)) {}
void consume_ptr(decltype(g1)* p, const decltype(a1)& r, decltype(g1) (*fp)(decltype(a2)[2]), decltype(a2) (&arr)[2]) {
  (void)p;
  (void)r;
  (void)fp;
  (void)arr;
}

// --- template argument kinds and values -----------------------------------------
template <auto V>
struct Wrap {
  int w;
};
template <class T>
struct Box {
  T t;
};
struct K {
  int k;
};
void take_wrap(Wrap<1> a, Wrap<1u> b, Wrap<K{1}> c, Wrap<K{2}> d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
}
void use_memptr(int Box<decltype(g1)>::* a, int Box<decltype(g2)>::* b) {  // member-pointer class keeps its arguments
  (void)a;
  (void)b;
}

// --- same-named local types in separate blocks ----------------------------------
int local_types() {
  int total = 0;
  {
    struct L {
      int a;
    };
    enum E { X };
    auto lam = [](L l, E e, Box<L> b) { return l.a + e + b.t.a; };
    total += lam(L{1}, X, Box<L>{L{2}});
  }
  {
    struct L {
      int b;
    };
    enum E { Y };
    auto lam = [](L l, E e, Box<L> b) { return l.b + e + b.t.b; };
    total += lam(L{3}, Y, Box<L>{L{4}});
  }
  return total;
}

// --- function ABI in local-type-containing signatures --------------------------
void abi(void (*plain)(decltype(g1)), void (*nothrow)(decltype(g1)) noexcept, void(__vectorcall* vc)(decltype(g1))) {
  (void)plain;
  (void)nothrow;
  (void)vc;
}

// --- closure members ------------------------------------------------------------
int exercise(Payload payload) {
  auto captured = [payload] { return payload.value; };
  auto copied = captured;                                      // hidden closure copy constructor (copies Payload)
  auto moved = static_cast<decltype(captured)&&>(captured);   // hidden closure move constructor
  auto empty = [] { return 5; };
  auto empty_copy = empty;
  empty_copy = empty;                                          // hidden copy assignment, operator syntax
  empty_copy.operator=(static_cast<decltype(empty)&&>(empty));  // hidden move assignment, member syntax
  auto generic = [](auto x) { return x; };
  int a = copied();                                            // real call operator: folds into the lambda
  int b = copied.operator()();                                 // member syntax: same fold
  int c = generic(1);                                          // generic specialization <int>
  int d = generic.operator()(2.5);                             // generic specialization <double>, member syntax
  int e = [] { return 6; }();                                  // immediately invoked
  int (*fp)() = empty;                                         // hidden conversion function
  decltype(a1) anonymous{};
  auto anonymous_copy = anonymous;                             // hidden (ctor)(const [anon] &)
  auto anonymous_move = static_cast<decltype(anonymous)&&>(anonymous);  // hidden (ctor)([anon] &&)
  return a + b + c + d + e + fp() + anonymous_copy.payload.value + anonymous_move.payload.value + g1() + ns::g3(1) +
         pair_init() + empty_copy() + moved();
}

void assign_only(decltype(g1)& x, decltype(g1)& y) { x = y; }  // zero enclosing-to-lambda execution calls
