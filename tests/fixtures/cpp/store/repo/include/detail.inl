namespace geo {

inline int detail_value(int (*callback)(int)) { return callback(twice(21)); }

}  // namespace geo
