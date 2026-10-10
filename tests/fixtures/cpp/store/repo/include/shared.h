#pragma once

namespace geo {

struct Shape {
  virtual ~Shape();
  virtual int area() const;
};

inline int twice(int value) { return value * 2; }

int apply(int (*callback)(int), int value);

}  // namespace geo

#include "detail.inl"
