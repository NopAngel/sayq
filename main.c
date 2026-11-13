#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROGRAM_NAME "sayq"

#define CHAR_SQUOTE '\''
#define CHAR_DQUOTE '\"'
#define CHAR_BSLASH '\\'

typedef enum { OUT_STANDARD, OUT_JSON, OUT_CSV } out_mode_t;

typedef struct {
  bool no_newline;
  bool interpret_escapes;
  bool posixly_correct;
  bool use_stderr;
  bool format_mode;
  const char *format_str;
  const char *separator;
  const char *color_code;
  out_mode_t out_mode;
} sayq_config_t;

static void print_help(void) {
  printf("Use: %s [OPT]... [ARGS]...\n", PROGRAM_NAME);
}

static void print_version(void) { printf("%s 1.0.0\n", PROGRAM_NAME); }

static const char *resolve_ansi_color(const char *name) {
  if (strcmp(name, "red") == 0) return "\033[31m";
  if (strcmp(name, "green") == 0) return "\033[32m";
  if (strcmp(name, "blue") == 0) return "\033[34m";
  if (strcmp(name, "yellow") == 0) return "\033[33m";
  return NULL;
}

static bool print_escaped_string(const char *str, FILE *out,
                                 bool interpret_octal_hex) {
  if (!str) return false;
  for (const char *p = str; *p; p++) {
    if (interpret_octal_hex && *p == CHAR_BSLASH) {
      p++;
      if (*p == 'c') {
        return true;  // stop
      }
      if (*p == 'n')
        fputc('\n', out);
      else if (*p == 't')
        fputc('\t', out);
      else if (*p == 'r')
        fputc('\r', out);
      else if (*p == '\\')
        fputc('\\', out);
      else
        fputc(*p, out);
    } else {
      fputc(*p, out);
    }
  }
  return false;
}

static void print_shell_quoted(const char *str, FILE *out) {
  if (!str || !*str) {
    fputs("''\n", out);
    return;
  }
  fputc(CHAR_SQUOTE, out);
  for (const char *p = str; *p; p++) {
    if (*p == CHAR_SQUOTE) {
      fputc(CHAR_SQUOTE, out);
      fputc(CHAR_BSLASH, out);
      fputc(CHAR_SQUOTE, out);
      fputc(CHAR_SQUOTE, out);
    } else {
      fputc(*p, out);
    }
  }
  fputc(CHAR_SQUOTE, out);
}

static intmax_t parse_printf_int(const char *arg) {
  if (!arg || !*arg) return 0;
  const char *next = arg + 1;
  if ((*arg == CHAR_SQUOTE || *arg == CHAR_DQUOTE) && *next != '\0') {
    return (unsigned char)*next;
  }
  char *endptr = NULL;
  errno = 0;
  intmax_t val = strtoimax(arg, &endptr, 0);
  if (errno == ERANGE) {
    fprintf(stderr, "%s: warning: %s: numerical result out of range\n",
            PROGRAM_NAME, arg);
  } else if (*endptr != '\0') {
    fprintf(stderr, "%s: warning: %s: value not completely converted\n",
            PROGRAM_NAME, arg);
  }
  return val;
}

static uintmax_t parse_printf_uint(const char *arg) {
  if (!arg || !*arg) return 0;
  const char *next = arg + 1;
  if ((*arg == CHAR_SQUOTE || *arg == CHAR_DQUOTE) && *next != '\0') {
    return (unsigned char)*next;
  }
  char *endptr = NULL;
  errno = 0;
  uintmax_t val = strtoumax(arg, &endptr, 0);
  if (errno == ERANGE) {
    fprintf(stderr, "%s: warning: %s: numerical result out of range\n",
            PROGRAM_NAME, arg);
  } else if (*endptr != '\0') {
    fprintf(stderr, "%s: warning: %s: value not completely converted\n",
            PROGRAM_NAME, arg);
  }
  return val;
}

