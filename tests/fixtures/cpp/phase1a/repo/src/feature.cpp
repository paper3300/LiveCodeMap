#include "geo.h"

namespace geo {

#ifdef FEATURE_X
int feature_only() { return 42; }
#endif

int always() { return 0; }

}  // namespace geo
