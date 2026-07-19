/* C++ code produced by gperf version 3.3 */
/* Command-line: gperf --output-file=src/command_hash.hpp src/command_hash.gperf  */
/* Computed positions: -k'1-4,8,12-14,18-19,$' */

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

#define TOTAL_KEYWORDS 225
#define MIN_WORD_LENGTH 3
#define MAX_WORD_LENGTH 29
#define MIN_HASH_VALUE 9
#define MAX_HASH_VALUE 1180
/* maximum key range = 1172, duplicates = 0 */

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
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181,   55, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181,  190,   10,   30,   89,    5,
        15,    5,  326,   50, 1181,   80,    0,  100,    0,    0,
        90,    0,    5,   10,    5,  204,  190,   35,  225,   50,
       195, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181, 1181,
      1181, 1181, 1181, 1181, 1181, 1181
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
        hval += asso_values[static_cast<unsigned char>(str[13])];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
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
        hval += asso_values[static_cast<unsigned char>(str[3])];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
      case 3:
        hval += asso_values[static_cast<unsigned char>(str[2])];
#if (defined __cplusplus && (__cplusplus >= 201703L || (__cplusplus >= 201103L && defined __clang__ && __clang_major__ + (__clang_minor__ >= 9) > 3))) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 202000L && ((defined __GNUC__ && __GNUC__ >= 10) || (defined __clang__ && __clang_major__ >= 9)))
      [[fallthrough]];
#elif (defined __GNUC__ && __GNUC__ >= 7) || (defined __clang__ && __clang_major__ >= 10)
      __attribute__ ((__fallthrough__));
#endif
      /*FALLTHROUGH*/
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
       0,  0,  0,  0,  0,  0,  0,  0,  0,  4,  0,  0,  0,  3,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  3,  0,  0,  6,  0,
       3,  4,  0,  6,  0,  0,  0,  0,  6,  0,  8, 14,  0, 11,
       0,  8,  0, 15,  0,  0,  0,  0,  0, 16, 17,  0, 14, 15,
      21, 17,  0, 14, 15,  0,  0,  0,  0, 10, 11,  0, 23,  4,
       0, 21,  7,  0,  0,  0,  6, 17,  0,  0, 20,  0,  0,  0,
       0,  0, 11,  0, 23,  0, 20,  0,  0, 23,  4, 20,  6,  3,
       8,  0,  0, 11,  0, 18,  4,  0,  6,  0,  0,  0,  6,  0,
       0,  0, 24, 11, 21,  0,  0,  0,  0,  0,  7,  0,  4, 15,
       0,  0, 14,  4,  0,  0,  0,  0, 15,  0, 16,  0,  4,  0,
      15,  6,  0,  0, 10,  0,  0,  0,  0,  0, 15, 16,  0,  0,
       4, 10, 16,  0,  0,  4, 20, 21, 22,  0,  0,  5, 21, 17,
       0, 20, 20, 21,  0,  0,  0, 20,  0,  0, 23, 10, 15,  0,
       0, 18,  5,  6, 21,  0,  0,  9,  0,  0, 22,  0,  0, 10,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  5,  6,  0,  0,  0,
      15,  6,  0, 23,  4,  5,  0,  0,  0,  0,  5,  0,  0,  0,
       4,  0,  0,  0, 14,  0, 15, 16,  0, 13,  4,  0, 21,  7,
       0,  0,  0, 21,  0,  8,  9,  5,  6,  0,  0, 20,  5, 21,
       0,  0, 19,  0,  0, 12,  9,  0,  0,  0,  0,  8,  0, 15,
      21,  0,  4,  0,  6, 21,  0,  0,  4,  7,  0,  0,  0,  4,
       0, 16,  0,  0,  4, 11,  0,  0,  0,  0,  0, 11,  0,  0,
       5,  0,  6,  0,  0,  4, 15,  0,  0,  0,  0, 15,  6,  0,
       0,  0,  5,  6,  0,  0,  0,  0,  6, 22, 23,  0,  0, 11,
      17,  0,  0,  0,  7, 17,  0,  5,  6,  0,  7, 23,  0,  4,
       5,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  7,  7,  0,
       4, 12,  0,  7, 10,  4, 12,  0,  0,  0,  0,  0,  0,  0,
       5,  4,  0,  0,  0, 13,  6, 16,  0, 23,  0,  0,  0,  0,
      23, 10, 15,  6,  7, 14, 16,  0,  0,  0,  8,  4,  0,  0,
       0,  0,  0,  0,  0, 17,  0,  0,  0,  6,  0,  0,  0,  0,
       0,  0,  0,  4,  0,  0,  0,  0,  9,  0, 12,  0,  0,  0,
       0,  0,  7,  0,  4, 15,  0,  0,  0,  0,  5, 11,  0,  0,
       0, 10,  0,  0,  0, 11,  6, 11,  0,  0,  0,  0,  5,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 16,
       0,  7,  0,  0, 21,  0, 13,  0,  6,  4,  0, 22,  0,  5,
       5,  0, 22,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 14,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  5,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 29,  0,  0,  0,
       0, 13,  0,  6,  0,  0,  0,  0,  6,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  5,  0,  8,  0,  0,  0,  0, 14,  0,  0,  0,  0,  0,
       0,  0,  0,  6,  0,  0,  0,  0,  7,  0,  0,  0,  0,  0,
       0,  0,  0,  5,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  7,  0,  0,  0,  0,  0,  0,  9,  0,
       0,  0,  0, 11,  0,  0,  0,  7,  0,  0,  0, 12,  0,  0,
       0,  0,  0,  0,  0,  5,  0,  7,  0,  0,  5, 17,  0,  0,
       0,  0, 17,  0,  0,  0,  0,  0,  0,  0,  0,  0,  4,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  5,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 16,
       0,  0,  0,  0, 16,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  7,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  9,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 15,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0, 15,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  4,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
       0,  0,  0,  0, 12
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
      {"",goblin::core::CommandType::unknown},
