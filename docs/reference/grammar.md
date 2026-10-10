# dottr Manifest Language Grammar

This document gives the grammar of the dottr manifest language.

The grammar is given in two levels. The lexical grammar (Section 2) defines
how source text is divided into tokens; the syntactic grammar (Section 3)
defines how tokens form a manifest. Constraints that the grammar does not
express are listed in Section 4.

## 1. Notation

The grammar uses Extended Backus-Naur Form with the following conventions.

| Notation | Meaning |
|----------|---------|
| `a = b ;` | `a` is defined as `b` |
| `"x"` | the literal text `x`; in the syntactic grammar, the token with that text |
| `'x'` | the same as `"x"`; used when `x` contains a double quote |
| `a b` | `a` followed by `b` |
| `a \| b` | `a` or `b` |
| `[ a ]` | `a`, optional |
| `{ a }` | `a`, repeated zero or more times |
| `( a )` | grouping |
| `a - b` | any `a` that is not also a `b` |
| `? text ?` | a set of characters described in prose |
| `(* text *)` | a comment |

Names in upper case (`IDENT`, `STRING`, `RAW_BLOCK`, `NEWLINE`, `EOF`) denote
tokens produced by the lexer. Source text is processed as bytes; every
character named below is ASCII.

## 2. Lexical Grammar

```ebnf
(* Characters *)
letter        = ? an ASCII letter, A to Z or a to z ? ;
digit         = ? an ASCII digit, 0 to 9 ? ;
blank         = " " | ? horizontal tab ? ;
line_end      = [ ? carriage return ? ] ? line feed ? ;

(* A manifest is a sequence of lines. A line whose first non-blank
   characters are three backticks opens a raw block. *)
source        = { source_line | raw_block } ;
source_line   = { blank } [ token { { blank } token } { blank } ]
                [ comment ] ( line_end | ? end of input ? ) ;
comment       = "#" { ? any byte other than line feed ? } ;

token         = keyword | IDENT | STRING | "=" | "," ;

(* Words *)
word          = ( letter | "_" ) { letter | digit | "_" } ;
keyword       = "pkg" | "do" | "end" | "local" | "needs" | "check"
              | "create" | "apply" | "apply_if" | "apply_unless" | "else"
              | "shell" | "command" | "file" | "directory" ;
IDENT         = word - keyword ;

(* String literals *)
STRING        = '"' { string_char | escape | INTERP } '"' ;
string_char   = ? any byte other than NUL, line feed, '"', and '\' ? ;
escape        = "\" ( '"' | "\" ) ;
INTERP        = "@{" word { "." word } "}" ;

(* Raw blocks *)
raw_block     = open_fence { raw_line } close_fence ;
open_fence    = { blank } "```" [ info_string ] { blank } line_end ;
info_string   = "sh" | "bash" | "zsh" ;
close_fence   = { blank } "```" { blank } ( line_end | ? end of input ? ) ;
raw_line      = ( { ? any byte other than NUL and line feed ? } line_end )
                - close_fence ;
```

The lexer reads words by longest match, so two adjacent words must be
separated by a blank; other tokens need no separation (`local A="x"` is
valid). A word that matches a keyword is always the keyword. Within a
string literal, `@` followed by `{` always begins an `INTERP`, and any other
`@` is a `string_char`. Raw blocks contain no
tokens, comments, or interpolation references.

The following tokens are derived from the lexical structure rather than
written directly:

* **`NEWLINE`** is produced by the `line_end` of a `source_line` that contains
  at least one token. Line ends of blank and comment-only lines produce no
  token, so `NEWLINE` stands for one or more line terminators. A `NEWLINE` is
  also produced at the end of input if the last line containing a token has no
  line terminator.
* **`RAW_BLOCK`** is produced by a `raw_block`, followed by a `NEWLINE` for
  its closing fence. Its value is the sequence of `raw_line`s after margin
  removal; the `info_string`, if any, selects
  the interpreter.
* **`EOF`** is produced at the end of input.

## 3. Syntactic Grammar

```ebnf
manifest          = { pkg_decl } EOF ;

pkg_decl          = "pkg" unit_name NEWLINE
                  | "pkg" unit_name "do" NEWLINE { unit_stmt } "end" NEWLINE ;

unit_stmt         = local_stmt
                  | needs_stmt
                  | child_pkg
                  | check_stmt
                  | create_stmt
                  | apply_stmt
                  | apply_if_stmt
                  | apply_unless_stmt ;

local_stmt        = "local" IDENT "=" STRING NEWLINE ;
needs_stmt        = "needs" unit_name { "," unit_name } NEWLINE ;
child_pkg         = "pkg" unit_name NEWLINE ;
check_stmt        = "check" kind IDENT NEWLINE ;
create_stmt       = "create" "directory" IDENT NEWLINE ;

apply_stmt        = "apply" "do" NEWLINE { action_stmt } "end" NEWLINE ;
apply_if_stmt     = "apply_if" IDENT "do" NEWLINE { action_stmt }
                    [ "else" NEWLINE { action_stmt } ] "end" NEWLINE ;
apply_unless_stmt = "apply_unless" IDENT "do" NEWLINE { action_stmt }
                    "end" NEWLINE ;

action_stmt       = shell_stmt ;
shell_stmt        = "shell" "do" NEWLINE RAW_BLOCK NEWLINE "end" NEWLINE ;

kind              = "command" | "file" | "directory" ;
unit_name         = IDENT | STRING ;
```

Example:

````
apply_if TPM_DIR do
  shell do
    ```
    git -C "$TPM_DIR" pull --ff-only
    ```
  end
else
  shell do
    ```
    git clone --depth=1 https://github.com/tmux-plugins/tpm "$TPM_DIR"
    ```
  end
end
````

Notes:

* `action_stmt` currently has a single alternative. It is the extension point
  for future dottr-native actions that replace recurring shell patterns, such
  as a git clone construct. Such actions may be mixed
  with `shell` blocks inside the same `apply` body.
* `shell` may appear only inside `apply`, `apply_if`, or `apply_unless`. A
  `shell` block elsewhere is a syntax error.
* Block-form units may not be nested in version 1. Only the bare form `pkg`
  may appear inside a unit body.
* The block form `pkg ... do ... end` is a single statement.
* `create` accepts only the kind `directory`.

## 4. Constraints Not Expressed by the Grammar

Lexical constraints, reported by the lexer:

* Every captured line of a raw block that is not empty or blank must begin
  with the margin, the leading blanks of the opening fence.
* Within a string literal, `@{` must begin a well-formed `INTERP`.

Syntactic constraints, reported by the parser:

* A `unit_name` written as a `STRING` must be non-empty and must not contain
  an `INTERP` (*pending confirmation*). Unit names are resolved before
  evaluation, when no variable has a value.

Semantic constraints, reported before evaluation: unique unit names,
references to bound variables and defined built-in values, protected variable
names, a `check` of the variable before `apply_if` or `apply_unless`, and
declared, acyclic dependencies.
