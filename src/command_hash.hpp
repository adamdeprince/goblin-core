/* C++ code produced by gperf version 3.3 */
/* Command-line: gperf --output-file=/Users/adam/dev/packrat/src/command_hash.hpp /Users/adam/dev/packrat/src/command_hash.gperf  */
/* Computed positions: -k'1-2,8,12-13,18-19,$' */

#if !((' ' == 32) && ('!' == 33) && ('"' == 34) && ('#' == 35) \
      && ('%' == 37) && ('&' == 38) && ('\'' == 39) && ('(' == 40) \
      && (')' == 41) && ('*' == 42) && ('+' == 43) && (',' == 44) \
      && ('-' == 45) && ('.' == 46) && ('/' == 47) && ('0' == 48) \
      && ('1' == 49) && ('2' == 50) && ('3' == 51) && ('4' == 52) \
      && ('5' == 53) && ('6' == 54) && ('7' == 55) && ('8' == 56) \
      && ('9' == 57) && (':' == 58) && (';' == 59) && ('<' == 60) \
      && ('=' == 61) && ('>' == 62) && ('?' == 63) && ('A' == 65) \
      && ('B' == 66) && ('C' == 67) && ('D' == 68) && ('E' == 69) \
      && ('F' == 70) && ('G' == 71) && ('H' == 72) && ('I' == 73) \
      && ('J' == 74) && ('K' == 75) && ('L' == 76) && ('M' == 77) \
      && ('N' == 78) && ('O' == 79) && ('P' == 80) && ('Q' == 81) \
      && ('R' == 82) && ('S' == 83) && ('T' == 84) && ('U' == 85) \
      && ('V' == 86) && ('W' == 87) && ('X' == 88) && ('Y' == 89) \
      && ('Z' == 90) && ('[' == 91) && ('\\' == 92) && (']' == 93) \
      && ('^' == 94) && ('_' == 95) && ('a' == 97) && ('b' == 98) \
      && ('c' == 99) && ('d' == 100) && ('e' == 101) && ('f' == 102) \
      && ('g' == 103) && ('h' == 104) && ('i' == 105) && ('j' == 106) \
      && ('k' == 107) && ('l' == 108) && ('m' == 109) && ('n' == 110) \
      && ('o' == 111) && ('p' == 112) && ('q' == 113) && ('r' == 114) \
      && ('s' == 115) && ('t' == 116) && ('u' == 117) && ('v' == 118) \
      && ('w' == 119) && ('x' == 120) && ('y' == 121) && ('z' == 122) \
      && ('{' == 123) && ('|' == 124) && ('}' == 125) && ('~' == 126))
/* The character set is not based on ISO-646.  */
#error "gperf generated tables don't work with this execution character set. Please report a bug to <bug-gperf@gnu.org>."
#endif

#line 1 "/Users/adam/dev/packrat/src/command_hash.gperf"

// Perfect-hash command dispatch. Regenerate the checked-in header with:
//   gperf src/command_hash.gperf --output-file=src/command_hash.hpp
// (or: cmake --build <dir> --target goblin_core_regen_command_hash)
// Keywords are UPPER-CASE; the caller upper-cases the command name first (see
// parse_command in command.cpp), so this table is matched case-sensitively.
#include "goblin/core/command.hpp"
#line 17 "/Users/adam/dev/packrat/src/command_hash.gperf"
struct CommandEntry { const char* name; goblin::core::CommandType type; };

#define TOTAL_KEYWORDS 124
#define MIN_WORD_LENGTH 3
#define MAX_WORD_LENGTH 29
#define MIN_HASH_VALUE 8
#define MAX_HASH_VALUE 388
/* maximum key range = 381, duplicates = 0 */

class CommandDispatch
{
private:
  static inline unsigned int hash (const char *str, size_t len);
public:
  static const struct CommandEntry *lookup (const char *str, size_t len);
};

inline unsigned int
CommandDispatch::hash (const char *str, size_t len)
{
  static const unsigned short asso_values[] =
    {
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389,   0, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389,  70,   0,  62, 122,   5,
      389,   5,  55,  30, 389,  10,   5, 115,  20,   0,
       10,  25,  45,  15,   0,  85,  25,  25,  95, 105,
       17, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389, 389, 389, 389, 389,
      389, 389, 389, 389, 389, 389
    };
  unsigned int hval = len;

  switch (hval)
    {
      default:
        hval += asso_values[static_cast<unsigned char>(str[18])];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 18:
        hval += asso_values[static_cast<unsigned char>(str[17])];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 17:
      case 16:
      case 15:
      case 14:
      case 13:
        hval += asso_values[static_cast<unsigned char>(str[12])];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 12:
        hval += asso_values[static_cast<unsigned char>(str[11])];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 11:
      case 10:
      case 9:
      case 8:
        hval += asso_values[static_cast<unsigned char>(str[7])];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 7:
      case 6:
      case 5:
      case 4:
      case 3:
      case 2:
        hval += asso_values[static_cast<unsigned char>(str[1])];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 1:
        hval += asso_values[static_cast<unsigned char>(str[0])];
        break;
    }
  return hval + asso_values[static_cast<unsigned char>(str[len - 1])];
}

