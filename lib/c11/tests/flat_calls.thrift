// SPDX-License-Identifier: Apache-2.0
namespace c11 flat
exception Problem { 1: string reason }
struct Item { 1: i32 number }
service Base {
  void done() throws (1: Problem problem)
}
service Api extends Base {
  i32 number() throws (1: Problem problem)
  Item item() throws (1: Problem problem, 2: Problem other)
  oneway void notify(1: i32 status)
  i32 names(1:i32 client, 2:i32 args, 3:i32 result, 4:i32 status,
            5:i32 out_success, 6:i32 switch, 7:string text, 8:i32 text_data)
  string text()
  list<i32> values()
}
