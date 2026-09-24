// Public synthetic fixture for the Clang LibTooling smoke test.
// No includes on purpose: the smoke test must not depend on the host's
// standard library include configuration.

namespace geo {

struct Shape {
  double area() const;          // declaration only
  double scaled(double k) const;
  static int count;
};

double Shape::area() const { return 1.0; }                       // definition, owner geo::Shape
double Shape::scaled(double k) const { return area() * k; }      // direct member call to area()

int Shape::count = 0;

}  // namespace geo

int print(int value);       // declaration
double print(double value); // declaration, overload by parameter type

int print(int value) { return value; }          // definition of print(int)
double print(double value) { return value; }    // definition of print(double)

struct Other {
  void print(int value);  // same spelling as the free functions; different owner
};

void Other::print(int) {}

// "print(99)" appears here only in a comment and in the string literal below.
int use(const geo::Shape& shape) {
  const char* text = "print(99); shape.area();";  // text, not code
  (void)text;
  print(1);         // resolves to ::print(int)
  print(2.5);       // resolves to ::print(double)
  Other other;
  other.print(3);   // resolves to Other::print(int)
  return static_cast<int>(shape.area());  // resolves to geo::Shape::area() const
}
