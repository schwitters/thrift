namespace c11 example
exception CalculationError {
  1: required string message
}
service Calculator {
  i64 add(1: required i64 left, 2: required i64 right) throws (1: CalculationError error)
}
