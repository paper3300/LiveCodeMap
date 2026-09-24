// Independent four-case semantic matrix (execution plan) plus audit boundaries.

// Case 1: default argument evaluated at the use site, not by the declaring function.
int seed();
int calculate(int value = seed());
int calculate(int value) { return value; }
int seed() { return 7; }
int default_caller() { return calculate(); }
int explicit_caller() { return calculate(3); }

// Case 2: init-capture lambda invoked immediately.
int make_value();
int consume(int v);
int make_value() { return 1; }
int consume(int v) { return v; }
int lambda_caller() {
  return [x = make_value()] { return consume(x); }();
}

// Case 3: virtual dispatch, final override, qualified base call, same-class qualification.
struct Base {
  virtual int run() const;
  virtual ~Base() = default;
};
int Base::run() const { return 1; }
struct Derived final : Base {
  int run() const override;
};
int Derived::run() const { return 2; }
int base_dispatch(const Base& b) { return b.run(); }             // virtual slot: Base::run
int final_target(const Derived& d) { return d.run(); }           // final class: devirtualized Derived::run
int qualified_base(const Derived& d) { return d.Base::run(); }   // explicit qualification: Base::run
int same_class_qualified(const Derived& d) { return d.Derived::run(); }  // qualified with its own class
struct Plain {
  int run() const;  // not virtual
};
int Plain::run() const { return 3; }
int plain_qualified(const Plain& p) { return p.Plain::run(); }   // explicit qualifier on a non-virtual method: qualified
int plain_unqualified(const Plain& p) { return p.run(); }        // ordinary member call: static_target

// Case 4: function template primary, explicit specialization, implicit use.
template <class T>
int token() { return 0; }
template <>
int token<int>() { return 1; }
int uses_int() { return token<int>(); }        // calls the explicit specialization
int uses_double() { return token<double>(); }  // implicit instantiation folds to the primary with <double>

// Audit: primaries with equal parameter lists but different template-parameter kinds.
template <class T>
int collision() { return 2; }
template <int N>
int collision() { return 3; }
int type_use() { return collision<int>(); }
int value_use() { return collision<1>(); }
// Redeclaration with renamed template parameter: same primary.
template <class U>
int collision();

// Audit: combined aspects. A default argument containing a virtual call, and an
// init-capture containing an implicit template use.
Base& global_base();
int with_default(int value = global_base().run());
int with_default(int value) { return value; }
int default_user() { return with_default(); }
template <class T>
int folded() { return 1; }
int lambda_cross() { return [x = folded<double>()] { return x; }(); }

// Audit: anonymous namespace-scope types anchored to their declarators.
struct { int first; } anonymous_one;
struct { double second; } anonymous_two;
typedef struct { int tagged; } TaggedAnon;
struct Holder {
  union { int a; float b; };   // unnamed member records: anchored to Holder with ordinals
  union { char c; };
  struct { int x; } named_member;  // declarator-anchored
};

// Boundary: same-named local classes in different blocks stay distinct.
int locals() {
  {
    struct Local { int v() const { return 1; } };
    Local a;
    (void)a.v();
  }
  {
    struct Local { int v() const { return 2; } };
    Local b;
    (void)b.v();
  }
  return 0;
}