static double parse_printf_float(const char *arg) {
  if (!arg || !*arg) return 0.0;
  const char *next = arg + 1;
  if ((*arg == CHAR_SQUOTE || *arg == CHAR_DQUOTE) && *next != '\0') {
    return (double)(unsigned char)*next;
  }
  char *endptr = NULL;
  errno = 0;
  double val = strtod(arg, &endptr);
  if (errno == ERANGE) {
    fprintf(stderr, "%s: warning: %s: numerical result out of range\n",
            PROGRAM_NAME, arg);
  } else if (*endptr != '\0') {
    fprintf(stderr, "%s: warning: %s: value not completely converted\n",
            PROGRAM_NAME, arg);
  }
  return val;
}

static int run_printf_engine(const char *fmt, int argc, char **argv,
                             FILE *out) {
  int arg_idx = 0;
  bool stop_output = false;
  do {
    int start_idx = arg_idx;
    const char *p = fmt;
    while (*p && !stop_output) {
      if (*p == CHAR_BSLASH) {
        char esc_buf[256];
        memset(esc_buf, 0, sizeof(esc_buf));
        char *eb = esc_buf;
        *eb++ = *p++;
        if (*p == 'x' || *p == 'u' || *p == 'U') {
          *eb++ = *p++;
          while (*p && isxdigit((unsigned char)*p) &&
                 (size_t)(eb - esc_buf) < sizeof(esc_buf) - 1) {
            *eb++ = *p++;
          }
        } else if (*p >= '0' && *p <= '7') {
          int max_o = (*p == '0') ? 4 : 3;
          while (*p >= '0' && *p <= '7' && max_o-- > 0 &&
                 (size_t)(eb - esc_buf) < sizeof(esc_buf) - 1) {
            *eb++ = *p++;
          }
        } else if (*p) {
          *eb++ = *p++;
        }
        if (print_escaped_string(esc_buf, out, false)) {
          stop_output = true;
        }
        continue;
      }
      if (*p != '%') {
        fputc(*p++, out);
        continue;
      }
      p++;
      if (*p == '%') {
        fputc('%', out);
        p++;
        continue;
      }
      char directive[128];
      char *dp = directive;
      *dp++ = '%';
      while (*p && (strchr("-+ #0", *p) != NULL || *p == CHAR_SQUOTE) &&
             (size_t)(dp - directive) < sizeof(directive) - 12) {
        *dp++ = *p++;
      }
      if (*p == '\0') {
        p++;
        int width = 0;
        if (arg_idx < argc) {
          char **ap = argv + arg_idx++;
          width = (int)parse_printf_int(*ap);
        }
        dp += snprintf(dp, sizeof(directive) - (size_t)(dp - directive), "%d",
                       width);
      } else {
        while (isdigit((unsigned char)*p) &&
               (size_t)(dp - directive) < sizeof(directive) - 12) {
          *dp++ = *p++;
        }
      }
      if (*p == '.') {
        *dp++ = *p++;
        if (*p == '\0') {
          p++;
          int prec = 0;
          if (arg_idx < argc) {
            char **ap = argv + arg_idx++;
            prec = (int)parse_printf_int(*ap);
          }
          dp += snprintf(dp, sizeof(directive) - (size_t)(dp - directive), "%d",
                         prec);
        } else {
          while (isdigit((unsigned char)*p) &&
                 (size_t)(dp - directive) < sizeof(directive) - 12) {
            *dp++ = *p++;
          }
        }
      }
      while (*p && strchr("hljztL", *p) != NULL) {
        p++;
      }
      char spec = *p;
      if (!spec) {
        fprintf(stderr, "%s: %s: missing conversion character\n", PROGRAM_NAME,
                fmt);
        return EXIT_FAILURE;
      }
      p++;
      const char *curr_arg = "";
      if (arg_idx < argc) {
        char **ap = argv + arg_idx++;
        curr_arg = *ap;
      }
      switch (spec) {
        case 'b':
          if (print_escaped_string(curr_arg, out, true)) {
            stop_output = true;
          }
          break;
        case 'q':
          print_shell_quoted(curr_arg, out);
          break;
        case 's':
          *dp++ = 's';
          *dp = '\0';
          fprintf(out, directive, curr_arg);
          break;
        case 'c':
          *dp++ = 'c';
          *dp = '\0';
          fprintf(out, directive, *curr_arg ? *curr_arg : '\0');
          break;
        case 'd':
        case 'i': {
          intmax_t val = parse_printf_int(curr_arg);
          snprintf(dp, sizeof(directive) - (size_t)(dp - directive), "%s",
                   PRIdMAX);
          fprintf(out, directive, val);
          break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
          uintmax_t val = parse_printf_uint(curr_arg);
          const char *pri = (spec == 'u')   ? PRIuMAX
                            : (spec == 'o') ? PRIoMAX
                            : (spec == 'x') ? PRIxMAX
                                            : PRIXMAX;
          snprintf(dp, sizeof(directive) - (size_t)(dp - directive), "%s", pri);
          fprintf(out, directive, val);
          break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
          double val = parse_printf_float(curr_arg);
          *dp++ = spec;
          *dp = '\0';
          fprintf(out, directive, val);
          break;
        }
        default:
          fprintf(stderr, "%s: %%%c: invalid conversion specification\n",
                  PROGRAM_NAME, spec);
          return EXIT_FAILURE;
      }
    }
    if (stop_output || arg_idx == start_idx) {
      break;
    }
  } while (arg_idx < argc);
  return EXIT_SUCCESS;
}