#line 167 "src/command_hash.gperf"
      {"LLEN", goblin::core::CommandType::llen},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 224 "src/command_hash.gperf"
      {"TTL", goblin::core::CommandType::ttl},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 201 "src/command_hash.gperf"
      {"GET", goblin::core::CommandType::get},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 205 "src/command_hash.gperf"
      {"STRLEN", goblin::core::CommandType::strlen},
      {"",goblin::core::CommandType::unknown},
#line 200 "src/command_hash.gperf"
      {"SET", goblin::core::CommandType::set},
#line 170 "src/command_hash.gperf"
      {"LSET", goblin::core::CommandType::lset},
      {"",goblin::core::CommandType::unknown},
#line 24 "src/command_hash.gperf"
      {"SELECT", goblin::core::CommandType::select},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 202 "src/command_hash.gperf"
      {"GETSET", goblin::core::CommandType::getset},
      {"",goblin::core::CommandType::unknown},
#line 212 "src/command_hash.gperf"
      {"GETRANGE", goblin::core::CommandType::getrange},
#line 95 "src/command_hash.gperf"
      {"GOBLIN.RT.HLEN", goblin::core::CommandType::hlen},
      {"",goblin::core::CommandType::unknown},
#line 231 "src/command_hash.gperf"
      {"GOBLIN.SAVE", goblin::core::CommandType::goblin_save},
      {"",goblin::core::CommandType::unknown},
#line 213 "src/command_hash.gperf"
      {"SETRANGE", goblin::core::CommandType::setrange},
      {"",goblin::core::CommandType::unknown},
#line 145 "src/command_hash.gperf"
      {"GOBLIN.RT.ARLEN", goblin::core::CommandType::arlen},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 149 "src/command_hash.gperf"
      {"GOBLIN.RT.ARNEXT", goblin::core::CommandType::arnext},
#line 92 "src/command_hash.gperf"
      {"GOBLIN.RT.HGETALL", goblin::core::CommandType::hgetall},
      {"",goblin::core::CommandType::unknown},
#line 89 "src/command_hash.gperf"
      {"GOBLIN.RT.HGET", goblin::core::CommandType::hget},
#line 142 "src/command_hash.gperf"
      {"GOBLIN.RT.ARGET", goblin::core::CommandType::arget},
#line 193 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LLEN", goblin::core::CommandType::segmented_llen},
#line 97 "src/command_hash.gperf"
      {"GOBLIN.RT.HSTRLEN", goblin::core::CommandType::hstrlen},
      {"",goblin::core::CommandType::unknown},