const struct CommandEntry *
CommandDispatch::lookup (const char *str, size_t len)
{
  static const unsigned char lengthtable[] =
    {
       0,  0,  0,  0,  0,  0,  0,  0,  3,  0,  0,  0,  0,  3,
       0,  0,  6,  0,  0,  4,  0,  6,  7,  3,  4,  0,  0,  7,
       8,  4,  0, 11,  0,  0,  4,  0, 11,  0,  8,  4,  0,  6,
       7,  6,  0,  0,  0,  0,  0,  4, 15,  0,  0,  0,  4, 15,
       0,  0,  0,  0, 15,  6,  0,  0,  4,  0, 11,  0, 18,  4,
       0,  4,  7,  6,  4,  5,  0,  5, 13,  0,  8,  9,  7,  6,
       4,  5, 21, 17,  0,  0,  8, 21, 10,  0,  9, 15, 21,  7,
       0,  4,  5, 16, 10,  0, 24,  0,  0,  7,  8,  0,  0,  6,
       0,  0,  4,  5,  6, 12,  0,  0,  5,  6,  0, 23,  4,  5,
      11, 15,  0,  0,  0, 21, 12,  0,  4,  3,  6, 22,  0,  0,
       0, 16, 17, 11,  0, 10, 11,  0, 11, 14,  0, 16,  0,  0,
       9,  0,  6,  0,  0,  0,  0,  6, 17,  0, 14, 16, 14,  0,
       0,  4, 15,  6,  0, 16,  9,  5,  4, 22, 23,  0,  0,  4,
      17, 14, 29,  0,  4, 22,  0,  0,  0,  0,  0, 11,  0, 15,
       0,  7, 23, 10, 15,  0,  0,  0,  0,  0,  5,  0,  6,  0,
       0,  0,  0,  4,  0, 15,  0,  0, 23,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0, 21,  0,  0,  0,  0,  0,  0,
       6,  0,  0,  0,  0, 13,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 12,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0, 12,  0,  0, 12,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 13
    };
  static const struct CommandEntry wordlist[] =
    {
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 123 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"TTL", goblin::core::CommandType::ttl},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 101 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GET", goblin::core::CommandType::get},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 102 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GETSET", goblin::core::CommandType::getset},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 124 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"PTTL", goblin::core::CommandType::pttl},
      {"",goblin::core::CommandType::unknown},
#line 104 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GETDEL", goblin::core::CommandType::getdel},
#line 125 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"PERSIST", goblin::core::CommandType::persist},
#line 100 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"SET", goblin::core::CommandType::set},
#line 70 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LSET", goblin::core::CommandType::lset},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 120 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"PEXPIRE", goblin::core::CommandType::pexpire},
#line 112 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GETRANGE", goblin::core::CommandType::getrange},
#line 65 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LPOP", goblin::core::CommandType::lpop},
      {"",goblin::core::CommandType::unknown},
#line 127 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"PEXPIRETIME", goblin::core::CommandType::pexpiretime},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 67 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LLEN", goblin::core::CommandType::llen},
      {"",goblin::core::CommandType::unknown},
#line 130 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SAVE", goblin::core::CommandType::goblin_save},
      {"",goblin::core::CommandType::unknown},
#line 113 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"SETRANGE", goblin::core::CommandType::setrange},
#line 22 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"EVAL", goblin::core::CommandType::eval},
      {"",goblin::core::CommandType::unknown},
#line 105 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"STRLEN", goblin::core::CommandType::strlen},
#line 73 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LINSERT", goblin::core::CommandType::linsert},
#line 48 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"ZSCORE", goblin::core::CommandType::zscore},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 19 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"PING", goblin::core::CommandType::ping},
#line 83 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.LSET", goblin::core::CommandType::pma_lset},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 21 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"INFO", goblin::core::CommandType::info},
#line 78 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.LPOP", goblin::core::CommandType::pma_lpop},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 80 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.LLEN", goblin::core::CommandType::pma_llen},
#line 69 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LRANGE", goblin::core::CommandType::lrange},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 51 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HGET", goblin::core::CommandType::hget},
      {"",goblin::core::CommandType::unknown},
