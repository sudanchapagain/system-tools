#pragma once

#define ALEN(v) (sizeof(v) / sizeof((v)[0]))
#define DO_PRAGMA(x) _Pragma(#x)
#define VERCMP(x, y, cx, cy) ((cx > x) || ((cx == x) && (cy >= y)))

#ifdef __has_include
    #define HAS_INCLUDE(x) __has_include(x)
#else
    #define HAS_INCLUDE(x) 0
#endif

#ifdef __has_attribute
    #define HAS_ATTRIBUTE(x) __has_attribute(x)
#else
    #define HAS_ATTRIBUTE(x) 0
#endif

#ifdef __has_feature
    #define HAS_FEATURE(x) __has_feature(x)
#else
    #define HAS_FEATURE(x) 0
#endif

#ifdef __has_builtin
    #define HAS_BUILTIN(x) __has_builtin(x)
#else
    #define HAS_BUILTIN(x) 0
#endif

// __has_extension() is a Clang macro used to determine if a feature is
// available even if not standardized in the current "-std" mode.
#ifdef __has_extension
    #define HAS_EXTENSION(x) __has_extension(x)
#else
    // Clang versions prior to 3.0 only supported __has_feature()
    #define HAS_EXTENSION(x) HAS_FEATURE(x)
#endif

#if defined(__GNUC__) && defined(__GNUC_MINOR__)
    #define GNUC_AT_LEAST(x, y) VERCMP(x, y, __GNUC__, __GNUC_MINOR__)
#else
    #define GNUC_AT_LEAST(x, y) 0
#endif

#if GNUC_AT_LEAST(3, 0) || HAS_ATTRIBUTE(unused) || defined(__TINYC__)
    #define UNUSED __attribute__((__unused__))
#else
    #define UNUSED
#endif

#if GNUC_AT_LEAST(3, 0) || HAS_ATTRIBUTE(malloc)
    #define MALLOC WARN_UNUSED_RESULT __attribute__((__malloc__))
#else
    #define MALLOC WARN_UNUSED_RESULT
#endif

#if GNUC_AT_LEAST(3, 0) || HAS_ATTRIBUTE(pure)
    #define PURE WARN_UNUSED_RESULT __attribute__((__pure__))
#else
    #define PURE WARN_UNUSED_RESULT
#endif

#if GNUC_AT_LEAST(3, 0) || HAS_ATTRIBUTE(const)
    #define CONST_FN WARN_UNUSED_RESULT __attribute__((__const__))
#else
    #define CONST_FN WARN_UNUSED_RESULT
#endif

#if GNUC_AT_LEAST(3, 0) || HAS_ATTRIBUTE(format)
    #define PRINTF(x) __attribute__((__format__(__printf__, (x), (x + 1))))
    #define VPRINTF(x) __attribute__((__format__(__printf__, (x), 0)))
#else
    #define PRINTF(x)
    #define VPRINTF(x)
#endif

#if (GNUC_AT_LEAST(3, 0) || HAS_BUILTIN(__builtin_expect)) && defined(__OPTIMIZE__)
    #define likely(x) __builtin_expect(!!(x), 1)
    #define unlikely(x) __builtin_expect(!!(x), 0)
#else
    #define likely(x) (x)
    #define unlikely(x) (x)
#endif

#if __STDC_VERSION__ >= 201112L
    #define noreturn _Noreturn
#elif GNUC_AT_LEAST(3, 0)
    #define noreturn __attribute__((__noreturn__))
#else
    #define noreturn
#endif

#if GNUC_AT_LEAST(4, 3) || HAS_ATTRIBUTE(cold)
    #define COLD __attribute__((__cold__))
#else
    #define COLD
#endif

#if GNUC_AT_LEAST(3, 3) || HAS_ATTRIBUTE(nonnull)
    #define NONNULL_ARGS __attribute__((__nonnull__))
    #define NONNULL_ARG(...) __attribute__((__nonnull__(__VA_ARGS__)))
#else
    #define NONNULL_ARGS
    #define NONNULL_ARG(...)
#endif

#if GNUC_AT_LEAST(3, 4) || HAS_ATTRIBUTE(warn_unused_result)
    #define WARN_UNUSED_RESULT __attribute__((__warn_unused_result__))
#else
    #define WARN_UNUSED_RESULT
#endif

#if GNUC_AT_LEAST(4, 3) || HAS_ATTRIBUTE(hot)
    #define HOT __attribute__((__hot__))
#else
    #define HOT
#endif

#if GNUC_AT_LEAST(4, 5) || HAS_BUILTIN(__builtin_unreachable)
    #define UNREACHABLE() __builtin_unreachable()
#else
    #define UNREACHABLE()
#endif

#if GNUC_AT_LEAST(5, 0) || HAS_ATTRIBUTE(returns_nonnull)
    #define RETURNS_NONNULL __attribute__((__returns_nonnull__))
#else
    #define RETURNS_NONNULL
#endif

#define XMALLOC MALLOC RETURNS_NONNULL
#define XSTRDUP XMALLOC NONNULL_ARGS

#ifdef __clang__
    #define IGNORE_WARNING(wflag) \
        DO_PRAGMA(clang diagnostic push) \
        DO_PRAGMA(clang diagnostic ignored "-Wunknown-pragmas") \
        DO_PRAGMA(clang diagnostic ignored "-Wunknown-warning-option") \
        DO_PRAGMA(clang diagnostic ignored wflag)
    #define UNIGNORE_WARNINGS DO_PRAGMA(clang diagnostic pop)
#elif GNUC_AT_LEAST(4, 6)
    #define IGNORE_WARNING(wflag) \
        DO_PRAGMA(GCC diagnostic push) \
        DO_PRAGMA(GCC diagnostic ignored "-Wpragmas") \
        DO_PRAGMA(GCC diagnostic ignored wflag)
    #define UNIGNORE_WARNINGS DO_PRAGMA(GCC diagnostic pop)
#else
    #define IGNORE_WARNING(wflag)
    #define UNIGNORE_WARNINGS
#endif