#line 87 "src/command_hash.gperf"
      {"GOBLIN.RT.HSET", goblin::core::CommandType::hset},
#line 141 "src/command_hash.gperf"
      {"GOBLIN.RT.ARSET", goblin::core::CommandType::arset},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 235 "src/command_hash.gperf"
      {"GOBLIN.CAS", goblin::core::CommandType::goblin_cas},
#line 47 "src/command_hash.gperf"
      {"WREN.SCRIPT", goblin::core::CommandType::wren_script},
      {"",goblin::core::CommandType::unknown},
#line 195 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LRANGE", goblin::core::CommandType::segmented_lrange},
#line 38 "src/command_hash.gperf"
      {"INFO", goblin::core::CommandType::info},
      {"",goblin::core::CommandType::unknown},
#line 196 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LSET", goblin::core::CommandType::segmented_lset},
#line 173 "src/command_hash.gperf"
      {"LINSERT", goblin::core::CommandType::linsert},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 122 "src/command_hash.gperf"
      {"SINTER", goblin::core::CommandType::sinter},
#line 146 "src/command_hash.gperf"
      {"GOBLIN.RT.ARCOUNT", goblin::core::CommandType::arcount},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 108 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HLEN", goblin::core::CommandType::hlen},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 123 "src/command_hash.gperf"
      {"SINTERSTORE", goblin::core::CommandType::sinterstore},
      {"",goblin::core::CommandType::unknown},
#line 105 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HGETALL", goblin::core::CommandType::hgetall},
      {"",goblin::core::CommandType::unknown},
#line 102 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HGET", goblin::core::CommandType::hget},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 110 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HSTRLEN", goblin::core::CommandType::hstrlen},
#line 207 "src/command_hash.gperf"
      {"INCR", goblin::core::CommandType::incr},
#line 100 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HSET", goblin::core::CommandType::hset},
#line 23 "src/command_hash.gperf"
      {"CLIENT", goblin::core::CommandType::client},
#line 216 "src/command_hash.gperf"
      {"DEL", goblin::core::CommandType::del},
#line 48 "src/command_hash.gperf"
      {"TCL.EVAL", goblin::core::CommandType::tcl_eval},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 211 "src/command_hash.gperf"
      {"INCRBYFLOAT", goblin::core::CommandType::incrbyfloat},
      {"",goblin::core::CommandType::unknown},
#line 148 "src/command_hash.gperf"
      {"GOBLIN.RT.ARINSERT", goblin::core::CommandType::arinsert},
#line 225 "src/command_hash.gperf"
      {"PTTL", goblin::core::CommandType::pttl},
      {"",goblin::core::CommandType::unknown},
#line 41 "src/command_hash.gperf"
      {"SCRIPT", goblin::core::CommandType::script},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 204 "src/command_hash.gperf"
      {"GETDEL", goblin::core::CommandType::getdel},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 199 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LINSERT", goblin::core::CommandType::segmented_linsert},
#line 232 "src/command_hash.gperf"
      {"GOBLIN.LOAD", goblin::core::CommandType::goblin_load},
#line 112 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HSCAN", goblin::core::CommandType::hscan},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 226 "src/command_hash.gperf"
      {"PERSIST", goblin::core::CommandType::persist},
      {"",goblin::core::CommandType::unknown},
#line 215 "src/command_hash.gperf"
      {"MGET", goblin::core::CommandType::mget},
#line 180 "src/command_hash.gperf"
      {"GOBLIN.PMA.LLEN", goblin::core::CommandType::pma_llen},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 91 "src/command_hash.gperf"
      {"GOBLIN.RT.HDEL", goblin::core::CommandType::hdel},
#line 214 "src/command_hash.gperf"
      {"MSET", goblin::core::CommandType::mset},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 147 "src/command_hash.gperf"
      {"GOBLIN.RT.ARDEL", goblin::core::CommandType::ardel},
      {"",goblin::core::CommandType::unknown},
#line 150 "src/command_hash.gperf"
      {"GOBLIN.RT.ARSEEK", goblin::core::CommandType::arseek},
      {"",goblin::core::CommandType::unknown},
#line 208 "src/command_hash.gperf"
      {"DECR", goblin::core::CommandType::decr},
      {"",goblin::core::CommandType::unknown},
