#include "shared.h"
#include "excluded.h"

#ifdef VARIANT
int run_b_variant(const geo::Shape& shape) { return geo::twice(shape.area()) - excluded_value(); }
#else
int run_b(const geo::Shape& shape, int (*callback)(int)) {
  return geo::twice(shape.area()) + excluded_value() + geo::apply(callback, 1);
}
#endif
