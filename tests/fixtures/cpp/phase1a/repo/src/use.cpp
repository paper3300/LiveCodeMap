#include <cstdint>
#include "geo.h"
#include "ext.h"  // outside the repository root, reached via -I ../external_sdk

namespace geo {
int print(int value) { return value; }
double print(double value) { return value; }
void Other::print(int) {}
}  // namespace geo

typedef int (*Fn)(int);

// "print(99)" appears here only in a comment and in the string literal below.
int use(const geo::Shape& shape, Fn fp) {
  const char* text = "print(99); shape.area();";
  (void)text;
  geo::print(1);      // resolves to geo::print(int)
  geo::print(2.5);    // resolves to geo::print(double)
  geo::Other other;
  other.print(3);     // resolves to geo::Other::print(int)
  std::uint32_t small = 7;
  (void)small;
  auto twice = [](int v) { return geo::print(v) * 2; };  // lambda: its body call belongs to the lambda
  int r = twice(4);                                     // call to the lambda
  r += fp(1);                                           // indirect: unresolved, no invented target
  r += ext_api(5);                                      // declared outside the root: placeholder target
  Fn taken = &geo::print;                               // function address: references, not calls
  (void)taken;
  return r + static_cast<int>(shape.area()) + (shape.name() != nullptr);  // virtual: confirmed slot Shape::name
}