#line 183 "src/command_hash.gperf"
      {"GOBLIN.PMA.LSET", goblin::core::CommandType::pma_lset},
#line 209 "src/command_hash.gperf"
      {"INCRBY", goblin::core::CommandType::incrby},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 233 "src/command_hash.gperf"
      {"GOBLIN.CAD", goblin::core::CommandType::goblin_cad},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 90 "src/command_hash.gperf"
      {"GOBLIN.RT.HMGET", goblin::core::CommandType::hmget},
#line 144 "src/command_hash.gperf"
      {"GOBLIN.RT.ARMGET", goblin::core::CommandType::armget},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 19 "src/command_hash.gperf"
      {"PING", goblin::core::CommandType::ping},
#line 50 "src/command_hash.gperf"
      {"TCL.SCRIPT", goblin::core::CommandType::tcl_script},
#line 143 "src/command_hash.gperf"
      {"GOBLIN.RT.ARMSET", goblin::core::CommandType::armset},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 219 "src/command_hash.gperf"
      {"TYPE", goblin::core::CommandType::key_type},
#line 155 "src/command_hash.gperf"
      {"GOBLIN.CLASSIC.ARLEN", goblin::core::CommandType::arlen},
#line 198 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LREM", goblin::core::CommandType::segmented_lrem},
#line 197 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LTRIM", goblin::core::CommandType::segmented_ltrim},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 171 "src/command_hash.gperf"
      {"LTRIM", goblin::core::CommandType::ltrim},
#line 159 "src/command_hash.gperf"
      {"GOBLIN.CLASSIC.ARNEXT", goblin::core::CommandType::arnext},
#line 98 "src/command_hash.gperf"
      {"GOBLIN.RT.HINCRBY", goblin::core::CommandType::hincrby},
      {"",goblin::core::CommandType::unknown},
#line 104 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HDEL", goblin::core::CommandType::hdel},
#line 152 "src/command_hash.gperf"
      {"GOBLIN.CLASSIC.ARGET", goblin::core::CommandType::arget},
#line 106 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HKEYS", goblin::core::CommandType::hkeys},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 151 "src/command_hash.gperf"
      {"GOBLIN.CLASSIC.ARSET", goblin::core::CommandType::arset},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 111 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HINCRBY", goblin::core::CommandType::hincrby},
#line 128 "src/command_hash.gperf"
      {"SDIFFSTORE", goblin::core::CommandType::sdiffstore},
#line 93 "src/command_hash.gperf"
      {"GOBLIN.RT.HKEYS", goblin::core::CommandType::hkeys},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 186 "src/command_hash.gperf"
      {"GOBLIN.PMA.LINSERT", goblin::core::CommandType::pma_linsert},
#line 127 "src/command_hash.gperf"
      {"SDIFF", goblin::core::CommandType::sdiff},
#line 210 "src/command_hash.gperf"
      {"DECRBY", goblin::core::CommandType::decrby},
#line 103 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HMGET", goblin::core::CommandType::hmget},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 116 "src/command_hash.gperf"
      {"SISMEMBER", goblin::core::CommandType::sismember},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 156 "src/command_hash.gperf"
      {"GOBLIN.CLASSIC.ARCOUNT", goblin::core::CommandType::arcount},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 117 "src/command_hash.gperf"
      {"SMISMEMBER", goblin::core::CommandType::smismember},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 135 "src/command_hash.gperf"
      {"ARLEN", goblin::core::CommandType::arlen},
#line 169 "src/command_hash.gperf"
      {"LRANGE", goblin::core::CommandType::lrange},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 234 "src/command_hash.gperf"
      {"GOBLIN.CAEXPIRE", goblin::core::CommandType::goblin_caexpire},
#line 139 "src/command_hash.gperf"
      {"ARNEXT", goblin::core::CommandType::arnext},
      {"",goblin::core::CommandType::unknown},
#line 158 "src/command_hash.gperf"
      {"GOBLIN.CLASSIC.ARINSERT", goblin::core::CommandType::arinsert},
#line 172 "src/command_hash.gperf"
      {"LREM", goblin::core::CommandType::lrem},
