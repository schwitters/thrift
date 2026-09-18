include "common.thrift"
namespace c11 test

enum Color { RED = 1, BLUE = 7 }
typedef i64 Counter
typedef list<string> Names
struct Node {
  1: required i32 value
  2: optional Node next
}
union Choice {
  1: i32 number
  2: string text
}
union DefaultChoice {
  1: i32 number = 9
  2: string text
}
struct Packet {
  1: required bool enabled
  2: required i8 small
  3: required i16 shortValue
  4: required i32 number
  5: required Counter large
  6: required double ratio
  7: required string text
  8: required binary payload
  9: required list<i32> values
  10: required set<string> names
  11: required map<string, list<i64>> index
  12: required Node node
  13: optional Color color = Color.BLUE
  14: optional Choice choice
  15: optional common.External external
  16: optional uuid identifier
  17: optional Names defaults = ["first", "second"]
}
struct Empty {}
exception Problem {
  1: required string reason
}
const Packet SAMPLE = {
  "enabled": true, "small": -128, "shortValue": -32768,
  "number": -2147483648, "large": -9223372036854775808,
  "ratio": -1.5, "text": "hello", "payload": "binary",
  "values": [1, -2, 3], "names": ["alpha", "beta"],
  "index": {"entry": [10, 20]}, "node": {"value": 42},
  "color": Color.RED, "choice": {"text": "selected"},
  "external": {"value": 99}, "identifier": "00112233-4455-6677-8899-aabbccddeeff"
}
const map<string, list<Node>> NESTED = {"nodes": [{"value": 1}, {"value": 2}]}
const i32 ANSWER = 42
service Echo extends common.Base {
  Packet echo(1: required Packet packet) throws (1: Problem problem)
  void ping()
  oneway void notify(1: i32 value)
}

struct EncodingEdges {
  1: required bool yes
  2: required bool no
  20: required list<bool> flags
  3: required map<i16, bool> lookup
  4: required list<list<i32>> nested
  5: required map<binary, list<bool>> emptyMap
  32767: optional i16 maximumId
}

union OwnedChoice {
  1: list<string> names
  2: Node node
  3: map<string, i32> index
}

enum EnumBoundary {
  MINIMUM = -2147483648,
  MAXIMUM = 2147483647,
  NEGATIVE = -1
}
struct EnumValues {
  1: Color color
  2: EnumBoundary boundary
  3: list<Color> colors
}
