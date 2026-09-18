// SPDX-License-Identifier: Apache-2.0
include "imported.thrift"
namespace * unused.fallback
namespace c11 conform

typedef byte ByteAlias
typedef ByteAlias ByteChain
typedef imported.Item ItemAlias
typedef map<imported.State, list<ItemAlias>> ItemIndex

const bool YES = true
const bool NO = false
const byte BYTE_MIN = -128
const i8 BYTE_MAX = 127
const i16 SHORT_MIN = -32768
const i16 SHORT_MAX = 32767
const i32 INT_MINIMUM = -2147483648
const i32 INT_MAXIMUM = 2147483647
const i64 LONG_MIN = -9223372036854775808
const i64 LONG_MAX = 9223372036854775807
const i32 HEX = 0x2a
const i32 NEGATIVE_HEX = -0x2a
const double FRACTION = -1.25e-3
const double INTEGER_DOUBLE = 42
const string ESCAPED = 'line\n"quote"\\tail'
const binary OCTETS = "abc"
const uuid ID = '{00112233-4455-6677-8899-aAbBcCdDeEfF}'
const i32 IMPORTED = imported.BASE
const i32 REFERENCED = IMPORTED
const imported.State ENUM_VALUE = imported.State.NEXT
const ItemAlias ITEM = {"value": REFERENCED}
const ItemIndex INDEX = {imported.State.SECOND: [{"value": 7}, {"value": 9}]}
const set<i16> MEMBERS = [1, -2, 3]
const list<string> EMPTY_LIST = []
const map<string, i32> EMPTY_MAP = {}
const set<i32> EMPTY_SET = []

struct Defaults {
  1: required i32 mandatory = REFERENCED
  2: optional string optionalText = "default"
  3: i32 ordinary = 17
  4: optional ItemAlias item = {"value": 41}
  5: optional ItemIndex index = {imported.State.SECOND: [{"value": 7}, {"value": 9}]}
  6: optional ByteChain small = BYTE_MIN
  7: optional uuid identifier = ID
}
// A peer with different defaults must receive the sender's initialized values.
struct DefaultsPeer {
  2: optional string optionalText = "peer default"
  4: optional ItemAlias item
  5: optional ItemIndex index
  6: optional ByteChain small = 12
}
struct Ids { i32 implicitFirst; i16 implicitSecond; 32767: optional i64 highest }
struct Keywords { 1: i32 int; 2: string switch; 3: bool bool; 4: binary union }
struct Left { 1: optional Right right }
struct Right { 1: optional Left left }
struct Tree { 1: list<Tree> children; 2: i32 value }
struct Empty {}
union Selection { 1: i32 number = 23; 2: string text }
exception OtherFailure { 1: i32 code }

service Middle extends imported.Base { void ping() }
service Leaf extends Middle {
  ItemAlias fetch(1: ItemIndex index) throws (1: imported.Failure failure, 2: OtherFailure other)
  void mayFail() throws (1: OtherFailure other)
  oneway void notify(1: ByteChain value)
}
service EmptyService {}