#line 111 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"INCRBYFLOAT", goblin::core::CommandType::incrbyfloat},
      {"",goblin::core::CommandType::unknown},
#line 86 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.LINSERT", goblin::core::CommandType::pma_linsert},
#line 66 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"RPOP", goblin::core::CommandType::rpop},
      {"",goblin::core::CommandType::unknown},
#line 20 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"ECHO", goblin::core::CommandType::echo},
#line 54 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HGETALL", goblin::core::CommandType::hgetall},
#line 42 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"ZRANGE", goblin::core::CommandType::zrange},
#line 49 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HSET", goblin::core::CommandType::hset},
#line 61 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LPUSH", goblin::core::CommandType::lpush},
      {"",goblin::core::CommandType::unknown},
#line 43 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"ZRANK", goblin::core::CommandType::zrank},
#line 141 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.HSETGT", goblin::core::CommandType::goblin_hsetgt},
      {"",goblin::core::CommandType::unknown},
#line 31 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"TCL.EVAL", goblin::core::CommandType::tcl_eval},
#line 44 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"ZREVRANGE", goblin::core::CommandType::zrevrange},
#line 58 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HEXISTS", goblin::core::CommandType::hexists},
#line 24 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"SCRIPT", goblin::core::CommandType::script},
#line 57 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HLEN", goblin::core::CommandType::hlen},
#line 55 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HKEYS", goblin::core::CommandType::hkeys},
#line 96 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LSET", goblin::core::CommandType::segmented_lset},
#line 82 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.LRANGE", goblin::core::CommandType::pma_lrange},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 45 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"ZREVRANK", goblin::core::CommandType::zrevrank},
#line 91 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LPOP", goblin::core::CommandType::segmented_lpop},
#line 134 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.CAS", goblin::core::CommandType::goblin_cas},
      {"",goblin::core::CommandType::unknown},
#line 122 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"PEXPIREAT", goblin::core::CommandType::pexpireat},
#line 79 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.RPOP", goblin::core::CommandType::pma_rpop},
#line 93 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LLEN", goblin::core::CommandType::segmented_llen},
#line 59 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HSTRLEN", goblin::core::CommandType::hstrlen},
      {"",goblin::core::CommandType::unknown},
#line 107 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"INCR", goblin::core::CommandType::incr},
#line 56 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HVALS", goblin::core::CommandType::hvals},
#line 74 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.LPUSH", goblin::core::CommandType::pma_lpush},
#line 33 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"TCL.SCRIPT", goblin::core::CommandType::tcl_script},
      {"",goblin::core::CommandType::unknown},
#line 99 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LINSERT", goblin::core::CommandType::segmented_linsert},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 23 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"EVALSHA", goblin::core::CommandType::evalsha},
#line 121 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"EXPIREAT", goblin::core::CommandType::expireat},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 119 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"EXPIRE", goblin::core::CommandType::expire},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 118 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"TYPE", goblin::core::CommandType::key_type},
#line 62 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"RPUSH", goblin::core::CommandType::rpush},
#line 63 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LPUSHX", goblin::core::CommandType::lpushx},
#line 34 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"UPYTHON.EVAL", goblin::core::CommandType::upython_eval},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 103 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"SETNX", goblin::core::CommandType::setnx},
#line 117 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"EXISTS", goblin::core::CommandType::exists},
      {"",goblin::core::CommandType::unknown},
#line 95 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LRANGE", goblin::core::CommandType::segmented_lrange},
#line 115 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"MGET", goblin::core::CommandType::mget},
#line 71 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LTRIM", goblin::core::CommandType::ltrim},
#line 30 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"WREN.SCRIPT", goblin::core::CommandType::wren_script},
#line 133 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.CAEXPIRE", goblin::core::CommandType::goblin_caexpire},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 92 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.RPOP", goblin::core::CommandType::segmented_rpop},
#line 37 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"QUICKJS.EVAL", goblin::core::CommandType::quickjs_eval},
      {"",goblin::core::CommandType::unknown},
#line 114 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"MSET", goblin::core::CommandType::mset},
#line 116 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"DEL", goblin::core::CommandType::del},
#line 68 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LINDEX", goblin::core::CommandType::lindex},
#line 87 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LPUSH", goblin::core::CommandType::segmented_lpush},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 75 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.RPUSH", goblin::core::CommandType::pma_rpush},
#line 76 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.LPUSHX", goblin::core::CommandType::pma_lpushx},
#line 131 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.LOAD", goblin::core::CommandType::goblin_load},
      {"",goblin::core::CommandType::unknown},
#line 126 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"EXPIRETIME", goblin::core::CommandType::expiretime},
#line 27 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LUAU.SCRIPT", goblin::core::CommandType::luau_script},
      {"",goblin::core::CommandType::unknown},
