// TreeSitterQueries.hpp
#pragma once

namespace editor {

//! Query de simbolos C++ (gramatica vendorizada em `vendor/tree-sitter-cpp`).
//!
//! Duas regras que a gramatica impoe e que custam horas se ignoradas:
//!
//!  1. O `declarator` e tipado por no. `typedef long unsigned int size_t;`
//!     produz `declarator: (primitive_type)` -- `size_t` e palavra-chave
//!     builtin da gramatica, nao `type_identifier`. E `int x = 0;` tem o
//!     declarator embrulhado em `init_declarator`, `int* p` em
//!     `pointer_declarator`, `int& r` em `reference_declarator`, `int a[3]` em
//!     `array_declarator`. Por isso o mesmo nome aparece em varias formas.
//!     `ts_query_new` **rejeita** um no que o campo nao pode ter (erro 5), o
//!     que serve de rede de seguranca para pegar forma errada.
//!  2. Capturar o no inteiro de um `qualified_identifier` traz o texto
//!     completo, que em C++ pode comecar linhas antes (no tipo de retorno).
//!     Por isso os padroes usam o campo `name:` nos qualified-ids.
//!
//! Capturas: scope.name | func.name | field.name | typedef.name | var.name |
//!           enum_item.name | macro.name
inline const char* K_CPP_SYMBOL_QUERY = R"(
  ;; ---- Escopos ----
  (namespace_definition name: (namespace_identifier) @scope.name)
  (class_specifier name: (type_identifier) @scope.name)
  (struct_specifier name: (type_identifier) @scope.name)
  (union_specifier name: (type_identifier) @scope.name)
  (enum_specifier name: (type_identifier) @scope.name)

  ;; ---- Funcoes: definicoes, metodos e prototipos ----
  ;; dentro do corpo da classe um metodo e `declaration`, mas com `static`
  ;; vira `field_declaration` -- por isso os dois caminhos, com e sem ponteiro.
  (function_definition declarator: (function_declarator declarator: [
     (identifier) @func.name
     (field_identifier) @func.name
     (operator_name) @func.name
     (destructor_name) @func.name
     (qualified_identifier name: (identifier) @func.name)
     (qualified_identifier name: (field_identifier) @func.name)
  ]))
  (function_definition declarator: (pointer_declarator declarator: (function_declarator declarator: [
     (identifier) @func.name
     (field_identifier) @func.name
     (operator_name) @func.name
     (destructor_name) @func.name
     (qualified_identifier name: (identifier) @func.name)
  ])))
  (declaration declarator: (function_declarator declarator: [
     (identifier) @func.name
     (field_identifier) @func.name
     (operator_name) @func.name
     (destructor_name) @func.name
     (qualified_identifier name: (identifier) @func.name)
  ]))
  (declaration declarator: (pointer_declarator declarator: (function_declarator declarator: [
     (identifier) @func.name
     (field_identifier) @func.name
     (operator_name) @func.name
     (destructor_name) @func.name
     (qualified_identifier name: (identifier) @func.name)
  ])))
  (field_declaration declarator: (function_declarator declarator: [
     (identifier) @func.name
     (field_identifier) @func.name
     (operator_name) @func.name
     (destructor_name) @func.name
     (qualified_identifier name: (identifier) @func.name)
  ]))
  (field_declaration declarator: (pointer_declarator declarator: (function_declarator declarator: [
     (identifier) @func.name
     (field_identifier) @func.name
     (operator_name) @func.name
     (destructor_name) @func.name
     (qualified_identifier name: (identifier) @func.name)
  ])))

  ;; ---- Campos de classe/struct ----
  (field_declaration declarator: [
     (field_identifier) @field.name
     (identifier) @field.name
  ])
  (field_declaration declarator: (pointer_declarator declarator: [
     (field_identifier) @field.name
     (identifier) @field.name
  ]))
  (field_declaration declarator: (array_declarator declarator: [
     (field_identifier) @field.name
     (identifier) @field.name
  ]))

  ;; ---- typedef / using X = ... ----
  ;; `primitive_type` cobre size_t, ptrdiff_t, wchar_t... (keywords do C)
  (type_definition declarator: [
     (type_identifier) @typedef.name
     (primitive_type) @typedef.name
  ])

  ;; ---- Variaveis (declaracao cujo declarator NAO e funcao) ----
  (declaration declarator: [
     (identifier) @var.name
     (field_identifier) @var.name
     (qualified_identifier name: (identifier) @var.name)
  ])
  (declaration declarator: (init_declarator declarator: [
     (identifier) @var.name
     (qualified_identifier name: (identifier) @var.name)
  ]))
  (declaration declarator: (pointer_declarator declarator: [
     (identifier) @var.name
     (qualified_identifier name: (identifier) @var.name)
  ]))
  (declaration declarator: (init_declarator declarator: (pointer_declarator declarator: [
     (identifier) @var.name
     (qualified_identifier name: (identifier) @var.name)
  ])))
  (declaration declarator: (array_declarator declarator: [
     (identifier) @var.name
     (qualified_identifier name: (identifier) @var.name)
  ]))

  ;; ---- Itens de enum ----
  (enumerator name: (identifier) @enum_item.name)

  ;; ---- Macros ----
  (preproc_def name: (identifier) @macro.name)
  (preproc_function_def name: (identifier) @macro.name)
)";

}  // namespace editor
