#include "shared.h"
#include "solo.h"
#include "generated.h"
#include <ext.h>

namespace {
struct Square : geo::Shape {
  int area() const override { return 4; }
};
}  // namespace

int run_a(const geo::Shape& shape) {
  int total = geo::twice(shape.area()) + solo_value() + generated_value() + ext::api();
  auto add = [&total](int v) { return total + v; };
  return add(geo::detail_value(geo::twice)) + Square{}.area();
}
