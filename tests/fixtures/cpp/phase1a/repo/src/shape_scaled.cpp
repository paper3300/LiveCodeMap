#include "geo.h"

namespace geo {

static int helper() { return 10; }  // same spelling as in shape_area.cpp, different file scope

namespace {
int detail_fn() { return 20; }
}  // namespace

double Shape::scaled(double k) const { return area() * k + helper() + detail_fn(); }

}  // namespace geo
