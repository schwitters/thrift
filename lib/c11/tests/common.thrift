namespace c11 test
struct External {
  1: required i32 value
}
service Base {
  i32 inherited(1: i32 value)
}
