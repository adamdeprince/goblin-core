/* C++ code produced by gperf version 3.3 */
/* Command-line: gperf --output-file=src/command_hash.hpp src/command_hash.gperf  */
/* Computed positions: -k'1-2,8,$' */

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

#line 1 "src/command_hash.gperf"

// Perfect-hash command dispatch. Regenerate the checked-in header with:
//   gperf src/command_hash.gperf --output-file=src/command_hash.hpp
// (or: cmake --build <dir> --target goblin_core_regen_command_hash)
// Keywords are UPPER-CASE; the caller upper-cases the command name first (see
// parse_command in command.cpp), so this table is matched case-sensitively.
#include "goblin/core/command.hpp"
#line 17 "src/command_hash.gperf"
struct CommandEntry { const char* name; goblin::core::CommandType type; };

#define TOTAL_KEYWORDS 80
#define MIN_WORD_LENGTH 3
#define MAX_WORD_LENGTH 29
#define MIN_HASH_VALUE 8
#define MAX_HASH_VALUE 182
/* maximum key range = 175, duplicates = 0 */

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
  static const unsigned char asso_values[] =
    {
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183,  30, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183,  30, 183,  20,  30,   0,
      183,   0,   5,  10, 183,  10,  30,  80,  15,  10,
       70,   0,  45,  10,  10,  10,  95,  65,   5,  30,
       35, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183, 183, 183, 183, 183,
      183, 183, 183, 183, 183, 183
    };
  unsigned int hval = len;

  switch (hval)
    {
      default:
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
       0,  0,  0,  0,  0,  0,  0,  0,  8,  0,  0,  6,  0,  3,
       0,  0,  6,  0,  8,  4,  5,  6,  7,  3,  0, 10,  6,  0,
       0,  4,  5, 11,  0,  8,  4, 15,  6,  7, 13,  4,  0,  6,
       7,  0,  4, 15,  6,  0,  0, 29, 10,  6,  7,  3,  4,  0,
       0,  0,  0,  0, 10,  6,  0,  3, 14,  0,  6,  0,  0,  4,
      10,  0,  0,  0,  4,  0, 11,  7,  0,  4,  0, 11, 12,  0,
       4, 15,  6,  7,  0,  9,  5, 11,  0,  0,  4,  5, 16,  0,
       8,  4,  5, 11,  0,  0,  4,  0, 11,  0,  8,  9,  0,  0,
      12,  0,  4,  5,  0,  0,  0,  9,  0,  0,  0,  0, 14,  0,
       0,  0,  0,  4,  0,  0,  7, 13, 14,  0,  6,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 12,  0,
       0, 15,  0,  0,  0,  0,  0,  0,  0,  0,  4,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0, 11,  0,  0,  9,  0,  0,
      12
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
#line 73 "src/command_hash.gperf"
      {"GETRANGE", goblin::core::CommandType::getrange},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 80 "src/command_hash.gperf"
      {"EXPIRE", goblin::core::CommandType::expire},
      {"",goblin::core::CommandType::unknown},
#line 62 "src/command_hash.gperf"
      {"GET", goblin::core::CommandType::get},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 63 "src/command_hash.gperf"
      {"GETSET", goblin::core::CommandType::getset},
      {"",goblin::core::CommandType::unknown},
#line 74 "src/command_hash.gperf"
      {"SETRANGE", goblin::core::CommandType::setrange},
#line 51 "src/command_hash.gperf"
      {"HGET", goblin::core::CommandType::hget},
#line 64 "src/command_hash.gperf"
      {"SETNX", goblin::core::CommandType::setnx},
#line 78 "src/command_hash.gperf"
      {"EXISTS", goblin::core::CommandType::exists},
#line 58 "src/command_hash.gperf"
      {"HEXISTS", goblin::core::CommandType::hexists},
#line 61 "src/command_hash.gperf"
      {"SET", goblin::core::CommandType::set},
      {"",goblin::core::CommandType::unknown},
#line 87 "src/command_hash.gperf"
      {"EXPIRETIME", goblin::core::CommandType::expiretime},
#line 50 "src/command_hash.gperf"
      {"HSETNX", goblin::core::CommandType::hsetnx},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 49 "src/command_hash.gperf"
      {"HSET", goblin::core::CommandType::hset},
#line 55 "src/command_hash.gperf"
      {"HKEYS", goblin::core::CommandType::hkeys},
#line 91 "src/command_hash.gperf"
      {"GOBLIN.SAVE", goblin::core::CommandType::goblin_save},
      {"",goblin::core::CommandType::unknown},
#line 82 "src/command_hash.gperf"
      {"EXPIREAT", goblin::core::CommandType::expireat},
#line 20 "src/command_hash.gperf"
      {"ECHO", goblin::core::CommandType::echo},
#line 90 "src/command_hash.gperf"
      {"GOBLIN.OPTIMIZE", goblin::core::CommandType::goblin_optimize},
#line 65 "src/command_hash.gperf"
      {"GETDEL", goblin::core::CommandType::getdel},
#line 59 "src/command_hash.gperf"
      {"HSTRLEN", goblin::core::CommandType::hstrlen},
#line 97 "src/command_hash.gperf"
      {"GOBLIN.INCREX", goblin::core::CommandType::goblin_increx},
#line 21 "src/command_hash.gperf"
      {"INFO", goblin::core::CommandType::info},
      {"",goblin::core::CommandType::unknown},
#line 66 "src/command_hash.gperf"
      {"STRLEN", goblin::core::CommandType::strlen},
#line 54 "src/command_hash.gperf"
      {"HGETALL", goblin::core::CommandType::hgetall},
      {"",goblin::core::CommandType::unknown},
#line 79 "src/command_hash.gperf"
      {"TYPE", goblin::core::CommandType::key_type},
#line 94 "src/command_hash.gperf"
      {"GOBLIN.CAEXPIRE", goblin::core::CommandType::goblin_caexpire},
#line 24 "src/command_hash.gperf"
      {"SCRIPT", goblin::core::CommandType::script},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 96 "src/command_hash.gperf"
      {"GOBLIN.TD_LEADERBOARD_RESCORE", goblin::core::CommandType::goblin_td_leaderboard_rescore},
#line 95 "src/command_hash.gperf"
      {"GOBLIN.CAS", goblin::core::CommandType::goblin_cas},
#line 48 "src/command_hash.gperf"
      {"ZSCORE", goblin::core::CommandType::zscore},
#line 60 "src/command_hash.gperf"
      {"HINCRBY", goblin::core::CommandType::hincrby},
#line 84 "src/command_hash.gperf"
      {"TTL", goblin::core::CommandType::ttl},
#line 57 "src/command_hash.gperf"
      {"HLEN", goblin::core::CommandType::hlen},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 33 "src/command_hash.gperf"
      {"TCL.SCRIPT", goblin::core::CommandType::tcl_script},
#line 70 "src/command_hash.gperf"
      {"INCRBY", goblin::core::CommandType::incrby},
      {"",goblin::core::CommandType::unknown},
#line 77 "src/command_hash.gperf"
      {"DEL", goblin::core::CommandType::del},
#line 39 "src/command_hash.gperf"
      {"QUICKJS.SCRIPT", goblin::core::CommandType::quickjs_script},
      {"",goblin::core::CommandType::unknown},
#line 71 "src/command_hash.gperf"
      {"DECRBY", goblin::core::CommandType::decrby},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 53 "src/command_hash.gperf"
      {"HDEL", goblin::core::CommandType::hdel},
#line 93 "src/command_hash.gperf"
      {"GOBLIN.CAD", goblin::core::CommandType::goblin_cad},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 68 "src/command_hash.gperf"
      {"INCR", goblin::core::CommandType::incr},
      {"",goblin::core::CommandType::unknown},
#line 72 "src/command_hash.gperf"
      {"INCRBYFLOAT", goblin::core::CommandType::incrbyfloat},
#line 81 "src/command_hash.gperf"
      {"PEXPIRE", goblin::core::CommandType::pexpire},
      {"",goblin::core::CommandType::unknown},
#line 69 "src/command_hash.gperf"
      {"DECR", goblin::core::CommandType::decr},
      {"",goblin::core::CommandType::unknown},
#line 92 "src/command_hash.gperf"
      {"GOBLIN.LOAD", goblin::core::CommandType::goblin_load},
#line 37 "src/command_hash.gperf"
      {"QUICKJS.EVAL", goblin::core::CommandType::quickjs_eval},
      {"",goblin::core::CommandType::unknown},
#line 19 "src/command_hash.gperf"
      {"PING", goblin::core::CommandType::ping},
#line 38 "src/command_hash.gperf"
      {"QUICKJS.EVALSHA", goblin::core::CommandType::quickjs_evalsha},
#line 42 "src/command_hash.gperf"
      {"ZRANGE", goblin::core::CommandType::zrange},
#line 86 "src/command_hash.gperf"
      {"PERSIST", goblin::core::CommandType::persist},
      {"",goblin::core::CommandType::unknown},
#line 44 "src/command_hash.gperf"
      {"ZREVRANGE", goblin::core::CommandType::zrevrange},
#line 41 "src/command_hash.gperf"
      {"ZCARD", goblin::core::CommandType::zcard},
#line 88 "src/command_hash.gperf"
      {"PEXPIRETIME", goblin::core::CommandType::pexpiretime},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 76 "src/command_hash.gperf"
      {"MGET", goblin::core::CommandType::mget},
#line 43 "src/command_hash.gperf"
      {"ZRANK", goblin::core::CommandType::zrank},
#line 47 "src/command_hash.gperf"
      {"ZREMRANGEBYSCORE", goblin::core::CommandType::zremrangebyscore},
      {"",goblin::core::CommandType::unknown},
#line 31 "src/command_hash.gperf"
      {"TCL.EVAL", goblin::core::CommandType::tcl_eval},
#line 40 "src/command_hash.gperf"
      {"ZADD", goblin::core::CommandType::zadd},
#line 52 "src/command_hash.gperf"
      {"HMGET", goblin::core::CommandType::hmget},
#line 32 "src/command_hash.gperf"
      {"TCL.EVALSHA", goblin::core::CommandType::tcl_evalsha},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 75 "src/command_hash.gperf"
      {"MSET", goblin::core::CommandType::mset},
      {"",goblin::core::CommandType::unknown},
#line 27 "src/command_hash.gperf"
      {"LUAU.SCRIPT", goblin::core::CommandType::luau_script},
      {"",goblin::core::CommandType::unknown},
#line 45 "src/command_hash.gperf"
      {"ZREVRANK", goblin::core::CommandType::zrevrank},
#line 25 "src/command_hash.gperf"
      {"LUAU.EVAL", goblin::core::CommandType::luau_eval},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 26 "src/command_hash.gperf"
      {"LUAU.EVALSHA", goblin::core::CommandType::luau_evalsha},
      {"",goblin::core::CommandType::unknown},
#line 85 "src/command_hash.gperf"
      {"PTTL", goblin::core::CommandType::pttl},
#line 56 "src/command_hash.gperf"
      {"HVALS", goblin::core::CommandType::hvals},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 83 "src/command_hash.gperf"
      {"PEXPIREAT", goblin::core::CommandType::pexpireat},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 98 "src/command_hash.gperf"
      {"GOBLIN.ZWINDOW", goblin::core::CommandType::goblin_zwindow},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 22 "src/command_hash.gperf"
      {"EVAL", goblin::core::CommandType::eval},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 23 "src/command_hash.gperf"
      {"EVALSHA", goblin::core::CommandType::evalsha},
#line 89 "src/command_hash.gperf"
      {"GOBLIN.MEMORY", goblin::core::CommandType::goblin_memory},
#line 36 "src/command_hash.gperf"
      {"UPYTHON.SCRIPT", goblin::core::CommandType::upython_script},
      {"",goblin::core::CommandType::unknown},
#line 67 "src/command_hash.gperf"
      {"APPEND", goblin::core::CommandType::append},
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
#line 34 "src/command_hash.gperf"
      {"UPYTHON.EVAL", goblin::core::CommandType::upython_eval},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 35 "src/command_hash.gperf"
      {"UPYTHON.EVALSHA", goblin::core::CommandType::upython_evalsha},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 46 "src/command_hash.gperf"
      {"ZREM", goblin::core::CommandType::zrem},
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
#line 30 "src/command_hash.gperf"
      {"WREN.SCRIPT", goblin::core::CommandType::wren_script},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 28 "src/command_hash.gperf"
      {"WREN.EVAL", goblin::core::CommandType::wren_eval},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 29 "src/command_hash.gperf"
      {"WREN.EVALSHA", goblin::core::CommandType::wren_evalsha}
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
#line 99 "src/command_hash.gperf"