#line 132 "src/command_hash.gperf"
      {"ARGET", goblin::core::CommandType::arget},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 131 "src/command_hash.gperf"
      {"ARSET", goblin::core::CommandType::arset},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 114 "src/command_hash.gperf"
      {"SREM", goblin::core::CommandType::srem},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 240 "src/command_hash.gperf"
      {"GOBLIN.DECRPOS", goblin::core::CommandType::goblin_decrpos},
      {"",goblin::core::CommandType::unknown},
#line 185 "src/command_hash.gperf"
      {"GOBLIN.PMA.LREM", goblin::core::CommandType::pma_lrem},
#line 184 "src/command_hash.gperf"
      {"GOBLIN.PMA.LTRIM", goblin::core::CommandType::pma_ltrim},
      {"",goblin::core::CommandType::unknown},
#line 229 "src/command_hash.gperf"
      {"GOBLIN.MEMORY", goblin::core::CommandType::goblin_memory},
#line 218 "src/command_hash.gperf"
      {"SCAN", goblin::core::CommandType::scan},
      {"",goblin::core::CommandType::unknown},
#line 191 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LPOP", goblin::core::CommandType::segmented_lpop},
#line 136 "src/command_hash.gperf"
      {"ARCOUNT", goblin::core::CommandType::arcount},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 192 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.RPOP", goblin::core::CommandType::segmented_rpop},
      {"",goblin::core::CommandType::unknown},
#line 118 "src/command_hash.gperf"
      {"SMEMBERS", goblin::core::CommandType::smembers},
#line 45 "src/command_hash.gperf"
      {"WREN.EVAL", goblin::core::CommandType::wren_eval},
#line 129 "src/command_hash.gperf"
      {"SSCAN", goblin::core::CommandType::sscan},
#line 73 "src/command_hash.gperf"
      {"ZSCORE", goblin::core::CommandType::zscore},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 157 "src/command_hash.gperf"
      {"GOBLIN.CLASSIC.ARDEL", goblin::core::CommandType::ardel},
#line 203 "src/command_hash.gperf"
      {"SETNX", goblin::core::CommandType::setnx},
#line 160 "src/command_hash.gperf"
      {"GOBLIN.CLASSIC.ARSEEK", goblin::core::CommandType::arseek},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 130 "src/command_hash.gperf"
      {"GOBLIN.RT.ARRESERVE", goblin::core::CommandType::arreserve},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 243 "src/command_hash.gperf"
      {"GOBLIN.CLAIM", goblin::core::CommandType::goblin_claim},
#line 31 "src/command_hash.gperf"
      {"SUBSCRIBE", goblin::core::CommandType::subscribe},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 138 "src/command_hash.gperf"
      {"ARINSERT", goblin::core::CommandType::arinsert},
      {"",goblin::core::CommandType::unknown},
#line 99 "src/command_hash.gperf"
      {"GOBLIN.RT.HSCAN", goblin::core::CommandType::hscan},
#line 154 "src/command_hash.gperf"
      {"GOBLIN.CLASSIC.ARMGET", goblin::core::CommandType::armget},
      {"",goblin::core::CommandType::unknown},
#line 25 "src/command_hash.gperf"
      {"QUIT", goblin::core::CommandType::quit},
      {"",goblin::core::CommandType::unknown},
#line 125 "src/command_hash.gperf"
      {"SUNION", goblin::core::CommandType::sunion},
#line 153 "src/command_hash.gperf"
      {"GOBLIN.CLASSIC.ARMSET", goblin::core::CommandType::armset},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 165 "src/command_hash.gperf"
      {"LPOP", goblin::core::CommandType::lpop},
#line 28 "src/command_hash.gperf"
      {"DISCARD", goblin::core::CommandType::discard},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 166 "src/command_hash.gperf"
      {"RPOP", goblin::core::CommandType::rpop},
      {"",goblin::core::CommandType::unknown},
#line 88 "src/command_hash.gperf"
      {"GOBLIN.RT.HSETNX", goblin::core::CommandType::hsetnx},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 119 "src/command_hash.gperf"
      {"SPOP", goblin::core::CommandType::spop},
#line 126 "src/command_hash.gperf"
      {"SUNIONSTORE", goblin::core::CommandType::sunionstore},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 49 "src/command_hash.gperf"
      {"TCL.EVALSHA", goblin::core::CommandType::tcl_evalsha},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 137 "src/command_hash.gperf"
      {"ARDEL", goblin::core::CommandType::ardel},
      {"",goblin::core::CommandType::unknown},
