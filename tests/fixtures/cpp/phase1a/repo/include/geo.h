// Public synthetic fixture: shared header included by two translation units.
#pragma once

namespace geo {

struct Shape {
  double area() const;            // defined in src/shape_area.cpp
  double scaled(double k) const;  // defined in src/shape_scaled.cpp (split implementation)
  virtual const char* name() const;
  static int count;
};

struct Circle : Shape {
  const char* name() const override;
};

int print(int value);
double print(double value);

struct Other {
  void print(int value);  // same spelling as the free functions; different owner
};

#ifdef FEATURE_X
int feature_only();
#endif

}  // namespace geo