static void print_json_array(int count, char **items, FILE *out) {
  fputc('[', out);
  for (int i = 0; i < count; i++) {
    if (i > 0) fputs(", ", out);
    fputc(CHAR_DQUOTE, out);
    char **ip = items + i;
    for (const char *p = *ip; *p; p++) {
      unsigned char c = (unsigned char)*p;
      switch (c) {
        case CHAR_DQUOTE:
          fputc(CHAR_BSLASH, out);
          fputc(CHAR_DQUOTE, out);
          break;
        case CHAR_BSLASH:
          fputc(CHAR_BSLASH, out);
          fputc(CHAR_BSLASH, out);
          break;
        case '\b':
          fputs("\\b", out);
          break;
        case '\f':
          fputs("\\f", out);
          break;
        case '\n':
          fputs("\\n", out);
          break;
        case '\r':
          fputs("\\r", out);
          break;
        case '\t':
          fputs("\\t", out);
          break;
        default:
          if (c < 0x20) {
            fprintf(out, "\\u00%02x", c);
          } else {
            fputc(c, out);
          }
          break;
      }
    }
    fputc(CHAR_DQUOTE, out);
  }
  fputc(']', out);
}

static void print_csv_line(int count, char **items, FILE *out) {
  for (int i = 0; i < count; i++) {
    if (i > 0) fputc(',', out);
    bool needs_quotes = false;
    char **ip = items + i;
    const char *item = *ip;
    for (const char *p = item; *p; p++) {
      if (*p == ',' || *p == CHAR_DQUOTE || *p == '\n' || *p == '\r') {
        needs_quotes = true;
        break;
      }
    }
    if (needs_quotes) {
      fputc(CHAR_DQUOTE, out);
      for (const char *p = item; *p; p++) {
        if (*p == CHAR_DQUOTE) {
          fputc(CHAR_DQUOTE, out);
          fputc(CHAR_DQUOTE, out);
        } else {
          fputc(*p, out);
        }
      }
      fputc(CHAR_DQUOTE, out);
    } else {
      fputs(item, out);
    }
  }
}

static bool is_valid_echo_flag_cluster(const char *arg) {
  if (!arg || *arg != '-') return false;
  const char *p = arg + 1;
  if (*p == '\0') return false;
  while (*p != '\0') {
    if (*p != 'n' && *p != 'e' && *p != 'E') {
      return false;
    }
    p++;
  }
  return true;
}

