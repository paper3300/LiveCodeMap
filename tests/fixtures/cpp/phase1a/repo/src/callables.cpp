// Phase 1B fixture: callable declaration state, hidden implicit members,
// initializer/construction/conversion/operator attribution, compiler-CFG
// lifetime facts, captures and closure lifetime. Header-free on purpose so it
// compiles unchanged under the default Windows driver modes and an explicit
// alternate (Itanium) target.

// --- Family A: defaulted/deleted state and hidden implicit members ----------
struct Policy {
  Policy() = default;
  Policy(const Policy&) = delete;
  Policy(Policy&&) = default;
  ~Policy();
};
Policy::~Policy() = default;  // defaulted out of line: user-provided, flag only on this declaration

struct NoCopy {
  NoCopy();
  NoCopy(const NoCopy&) = delete;
};
struct Wrap {
  NoCopy inner;
  Wrap();
  Wrap(const Wrap&) = default;  // explicitly defaulted AND implicitly deleted (inner is not copyable)
  Wrap& operator=(const Wrap&) = delete;
  ~Wrap() = default;
};
Wrap::Wrap() = default;

struct Tracker {
  Tracker();
  Tracker(const Tracker&);
  Tracker(Tracker&&);
  ~Tracker();
  int v;
};
struct Plain {
  Tracker t;
  int v;
};
int copy_plain(const Plain& a) {
  Plain b = a;  // hidden Plain(const Plain&) and hidden ~Plain become referenced
  return b.v;
}
struct Base {
  explicit Base(int);
  ~Base();
};
struct Unreferenced : Base {  // its implicit destructor is never used: no node
  Tracker m;
  Unreferenced();
};

// --- Family B: initializers, construction, conversion, operators, lifetime ---
struct Part {
  Part();
  Part(Part&&);
  Part(const Part&);
  ~Part();
  explicit operator int() const;
};
Part operator+(const Part&, const Part&);

struct Box : Base {
  Part part;
  Tracker tracker;  // no written initializer: the compiler synthesizes Tracker()
  Box(int n, Part p) : Base(n), part(static_cast<Part&&>(p)) {}
};

int build(bool early, Part src) {
  Box box(1, src);        // Box ctor; src copied into the by-value parameter
  Part sum = src + src;   // user operator+; C++17: the prvalue initializes sum, no temporary
  int i = static_cast<int>(sum);  // explicit conversion function
  int j = 1 + 2;          // built-in: no callable
  if (early) {
    Tracker inner;        // destroyed on the early return only
    return i + j;
  }
  {
    Tracker nested;       // destroyed at the end of the nested block
  }
  Tracker arr[2];         // array object: one destruction per array, element destructor
  return i + arr[0].v;
}

Tracker make();
void elision() {
  Tracker a = make();     // guaranteed elision: no temporary destroyed
  make();                 // discarded: temporary destroyed at the end of the full-expression
}

Tracker pure_nrvo(bool early) {
  Tracker result;         // NRVO candidate: its destruction and the return moves may be elided
  if (early) return result;
  result.v = 1;
  return result;
}
Tracker two_names(bool early) {
  Tracker a;
  Tracker b;
  if (early) return a;    // two candidates: nothing is elided
  return b;
}

struct Owner : Base {
  Tracker a;
  int b;
  Tracker c;
  Owner();
  ~Owner();
};
Owner::~Owner() {}        // owns ~Tracker for c and a, and ~Base; nothing for b

struct Implicit : Base {
  Tracker m;
  Implicit();
};
void use_implicit() {
  Implicit local;         // hidden ~Implicit owns ~Tracker (m) and ~Base
}

void take_def(Tracker by_value) { (void)by_value; }
void take_decl(Tracker by_value);
void call_both(const Tracker& t) {
  take_def(t);            // argument copy in call_both; destruction owned by the ABI-decided side
  take_decl(t);
}

// --- Family C: captures, closure lifetime, provable indirect call -----------
int make_value();
int target(int);
struct Big {
  Big();
  Big(const Big&);
  ~Big();
  int n;
};
struct Worker {
  int m;
  int run(int (*fp)(int)) {
    int x = 1;
    int y = 2;
    Big big;
    auto l1 = [x, &y, this, z = make_value(), big]() { return target(x + y + m + z + big.n); };
    auto l2 = [&]() { return target(x + m); };
    auto l3 = [*this]() {
      auto inner = [this]() { return target(m); };
      return inner();
    };
    int r = (*&target)(3) + fp(4);
    return l1() + l2() + l3() + r;
  }
};

// --- Cross-composition: nontrivial by-value arguments through a closure, member and free
// operators, a template instantiation and an unknown callee ------------------------------------
struct Sink {
  void operator()(Tracker by_value) const;  // member operator(): the object is argument 0, not a parameter
  Sink operator+(Tracker by_value) const;   // member operator+
};
Sink operator-(const Sink& s, Tracker by_value);  // free operator-: both operands are parameters
template <class T>
void generic_take(Tracker by_value, T tag) {
  (void)by_value;
  (void)tag;
}
template <class T>
void dependent_take(T by_value) {  // the destructible parameter type itself is dependent
  (void)by_value;
}
void compose(const Tracker& t, void (*sink)(Tracker), const Sink& s) {
  auto take_lambda = [](Tracker by_value) { return by_value.v; };
  take_lambda(t);      // closure operator()(Tracker): the lambda owns the parameter destruction
  s(t);                // member operator(): callee-owned; the object argument is skipped
  s + t;               // member operator+
  s - t;               // free operator-
  generic_take(t, 1);  // concrete instantiation: ownership decided before folding to the primary
  dependent_take(t);   // T = Tracker: the concrete parameter type decides, the pattern's T never could
  sink(t);             // unknown callee: destruction ownership is unresolved, never the caller's
}
struct Mem {
  void take(Tracker by_value);  // a candidate target the analyzer must never confirm through a member pointer
};
void indirect_member(Mem& object, Mem* pointer, void (Mem::*member)(Tracker), const Tracker& value) {
  (object.*member)(value);    // .*  : callee unknown; the parameter prototype is the member pointer's pointee
  (pointer->*member)(value);  // ->* : same
}