#line 32 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"TCL.EVALSHA", goblin::core::CommandType::tcl_evalsha},
#line 36 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"UPYTHON.SCRIPT", goblin::core::CommandType::upython_script},
      {"",goblin::core::CommandType::unknown},
#line 84 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.LTRIM", goblin::core::CommandType::pma_ltrim},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 28 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"WREN.EVAL", goblin::core::CommandType::wren_eval},
      {"",goblin::core::CommandType::unknown},
#line 64 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"RPUSHX", goblin::core::CommandType::rpushx},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 109 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"INCRBY", goblin::core::CommandType::incrby},
#line 81 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.LINDEX", goblin::core::CommandType::pma_lindex},
      {"",goblin::core::CommandType::unknown},
#line 39 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"QUICKJS.SCRIPT", goblin::core::CommandType::quickjs_script},
#line 47 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"ZREMRANGEBYSCORE", goblin::core::CommandType::zremrangebyscore},
#line 139 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.DECRPOS", goblin::core::CommandType::goblin_decrpos},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 72 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LREM", goblin::core::CommandType::lrem},
#line 129 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.OPTIMIZE", goblin::core::CommandType::goblin_optimize},
#line 50 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HSETNX", goblin::core::CommandType::hsetnx},
      {"",goblin::core::CommandType::unknown},
#line 138 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.INCRBOUND", goblin::core::CommandType::goblin_incrbound},
#line 25 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LUAU.EVAL", goblin::core::CommandType::luau_eval},
#line 52 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HMGET", goblin::core::CommandType::hmget},
#line 108 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"DECR", goblin::core::CommandType::decr},
#line 88 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.RPUSH", goblin::core::CommandType::segmented_rpush},
#line 89 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LPUSHX", goblin::core::CommandType::segmented_lpushx},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 46 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"ZREM", goblin::core::CommandType::zrem},
#line 77 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.RPUSHX", goblin::core::CommandType::pma_rpushx},
#line 137 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.ZWINDOW", goblin::core::CommandType::goblin_zwindow},
#line 135 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.TD_LEADERBOARD_RESCORE", goblin::core::CommandType::goblin_td_leaderboard_rescore},
      {"",goblin::core::CommandType::unknown},
#line 53 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HDEL", goblin::core::CommandType::hdel},
#line 97 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LTRIM", goblin::core::CommandType::segmented_ltrim},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 140 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.HCAD", goblin::core::CommandType::goblin_hcad},
      {"",goblin::core::CommandType::unknown},
#line 85 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.PMA.LREM", goblin::core::CommandType::pma_lrem},
      {"",goblin::core::CommandType::unknown},
#line 60 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"HINCRBY", goblin::core::CommandType::hincrby},
#line 94 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LINDEX", goblin::core::CommandType::segmented_lindex},
#line 132 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.CAD", goblin::core::CommandType::goblin_cad},
#line 35 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"UPYTHON.EVALSHA", goblin::core::CommandType::upython_evalsha},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 41 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"ZCARD", goblin::core::CommandType::zcard},
      {"",goblin::core::CommandType::unknown},
#line 106 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"APPEND", goblin::core::CommandType::append},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 40 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"ZADD", goblin::core::CommandType::zadd},
      {"",goblin::core::CommandType::unknown},
#line 38 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"QUICKJS.EVALSHA", goblin::core::CommandType::quickjs_evalsha},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 90 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.RPUSHX", goblin::core::CommandType::segmented_rpushx},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 98 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LREM", goblin::core::CommandType::segmented_lrem},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 110 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"DECRBY", goblin::core::CommandType::decrby},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 136 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.INCREX", goblin::core::CommandType::goblin_increx},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 29 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"WREN.EVALSHA", goblin::core::CommandType::wren_evalsha},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 142 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.CLAIM", goblin::core::CommandType::goblin_claim},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 26 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"LUAU.EVALSHA", goblin::core::CommandType::luau_evalsha},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 128 "/Users/adam/dev/packrat/src/command_hash.gperf"
      {"GOBLIN.MEMORY", goblin::core::CommandType::goblin_memory}
    };

  if (len <= MAX_WORD_LENGTH && len >= MIN_WORD_LENGTH)
    {
      unsigned int key = hash (str, len);

      if (key <= MAX_HASH_VALUE)
        if (len == lengthtable[key])
          {
            const char *s = wordlist[key].name;

            if (*str == *s && !memcmp (str + 1, s + 1, len - 1))
              return &wordlist[key];
          }
    }
  return static_cast<struct CommandEntry *> (0);
}
#line 143 "/Users/adam/dev/packrat/src/command_hash.gperf"

