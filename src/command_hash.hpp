/* C++ code produced by gperf version 3.3 */
/* Command-line: gperf --output-file=src/command_hash.hpp src/command_hash.gperf  */
/* Computed positions: -k'2,$' */

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

#define TOTAL_KEYWORDS 42
#define MIN_WORD_LENGTH 4
#define MAX_WORD_LENGTH 15
#define MIN_HASH_VALUE 4
#define MAX_HASH_VALUE 69
/* maximum key range = 66, duplicates = 0 */

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
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 10, 70, 15, 15,  0,
      70, 55, 70, 10, 70,  5,  5, 35, 45, 15,
      25, 70,  5,  0,  0, 35,  0, 70, 25,  5,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70, 70, 70, 70, 70,
      70, 70, 70, 70, 70, 70
    };
  return len + asso_values[static_cast<unsigned char>(str[1])] + asso_values[static_cast<unsigned char>(str[len - 1])];
}

const struct CommandEntry *
CommandDispatch::lookup (const char *str, size_t len)
{
  static const unsigned char lengthtable[] =
    {
       0,  0,  0,  0,  4,  5,  6,  7,  0,  4,  5,  6,  0,  0,
       9,  5, 11,  7,  8,  9,  0,  6,  7,  0,  4, 10, 11, 12,
       8,  4, 15,  6,  0, 13,  4,  5, 11,  0,  0, 14,  5, 11,
      12,  0,  4,  0, 11,  0,  0,  9, 15,  0,  7,  0,  4,  0,
       0, 12,  0,  4,  0,  0,  0,  0,  4,  0,  0,  7,  0,  4
    };
  static const struct CommandEntry wordlist[] =
    {
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 45 "src/command_hash.gperf"
      {"HSET", goblin::core::CommandType::hset},
#line 52 "src/command_hash.gperf"
      {"HVALS", goblin::core::CommandType::hvals},
#line 44 "src/command_hash.gperf"
      {"ZSCORE", goblin::core::CommandType::zscore},
#line 54 "src/command_hash.gperf"
      {"HEXISTS", goblin::core::CommandType::hexists},
      {"",goblin::core::CommandType::unknown},
#line 22 "src/command_hash.gperf"
      {"EVAL", goblin::core::CommandType::eval},
#line 51 "src/command_hash.gperf"
      {"HKEYS", goblin::core::CommandType::hkeys},
#line 39 "src/command_hash.gperf"
      {"ZRANGE", goblin::core::CommandType::zrange},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 41 "src/command_hash.gperf"
      {"ZREVRANGE", goblin::core::CommandType::zrevrange},
#line 40 "src/command_hash.gperf"
      {"ZRANK", goblin::core::CommandType::zrank},
#line 30 "src/command_hash.gperf"
      {"WREN.SCRIPT", goblin::core::CommandType::wren_script},
#line 23 "src/command_hash.gperf"
      {"EVALSHA", goblin::core::CommandType::evalsha},
#line 42 "src/command_hash.gperf"
      {"ZREVRANK", goblin::core::CommandType::zrevrank},
#line 28 "src/command_hash.gperf"
      {"WREN.EVAL", goblin::core::CommandType::wren_eval},
      {"",goblin::core::CommandType::unknown},
#line 24 "src/command_hash.gperf"
      {"SCRIPT", goblin::core::CommandType::script},
#line 56 "src/command_hash.gperf"
      {"HINCRBY", goblin::core::CommandType::hincrby},
      {"",goblin::core::CommandType::unknown},
#line 49 "src/command_hash.gperf"
      {"HDEL", goblin::core::CommandType::hdel},
#line 33 "src/command_hash.gperf"
      {"TCL.SCRIPT", goblin::core::CommandType::tcl_script},
#line 59 "src/command_hash.gperf"
      {"GOBLIN.SAVE", goblin::core::CommandType::goblin_save},
#line 29 "src/command_hash.gperf"
      {"WREN.EVALSHA", goblin::core::CommandType::wren_evalsha},
#line 31 "src/command_hash.gperf"
      {"TCL.EVAL", goblin::core::CommandType::tcl_eval},
#line 37 "src/command_hash.gperf"
      {"ZADD", goblin::core::CommandType::zadd},
#line 58 "src/command_hash.gperf"
      {"GOBLIN.OPTIMIZE", goblin::core::CommandType::goblin_optimize},
#line 46 "src/command_hash.gperf"
      {"HSETNX", goblin::core::CommandType::hsetnx},
      {"",goblin::core::CommandType::unknown},
#line 57 "src/command_hash.gperf"
      {"GOBLIN.MEMORY", goblin::core::CommandType::goblin_memory},
#line 20 "src/command_hash.gperf"
      {"ECHO", goblin::core::CommandType::echo},
#line 38 "src/command_hash.gperf"
      {"ZCARD", goblin::core::CommandType::zcard},
#line 32 "src/command_hash.gperf"
      {"TCL.EVALSHA", goblin::core::CommandType::tcl_evalsha},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 36 "src/command_hash.gperf"
      {"UPYTHON.SCRIPT", goblin::core::CommandType::upython_script},
#line 48 "src/command_hash.gperf"
      {"HMGET", goblin::core::CommandType::hmget},
#line 60 "src/command_hash.gperf"
      {"GOBLIN.LOAD", goblin::core::CommandType::goblin_load},
#line 34 "src/command_hash.gperf"
      {"UPYTHON.EVAL", goblin::core::CommandType::upython_eval},
      {"",goblin::core::CommandType::unknown},
#line 43 "src/command_hash.gperf"
      {"ZREM", goblin::core::CommandType::zrem},
      {"",goblin::core::CommandType::unknown},
#line 27 "src/command_hash.gperf"
      {"LUAU.SCRIPT", goblin::core::CommandType::luau_script},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 25 "src/command_hash.gperf"
      {"LUAU.EVAL", goblin::core::CommandType::luau_eval},
#line 35 "src/command_hash.gperf"
      {"UPYTHON.EVALSHA", goblin::core::CommandType::upython_evalsha},
      {"",goblin::core::CommandType::unknown},
#line 55 "src/command_hash.gperf"
      {"HSTRLEN", goblin::core::CommandType::hstrlen},
      {"",goblin::core::CommandType::unknown},
#line 53 "src/command_hash.gperf"
      {"HLEN", goblin::core::CommandType::hlen},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 26 "src/command_hash.gperf"
      {"LUAU.EVALSHA", goblin::core::CommandType::luau_evalsha},
      {"",goblin::core::CommandType::unknown},
#line 47 "src/command_hash.gperf"
      {"HGET", goblin::core::CommandType::hget},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 21 "src/command_hash.gperf"
      {"INFO", goblin::core::CommandType::info},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 50 "src/command_hash.gperf"
      {"HGETALL", goblin::core::CommandType::hgetall},
      {"",goblin::core::CommandType::unknown},
#line 19 "src/command_hash.gperf"
      {"PING", goblin::core::CommandType::ping}
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
#line 61 "src/command_hash.gperf"

