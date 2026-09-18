#include "core/sds.h"
#include "syntax/syntax.h"
#include "syntax/syntax_internal.h"

const Lang langs[] = {
  { "c", " c h ",
    " if else for while do switch case default return goto break continue"
    " sizeof typedef struct union enum const static extern inline volatile"
    " register restrict auto alignas alignof static_assert thread_local"
    " typeof typeof_unqual _Alignas _Alignof _Atomic _Generic _Noreturn"
    " _Static_assert _Thread_local defer ",
    " void char short int long float double signed unsigned bool _Bool"
    " _BitInt _Complex _Decimal32 _Decimal64 _Decimal128 _Imaginary"
    " size_t ssize_t ptrdiff_t intptr_t uintptr_t intmax_t uintmax_t"
    " wchar_t char8_t char16_t char32_t va_list"
    " int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t"
    " int_least8_t int_least16_t int_least32_t int_least64_t"
    " int_fast8_t int_fast16_t int_fast32_t int_fast64_t"
    " FILE NULL true false nullptr errno ",
    "//", "", "/*", "*/", "", "", 0, 1, 0, 1, 0, 0 },
  { "c++", " cpp cc cxx c++ hpp hh hxx h++ ipp tpp cu cuh ",
    " if else for while do switch case default return goto break continue"
    " sizeof typedef struct union enum const static extern inline volatile"
    " class namespace template typename public private protected virtual"
    " override final new delete this try catch throw using constexpr"
    " operator friend explicit mutable noexcept static_cast dynamic_cast"
    " reinterpret_cast const_cast decltype register restrict volatile"
    /* C++20/23: concepts, coroutines, modules — the gap that prompted this */
    " concept requires co_await co_yield co_return consteval constinit"
    " module import export "
    " alignas alignof static_assert thread_local typeid asm goto"
    " and and_eq bitand bitor compl not not_eq or or_eq xor xor_eq"
    " if_constexpr inline extern explicit ",
    " void char short int long float double signed unsigned bool auto"
    " char8_t char16_t char32_t wchar_t size_t ssize_t ptrdiff_t nullptr_t"
    " int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t"
    " std string string_view vector map unordered_map set unordered_set"
    " pair tuple array deque list optional variant any span"
    " unique_ptr shared_ptr weak_ptr function initializer_list"
    " nullptr true false NULL ",
    "//", "", "/*", "*/", "", "", 0, 1, 0, 1, 0, 0 },
  { "python", " py pyw ",
    " False None True and as assert async await break class continue def"
    " del elif else except finally for from global if import in is lambda"
    " nonlocal not or pass raise return try while with yield match case ",
    " print len range open str int float list dict set tuple bool bytes"
    " self super isinstance type Exception ValueError TypeError enumerate"
    " zip map filter sorted sum min max abs any all ",
    "#", "", "", "", "'''", "\"\"\"", 1, 0, 0, 2, 0, 0 },
  { "bash", " sh bash zsh ",
    " if then else elif fi for while until do done case esac function in"
    " select time return exit break continue local export readonly"
    " declare set unset shift source alias trap ",
    " echo printf read cd pwd test true false eval exec kill wait sleep"
    " grep sed awk cat ls rm mv cp mkdir ",
    "#", "", "", "", "", "", 0, 0, 0, 2, 1, 0 },
  { "rust", " rs ",
    " as break const continue crate dyn else enum extern fn for if impl in"
    " let loop match mod move mut pub ref return static struct super trait"
    " type unsafe use where while async await ",
    " i8 i16 i32 i64 i128 u8 u16 u32 u64 u128 f32 f64 usize isize bool"
    " char str String Vec Option Some None Result Ok Err Box self Self"
    " true false println print format vec ",
    "//", "", "/*", "*/", "", "", 1, 0, 0, 1, 0, 0 },
  { "sql", " sql ",
    " select from where insert into values update set delete create table"
    " drop alter index view as join left right inner outer full cross on"
    " group by order having limit offset union all distinct and or not"
    " null is in exists between like case when then else end primary key"
    " foreign references default unique check constraint begin commit"
    " rollback transaction if replace with ",
    " int integer bigint smallint varchar char text date time timestamp"
    " datetime boolean decimal numeric float real double blob serial"
    " count sum avg min max coalesce ifnull now ",
    "--", "", "/*", "*/", "", "", 1, 0, 1, 2, 0, 0 },
  { "javascript", " js jsx mjs cjs ",
    " break case catch class const continue debugger default delete do"
    " else export extends finally for function if import in instanceof"
    " let new of return static super switch this throw try typeof var"
    " void while with yield async await get set ",
    " true false null undefined console Number String Boolean Object"
    " Array Promise Map Set Symbol JSON Math document window require"
    " module NaN Infinity ",
    "//", "", "/*", "*/", "", "", 1, 0, 0, 2, 1, 0 },
  { "typescript", " ts tsx ",
    " break case catch class const continue debugger default delete do"
    " else export extends finally for function if import in instanceof"
    " let new of return static super switch this throw try typeof var"
    " void while with yield async await get set interface type enum"
    " implements declare readonly namespace abstract public private"
    " protected keyof infer is asserts satisfies ",
    " true false null undefined any string number boolean object unknown"
    " never void console Promise Array Map Set Record Partial JSON Math ",
    "//", "", "/*", "*/", "", "", 1, 0, 0, 2, 1, 0 },
  { "go", " go ",
    " break case chan const continue default defer else fallthrough for"
    " func go goto if import interface map package range return select"
    " struct switch type var ",
    " bool byte complex64 complex128 error float32 float64 int int8 int16"
    " int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr"
    " true false nil iota append cap close copy delete len make new panic"
    " print println recover any ",
    "//", "", "/*", "*/", "", "", 0, 0, 0, 1, 1, 0 },
  { "java", " java ",
    " abstract assert break case catch class const continue default do"
    " else enum extends final finally for goto if implements import"
    " instanceof interface native new package private protected public"
    " return static strictfp super switch synchronized this throw throws"
    " transient try volatile while var record sealed permits yield ",
    " boolean byte char double float int long short void true false null"
    " String Object Integer Long Double Boolean List Map Set ArrayList"
    " HashMap System ",
    "//", "", "/*", "*/", "", "", 1, 0, 0, 1, 0, 0 },
  { "lua", " lua ",
    " and break do else elseif end false for function goto if in local"
    " nil not or repeat return then true until while ",
    " print pairs ipairs table string math io os type tostring tonumber"
    " require self error pcall assert ",
    "--", "", "--[[", "]]", "", "", 1, 0, 0, 2, 0, 0 },
  { "ruby", " rb ",
    " alias and begin break case class def do else elsif end ensure false"
    " for if in module next nil not or redo rescue retry return self"
    " super then true undef unless until when while yield ",
    " puts print require require_relative attr_accessor attr_reader"
    " attr_writer new raise lambda proc each map select inject ",
    "#", "", "", "", "", "", 1, 0, 0, 2, 0, 0 },
  { "php", " php ",
    " echo print if else elseif while for foreach as function return"
    " class public private protected static new try catch finally throw"
    " namespace use require require_once include isset unset switch case"
    " default break continue do const abstract final interface implements"
    " extends instanceof match fn ",
    " true false null array string int float bool void this self parent ",
    "//", "#", "/*", "*/", "", "", 1, 0, 0, 2, 0, 0 },
  { "json", " json ",
    " ", " true false null ",
    "", "", "", "", "", "", 1, 0, 0, 0, 0, 0 },
  { "toml", " toml ini cfg conf ",
    " ", " true false ",
    "#", ";", "", "", "", "", 1, 0, 0, 2, 0, 0 },
  { "yaml", " yml yaml ",
    " ", " true false null yes no ",
    "#", "", "", "", "", "", 1, 0, 0, 2, 0, 0 },
  { "make", " mk makefile ",
    " ifeq ifneq ifdef ifndef else endif include define endef export ",
    " ", "#", "", "", "", "", "", 0, 0, 0, 0, 0, 0 },
  { "markdown", " md markdown mkd mdown mdwn ",
    " ", " ", "", "", "", "", "", "", 1, 0, 0, 0, 0, 1 },
  { "text", "", " ", " ", "", "", "", "", "", "", 0, 0, 0, 0, 0, 0 },
};
_Static_assert(sizeof langs / sizeof *langs == NLANGS,
               "NLANGS in syntax.h must match the langs[] table");

static const Lang *lang_by_name(const char *name) {
    for (int i = 0; i < NLANGS; i++)
        if (!strcmp(langs[i].name, name)) return &langs[i];
    return LANG_TEXT;
}
const Lang *lang_for(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!strcasecmp(base, "Makefile") || !strcasecmp(base, "GNUmakefile"))
        return lang_by_name("make");
    if (!strcasecmp(base, "CMakeLists.txt")) return lang_by_name("make");
    const char *dot = strrchr(base, '.');
    if (!dot || !dot[1]) return LANG_TEXT;
    char pat[32];
    snprintf(pat, sizeof pat, " %s ", dot + 1);
    for (char *p = pat; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (int i = 0; i + 1 < NLANGS; i++)
        if (strstr(langs[i].exts, pat)) return &langs[i];
    return LANG_TEXT;
}