#line 140 "src/command_hash.gperf"
      {"ARSEEK", goblin::core::CommandType::arseek},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 27 "src/command_hash.gperf"
      {"EXEC", goblin::core::CommandType::exec},
#line 178 "src/command_hash.gperf"
      {"GOBLIN.PMA.LPOP", goblin::core::CommandType::pma_lpop},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 179 "src/command_hash.gperf"
      {"GOBLIN.PMA.RPOP", goblin::core::CommandType::pma_rpop},
#line 217 "src/command_hash.gperf"
      {"EXISTS", goblin::core::CommandType::exists},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 121 "src/command_hash.gperf"
      {"SMOVE", goblin::core::CommandType::smove},
#line 134 "src/command_hash.gperf"
      {"ARMGET", goblin::core::CommandType::armget},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 133 "src/command_hash.gperf"
      {"ARMSET", goblin::core::CommandType::armset},
#line 101 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HSETNX", goblin::core::CommandType::hsetnx},
#line 109 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HEXISTS", goblin::core::CommandType::hexists},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 120 "src/command_hash.gperf"
      {"SRANDMEMBER", goblin::core::CommandType::srandmember},
#line 182 "src/command_hash.gperf"
      {"GOBLIN.PMA.LRANGE", goblin::core::CommandType::pma_lrange},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 22 "src/command_hash.gperf"
      {"COMMAND", goblin::core::CommandType::command},
#line 96 "src/command_hash.gperf"
      {"GOBLIN.RT.HEXISTS", goblin::core::CommandType::hexists},
      {"",goblin::core::CommandType::unknown},
#line 115 "src/command_hash.gperf"
      {"SCARD", goblin::core::CommandType::scard},
#line 36 "src/command_hash.gperf"
      {"PUBSUB", goblin::core::CommandType::pubsub},
      {"",goblin::core::CommandType::unknown},
#line 58 "src/command_hash.gperf"
      {"ZINCRBY", goblin::core::CommandType::zincrby},
#line 194 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LINDEX", goblin::core::CommandType::segmented_lindex},
      {"",goblin::core::CommandType::unknown},
#line 82 "src/command_hash.gperf"
      {"HLEN", goblin::core::CommandType::hlen},
#line 20 "src/command_hash.gperf"
      {"HELLO", goblin::core::CommandType::hello},
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
#line 69 "src/command_hash.gperf"
      {"ZMSCORE", goblin::core::CommandType::zmscore},
#line 79 "src/command_hash.gperf"
      {"HGETALL", goblin::core::CommandType::hgetall},
      {"",goblin::core::CommandType::unknown},
#line 76 "src/command_hash.gperf"
      {"HGET", goblin::core::CommandType::hget},
#line 54 "src/command_hash.gperf"
      {"QUICKJS.EVAL", goblin::core::CommandType::quickjs_eval},
      {"",goblin::core::CommandType::unknown},
#line 84 "src/command_hash.gperf"
      {"HSTRLEN", goblin::core::CommandType::hstrlen},
#line 124 "src/command_hash.gperf"
      {"SINTERCARD", goblin::core::CommandType::sintercard},
#line 74 "src/command_hash.gperf"
      {"HSET", goblin::core::CommandType::hset},
#line 34 "src/command_hash.gperf"
      {"PUNSUBSCRIBE", goblin::core::CommandType::punsubscribe},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 26 "src/command_hash.gperf"
      {"MULTI", goblin::core::CommandType::multi},
#line 37 "src/command_hash.gperf"
      {"ECHO", goblin::core::CommandType::echo},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 242 "src/command_hash.gperf"
      {"GOBLIN.HSETGT", goblin::core::CommandType::goblin_hsetgt},
#line 168 "src/command_hash.gperf"
      {"LINDEX", goblin::core::CommandType::lindex},
#line 68 "src/command_hash.gperf"
      {"ZREMRANGEBYSCORE", goblin::core::CommandType::zremrangebyscore},
      {"",goblin::core::CommandType::unknown},
#line 189 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LPUSHX", goblin::core::CommandType::segmented_lpushx},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 190 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.RPUSHX", goblin::core::CommandType::segmented_rpushx},
#line 33 "src/command_hash.gperf"
      {"PSUBSCRIBE", goblin::core::CommandType::psubscribe},
