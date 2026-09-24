#include "geo.h"

namespace geo {

static int helper() { return 1; }  // internal linkage: file-scoped identity

namespace {
int detail_fn() { return 2; }  // anonymous namespace: file-scoped identity
}  // namespace

double Shape::area() const { return 1.0 * helper() * detail_fn(); }

int Shape::count = 0;

const char* Shape::name() const { return "shape"; }
const char* Circle::name() const { return "circle"; }

}  // namespace geo
