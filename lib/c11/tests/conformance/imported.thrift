// SPDX-License-Identifier: Apache-2.0
namespace * fallback.space
const i32 BASE = 41
enum State { FIRST, SECOND, NEGATIVE = -3, NEXT }
struct Item { 1: required i32 value }
exception Failure { 1: string reason }
service Base { i32 inherited(1: i32 value) }