#line 230 "src/command_hash.gperf"
      {"GOBLIN.OPTIMIZE", goblin::core::CommandType::goblin_optimize},
#line 220 "src/command_hash.gperf"
      {"EXPIRE", goblin::core::CommandType::expire},
#line 70 "src/command_hash.gperf"
      {"ZPOPMIN", goblin::core::CommandType::zpopmin},
#line 238 "src/command_hash.gperf"
      {"GOBLIN.ZWINDOW", goblin::core::CommandType::goblin_zwindow},
#line 239 "src/command_hash.gperf"
      {"GOBLIN.INCRBOUND", goblin::core::CommandType::goblin_incrbound},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 222 "src/command_hash.gperf"
      {"EXPIREAT", goblin::core::CommandType::expireat},
#line 39 "src/command_hash.gperf"
      {"EVAL", goblin::core::CommandType::eval},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 181 "src/command_hash.gperf"
      {"GOBLIN.PMA.LINDEX", goblin::core::CommandType::pma_lindex},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 61 "src/command_hash.gperf"
      {"ZRANGE", goblin::core::CommandType::zrange},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 67 "src/command_hash.gperf"
      {"ZREM", goblin::core::CommandType::zrem},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 65 "src/command_hash.gperf"
      {"ZREVRANGE", goblin::core::CommandType::zrevrange},
      {"",goblin::core::CommandType::unknown},
#line 51 "src/command_hash.gperf"
      {"UPYTHON.EVAL", goblin::core::CommandType::upython_eval},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 221 "src/command_hash.gperf"
      {"PEXPIRE", goblin::core::CommandType::pexpire},
      {"",goblin::core::CommandType::unknown},
#line 78 "src/command_hash.gperf"
      {"HDEL", goblin::core::CommandType::hdel},
#line 94 "src/command_hash.gperf"
      {"GOBLIN.RT.HVALS", goblin::core::CommandType::hvals},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 72 "src/command_hash.gperf"
      {"ZSCAN", goblin::core::CommandType::zscan},
#line 228 "src/command_hash.gperf"
      {"PEXPIRETIME", goblin::core::CommandType::pexpiretime},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 227 "src/command_hash.gperf"
      {"EXPIRETIME", goblin::core::CommandType::expiretime},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 32 "src/command_hash.gperf"
      {"UNSUBSCRIBE", goblin::core::CommandType::unsubscribe},
#line 60 "src/command_hash.gperf"
      {"ZCOUNT", goblin::core::CommandType::zcount},
#line 241 "src/command_hash.gperf"
      {"GOBLIN.HCAD", goblin::core::CommandType::goblin_hcad},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 77 "src/command_hash.gperf"
      {"HMGET", goblin::core::CommandType::hmget},
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
#line 63 "src/command_hash.gperf"
      {"ZREVRANGEBYSCORE", goblin::core::CommandType::zrevrangebyscore},
      {"",goblin::core::CommandType::unknown},
#line 85 "src/command_hash.gperf"
      {"HINCRBY", goblin::core::CommandType::hincrby},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 107 "src/command_hash.gperf"
      {"GOBLIN.EFFICENT.HVALS", goblin::core::CommandType::hvals},
      {"",goblin::core::CommandType::unknown},
#line 62 "src/command_hash.gperf"
      {"ZRANGEBYSCORE", goblin::core::CommandType::zrangebyscore},
      {"",goblin::core::CommandType::unknown},
#line 206 "src/command_hash.gperf"
      {"APPEND", goblin::core::CommandType::append},
#line 113 "src/command_hash.gperf"
      {"SADD", goblin::core::CommandType::sadd},
      {"",goblin::core::CommandType::unknown},
#line 187 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.LPUSH", goblin::core::CommandType::segmented_lpush},
      {"",goblin::core::CommandType::unknown},
#line 64 "src/command_hash.gperf"
      {"ZRANK", goblin::core::CommandType::zrank},
#line 80 "src/command_hash.gperf"
      {"HKEYS", goblin::core::CommandType::hkeys},
      {"",goblin::core::CommandType::unknown},