int main(int argc, char **argv) {
  sayq_config_t cfg = {.no_newline = false,
                       .interpret_escapes = false,
                       .posixly_correct = (getenv("POSIXLY_CORRECT") != NULL),
                       .use_stderr = false,
                       .format_mode = false,
                       .format_str = NULL,
                       .separator = " ",
                       .color_code = NULL,
                       .out_mode = OUT_STANDARD};
  int arg_start = 1;
  if (!cfg.posixly_correct && argc == 2) {
    char **arg1 = argv + 1;
    if (strcmp(*arg1, "--help") == 0) {
      print_help();
      return EXIT_SUCCESS;
    }
    if (strcmp(*arg1, "--version") == 0) {
      print_version();
      return EXIT_SUCCESS;
    }
  }
  while (arg_start < argc) {
    char **ap = argv + arg_start;
    const char *arg = *ap;
    if (strcmp(arg, "--") == 0) {
      arg_start++;
      break;
    }
    if (strcmp(arg, "--help") == 0) {
      print_help();
      return EXIT_SUCCESS;
    }
    if (strcmp(arg, "--version") == 0) {
      print_version();
      return EXIT_SUCCESS;
    }
    if (strcmp(arg, "--stderr") == 0) {
      cfg.use_stderr = true;
      arg_start++;
      continue;
    }
    if (strcmp(arg, "--json") == 0) {
      cfg.out_mode = OUT_JSON;
      arg_start++;
      continue;
    }
    if (strcmp(arg, "--csv") == 0) {
      cfg.out_mode = OUT_CSV;
      arg_start++;
      continue;
    }
    if (strcmp(arg, "--color") == 0 && arg_start + 1 < argc) {
      char **next_ap = argv + arg_start + 1;
      cfg.color_code = resolve_ansi_color(*next_ap);
      arg_start += 2;
      continue;
    }
    if ((strcmp(arg, "-s") == 0 || strcmp(arg, "--sep") == 0) &&
        arg_start + 1 < argc) {
      char **next_ap = argv + arg_start + 1;
      cfg.separator = *next_ap;
      arg_start += 2;
      continue;
    }
    if ((strcmp(arg, "-f") == 0 || strcmp(arg, "--format") == 0) &&
        arg_start + 1 < argc) {
      char **next_ap = argv + arg_start + 1;
      cfg.format_mode = true;
      cfg.format_str = *next_ap;
      arg_start += 2;
      break;
    }
    if (is_valid_echo_flag_cluster(arg)) {
      if (cfg.posixly_correct && strcmp(arg, "-n") != 0) {
        break;
      }
      const char *p = arg + 1;
      while (*p != '\0') {
        if (*p == 'n')
          cfg.no_newline = true;
        else if (*p == 'e')
          cfg.interpret_escapes = true;
        else if (*p == 'E')
          cfg.interpret_escapes = false;
        p++;
      }
      arg_start++;
      continue;
    }
    break;
  }
  FILE *out = cfg.use_stderr ? stderr : stdout;
  bool apply_color = (cfg.color_code != NULL && isatty(fileno(out)));
  if (apply_color) {
    fputs(cfg.color_code, out);
  }
  int exit_status = EXIT_SUCCESS;
  if (cfg.format_mode) {
    exit_status = run_printf_engine(cfg.format_str, argc - arg_start,
                                    argv + arg_start, out);
  } else if (cfg.out_mode == OUT_JSON) {
    print_json_array(argc - arg_start, argv + arg_start, out);
    if (!cfg.no_newline) fputc('\n', out);
  } else if (cfg.out_mode == OUT_CSV) {
    print_csv_line(argc - arg_start, argv + arg_start, out);
    if (!cfg.no_newline) fputc('\n', out);
  } else {
    bool stopped = false;
    for (int i = arg_start; i < argc; i++) {
      if (i > arg_start) {
        fputs(cfg.separator, out);
      }
      char **ip = argv + i;
      if (cfg.interpret_escapes) {
        if (print_escaped_string(*ip, out, true)) {
          stopped = true;
          break;
        }
      } else {
        fputs(*ip, out);
      }
    }
    if (!stopped && !cfg.no_newline) {
      fputc('\n', out);
    }
  }
  if (apply_color) {
    fputs("\033[0m", out);
  }
  if (fflush(out) != 0) {
    perror(PROGRAM_NAME);
    return EXIT_FAILURE;
  }
  return exit_status;
}
