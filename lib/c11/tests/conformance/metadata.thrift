// SPDX-License-Identifier: Apache-2.0
/** Accepted grammar metadata; these extensions do not change C11 ownership. */
namespace c11 metadata (example.note = "namespace")
cpp_include "not_a_real_c11_header.h"
/** Numeric identifier. */
typedef i32 (example.note = "base") Number (example.note = "alias")
typedef list cpp_type "std::vector<int>" <Number> Numbers
typedef list<Number> cpp_type "std::vector<int>" LegacyNumbers
/** Documented enumeration. */
enum Annotated {
  /** First enumeration value. */
  VALUE = 1 (example.flag)
} (example.note = "enum")
/** Record documentation with UTF-8: Größe.
 *
 * A second paragraph with **Markdown**.
 */
struct AnnotatedRecord xsd_all {
  /** Optional numeric field. */
  1: optional Number value = 7 xsd_optional xsd_nillable
     xsd_attrs { 1: string attribute } (example.note = "field")
  2: optional AnnotatedRecord & reference
} (example.note = "record")
/** Service documentation. */
service AnnotatedService {
  /** Legacy operation documentation. */
  async void legacy(
    /** Input collection documentation. */
    1: Numbers values) (example.note = "method")
} (example.note = "service")

// Regression: a record descriptor must not shadow the corresponding enum typedef.
enum ExtendedAttributeType { NODES = 0, TABLE = 1, CALCULATED = 2 }
struct ExtendedAttribute {
  1: required ExtendedAttributeType extendedType;
  2: optional list<ExtendedAttribute> children;
}
typedef ExtendedAttribute AttributeAlias

// Preserve aliases in fields, including forward references and recursive records.
typedef LaterAlias EarlierAlias
typedef i32 LaterAlias
typedef AliasNode NodeAlias
struct AliasNode { 1: optional NodeAlias next }
struct AliasFields {
  1: Number number
  2: Numbers numbers
  3: AttributeAlias attribute
  4: EarlierAlias forwardChain
  5: NodeAlias node
}
service AliasService { EarlierAlias value(1: Numbers values) }
/** Default numeric constant. */
const EarlierAlias ALIASED_VALUE = 7

/** Documented exception. */
exception DocumentedError {
  /** Error message documentation. */
  1: string message
}
/** Documented union. */
union DocumentedChoice { 1: i32 number; 2: string text }
/** Derived service documentation. */
service DocumentedService extends AnnotatedService {
  /** Operation with a declared exception. */
  void check() throws (
    /** Declared exception documentation. */
    1: DocumentedError error
  )
}

/** A literal /* marker must not create a nested C comment. */
struct CommentMarker {}
service EmptyService {}