#line 188 "src/command_hash.gperf"
      {"GOBLIN.SEGMENTED.RPUSH", goblin::core::CommandType::segmented_rpush},
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
#line 56 "src/command_hash.gperf"
      {"QUICKJS.SCRIPT", goblin::core::CommandType::quickjs_script},
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
#line 59 "src/command_hash.gperf"
      {"ZCARD", goblin::core::CommandType::zcard},
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
#line 236 "src/command_hash.gperf"
      {"GOBLIN.TD_LEADERBOARD_RESCORE", goblin::core::CommandType::goblin_td_leaderboard_rescore},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 237 "src/command_hash.gperf"
      {"GOBLIN.INCREX", goblin::core::CommandType::goblin_increx},
      {"",goblin::core::CommandType::unknown},
#line 163 "src/command_hash.gperf"
      {"LPUSHX", goblin::core::CommandType::lpushx},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 164 "src/command_hash.gperf"
      {"RPUSHX", goblin::core::CommandType::rpushx},
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
#line 86 "src/command_hash.gperf"
      {"HSCAN", goblin::core::CommandType::hscan},
      {"",goblin::core::CommandType::unknown},
#line 66 "src/command_hash.gperf"
      {"ZREVRANK", goblin::core::CommandType::zrevrank},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 53 "src/command_hash.gperf"
      {"UPYTHON.SCRIPT", goblin::core::CommandType::upython_script},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 75 "src/command_hash.gperf"
      {"HSETNX", goblin::core::CommandType::hsetnx},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 40 "src/command_hash.gperf"
      {"EVALSHA", goblin::core::CommandType::evalsha},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 29 "src/command_hash.gperf"
      {"WATCH", goblin::core::CommandType::watch},
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
#line 71 "src/command_hash.gperf"
      {"ZPOPMAX", goblin::core::CommandType::zpopmax},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 223 "src/command_hash.gperf"
      {"PEXPIREAT", goblin::core::CommandType::pexpireat},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 44 "src/command_hash.gperf"
      {"LUAU.SCRIPT", goblin::core::CommandType::luau_script},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 83 "src/command_hash.gperf"
      {"HEXISTS", goblin::core::CommandType::hexists},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 46 "src/command_hash.gperf"
      {"WREN.EVALSHA", goblin::core::CommandType::wren_evalsha},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 161 "src/command_hash.gperf"
      {"LPUSH", goblin::core::CommandType::lpush},
      {"",goblin::core::CommandType::unknown},
#line 35 "src/command_hash.gperf"
      {"PUBLISH", goblin::core::CommandType::publish},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 162 "src/command_hash.gperf"
      {"RPUSH", goblin::core::CommandType::rpush},
#line 176 "src/command_hash.gperf"
      {"GOBLIN.PMA.LPUSHX", goblin::core::CommandType::pma_lpushx},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 177 "src/command_hash.gperf"
      {"GOBLIN.PMA.RPUSHX", goblin::core::CommandType::pma_rpushx},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 57 "src/command_hash.gperf"
      {"ZADD", goblin::core::CommandType::zadd},
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
#line 81 "src/command_hash.gperf"
      {"HVALS", goblin::core::CommandType::hvals},
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
#line 174 "src/command_hash.gperf"
      {"GOBLIN.PMA.LPUSH", goblin::core::CommandType::pma_lpush},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 175 "src/command_hash.gperf"
      {"GOBLIN.PMA.RPUSH", goblin::core::CommandType::pma_rpush},
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
#line 30 "src/command_hash.gperf"
      {"UNWATCH", goblin::core::CommandType::unwatch},
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
#line 42 "src/command_hash.gperf"
      {"LUAU.EVAL", goblin::core::CommandType::luau_eval},
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
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
      {"",goblin::core::CommandType::unknown},
#line 55 "src/command_hash.gperf"
      {"QUICKJS.EVALSHA", goblin::core::CommandType::quickjs_evalsha},
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
#line 52 "src/command_hash.gperf"
      {"UPYTHON.EVALSHA", goblin::core::CommandType::upython_evalsha},
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
#line 21 "src/command_hash.gperf"
      {"AUTH", goblin::core::CommandType::auth},
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
#line 43 "src/command_hash.gperf"
      {"LUAU.EVALSHA", goblin::core::CommandType::luau_evalsha}
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
#line 244 "src/command_hash.gperf"

