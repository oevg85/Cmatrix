/*
    cmatrix.c — Windows-порт для Dev-C++

    Оригинал: Copyright (C) 1999-2017 Chris Allegretta
              Copyright (C) 2017-Present Abishek V Ashok
    Лицензия: GNU GPL v3 или новее.

    Отличия от оригинала:
    - ncurses заменён на Win32 Console API (двойной буфер CHAR_INFO,
      цвет — через атрибуты консоли, ввод — через conio.h);
    - сигналы SIGWINCH/SIGTSTP заменены проверкой размера окна и
      обработчиком Ctrl+C (SetConsoleCtrlHandler);
    - getopt заменён на ручной разбор аргументов;
    - режимы -l/-f/-x (linux-консоль, mtx.pcf) оставлены как флаги,
      но на Windows не дают визуального эффекта.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <locale.h>
#include <stdbool.h>
#include <windows.h>
#include <conio.h>

#ifndef VERSION
#define VERSION "2.0-win"
#endif

/* --- Цвета в терминах Win32-консоли (вместо пар curses) --- */
#define COLOR_BLACK   0
#define COLOR_BLUE    FOREGROUND_BLUE
#define COLOR_GREEN   FOREGROUND_GREEN
#define COLOR_RED     FOREGROUND_RED
#define COLOR_CYAN    (FOREGROUND_GREEN | FOREGROUND_BLUE)
#define COLOR_MAGENTA (FOREGROUND_RED  | FOREGROUND_BLUE)
#define COLOR_YELLOW  (FOREGROUND_RED  | FOREGROUND_GREEN)
#define COLOR_WHITE   (FOREGROUND_RED  | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define COLOR_PAIR(c) (c)
#define A_BOLD        FOREGROUND_INTENSITY
#define A_ALTCHARSET  0x00010000  /* игнорируется: attron/attroff его пропускают */

/* Matrix typedef */
typedef struct cmatrix {
    int val;
    bool is_head;
} cmatrix;

/* Global variables */
static int LINES = 24, COLS = 80;
static int console = 0;
static int xwindow = 0;
static int lock = 0;
static cmatrix **matrix = NULL;
static int *length = NULL;   /* Length of cols in each line */
static int *spaces = NULL;   /* Spaces left to fill */
static int *updates = NULL;  /* What does this do again? */

/* --- Экран --- */
static HANDLE hOut = NULL;
static CHAR_INFO *screen = NULL;
static int cur_x = 0, cur_y = 0;
static int cur_color = COLOR_WHITE;
static int cur_bold = 0;

/* Прототипы */
void *nmalloc(size_t howmuch);
void var_init(void);
void usage(void);
void version(void);
int color_from_name(const char *name);
static void get_size(void);
static void clear_screen(void);
static void flush_screen(void);
static void put(int x, int y, int ch, int attr);
static void move(int y, int x);
static void addch(int ch);
static void addstr(const char *s);
static void attron(int a);
static void attroff(int a);
static void finish(void);
static void check_resize(void);
static BOOL WINAPI ctrl_handler(DWORD type);

/* Что делаем при выходе */
void finish(void) {
    CONSOLE_CURSOR_INFO ci;
    ci.dwSize = 20;
    ci.bVisible = TRUE;
    SetConsoleCursorInfo(hOut, &ci);
    SetConsoleTextAttribute(hOut,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    clear_screen();
    exit(0);
}

void c_die(char *msg, ...) {
    va_list ap;
    CONSOLE_CURSOR_INFO ci;
    ci.dwSize = 20;
    ci.bVisible = TRUE;
    SetConsoleCursorInfo(hOut, &ci);
    SetConsoleTextAttribute(hOut,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);
    exit(0);
}

/* Ctrl+C / закрытие консоли; в режиме блокировки игнорируем */
static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        if (lock)
            return TRUE;
        finish();
    }
    return FALSE;
}

static void get_size(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        COLS  = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        LINES = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
    if (LINES < 10) LINES = 10;
    if (COLS < 10) COLS = 10;
}

static void init_console(void) {
    CONSOLE_CURSOR_INFO ci;

    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == NULL || hOut == INVALID_HANDLE_VALUE)
        c_die("CMatrix: cannot get console handle\n");

    get_size();

    ci.dwSize = 1;
    ci.bVisible = FALSE;
    SetConsoleCursorInfo(hOut, &ci);

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
}

static void clear_screen(void) {
    COORD pos;
    DWORD n, written;
    if (!screen)
        return;
    pos.X = 0;
    pos.Y = 0;
    n = (DWORD)COLS * LINES;
    FillConsoleOutputCharacterW(hOut, L' ', n, pos, &written);
    FillConsoleOutputAttribute(hOut, 0, n, pos, &written);
}

static void flush_screen(void) {
    COORD size, origin;
    SMALL_RECT rect;
    if (!screen)
        return;
    size.X = (SHORT)COLS;
    size.Y = (SHORT)LINES;
    origin.X = 0;
    origin.Y = 0;
    rect.Left = 0;
    rect.Top = 0;
    rect.Right = (SHORT)(COLS - 1);
    rect.Bottom = (SHORT)(LINES - 1);
    WriteConsoleOutputW(hOut, screen, size, origin, &rect);
}

/* Буферизованный вывод: вместо прямого addch в curses */
static void put(int x, int y, int ch, int attr) {
    if (x < 0 || x >= COLS || y < 0 || y >= LINES)
        return;
    screen[y * COLS + x].Char.UnicodeChar = (WCHAR)ch;
    screen[y * COLS + x].Attributes = (WORD)(attr | FOREGROUND_INTENSITY * 0);
}

static void move(int y, int x) {
    cur_y = y;
    cur_x = x;
}

static void addch(int ch) {
    int attr = cur_color | (cur_bold ? FOREGROUND_INTENSITY : 0);
    put(cur_x, cur_y, ch, attr);
    cur_x++;
}

static void addstr(const char *s) {
    while (*s)
        addch(*s++);
}

static void attron(int a) {
    if (a & A_ALTCHARSET)
        return;
    if (a & FOREGROUND_INTENSITY)
        cur_bold = 1;
    else
        cur_color = a;
}

static void attroff(int a) {
    if (a & A_ALTCHARSET)
        return;
    if (a & FOREGROUND_INTENSITY)
        cur_bold = 0;
    /* цвет сбрасывать не нужно: следующее рисование всегда
       начинается с attron(COLOR_PAIR(...)), как в оригинале */
}

void usage(void) {
    printf(" Usage: cmatrix -[abBchlLnorsmxV] [-u delay] [-C color]\n");
    printf(" -a: Asynchronous scroll\n");
    printf(" -b: Bold characters on\n");
    printf(" -B: All bold characters (overrides -b)\n");
    printf(" -c: Classic mode (Unicode CJK characters). Needs a font with CJK support\n");
    printf(" -f: (ignored on Windows)\n");
    printf(" -l: Console mode flag (kept for compatibility, no visual effect)\n");
    printf(" -L: Lock mode (Ctrl+C is ignored; kill via Task Manager)\n");
    printf(" -o: Use old-style scrolling\n");
    printf(" -h: Print usage and exit\n");
    printf(" -n: No bold characters (overrides -b and -B, default)\n");
    printf(" -s: \"Screensaver\" mode, exits on first keystroke\n");
    printf(" -x: X window mode flag (kept for compatibility, no visual effect)\n");
    printf(" -V: Print version information and exit\n");
    printf(" -u delay (0 - 10, default 4): Screen update delay\n");
    printf(" -C [color]: Use this color for matrix (default green)\n");
    printf(" -r: rainbow mode\n");
    printf(" -m: lambda mode\n");
}

void version(void) {
    printf(" CMatrix version %s (compiled %s, %s)\n",
        VERSION, __TIME__, __DATE__);
    printf("Original: https://github.com/abishekvashok/cmatrix\n");
}

int color_from_name(const char *name) {
    if (!_stricmp(name, "green"))   return COLOR_GREEN;
    if (!_stricmp(name, "red"))     return COLOR_RED;
    if (!_stricmp(name, "blue"))    return COLOR_BLUE;
    if (!_stricmp(name, "white"))   return COLOR_WHITE;
    if (!_stricmp(name, "yellow"))  return COLOR_YELLOW;
    if (!_stricmp(name, "cyan"))    return COLOR_CYAN;
    if (!_stricmp(name, "magenta")) return COLOR_MAGENTA;
    if (!_stricmp(name, "black"))   return COLOR_BLACK;
    return -1;
}

/* nmalloc from nano by Big Gaute */
void *nmalloc(size_t howmuch) {
    void *r;

    if (!(r = malloc(howmuch))) {
        c_die("CMatrix: malloc: out of memory!");
    }

    return r;
}

/* Initialize the global variables */
void var_init(void) {
    int i, j;

    if (matrix != NULL) {
        free(matrix[0]);
        free(matrix);
    }

    matrix = nmalloc(sizeof(cmatrix *) * (LINES + 1));
    matrix[0] = nmalloc(sizeof(cmatrix) * (LINES + 1) * COLS);
    for (i = 1; i <= LINES; i++) {
        matrix[i] = matrix[i - 1] + COLS;
    }

    if (length != NULL)
        free(length);
    length = nmalloc(COLS * sizeof(int));

    if (spaces != NULL)
        free(spaces);
    spaces = nmalloc(COLS * sizeof(int));

    if (updates != NULL)
        free(updates);
    updates = nmalloc(COLS * sizeof(int));

    /* Make the matrix */
    for (i = 0; i <= LINES; i++) {
        for (j = 0; j <= COLS - 1; j += 2) {
            matrix[i][j].val = -1;
            matrix[i][j].is_head = false;
        }
    }

    for (j = 0; j <= COLS - 1; j += 2) {
        /* Set up spaces[] array of how many spaces to skip */
        spaces[j] = (int) rand() % LINES + 1;

        /* And length of the stream */
        length[j] = (int) rand() % (LINES - 3) + 3;

        /* Sentinel value for creation of new objects */
        matrix[1][j].val = ' ';

        /* And set updates[] array for update speed */
        updates[j] = (int) rand() % 3 + 1;
    }
}

/* Реакция на изменение размера окна (аналог SIGWINCH) */
static void check_resize(void) {
    int newL = LINES, newC = COLS;
    int k;
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        newC = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        newL = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
    if (newL < 10) newL = 10;
    if (newC < 10) newC = 10;

    /* Выделяем буфер при первом вызове и при каждом реальном изменении */
    if (screen == NULL || newL != LINES || newC != COLS) {
        LINES = newL;
        COLS = newC;
        if (screen)
            free(screen);
        screen = nmalloc(sizeof(CHAR_INFO) * COLS * LINES);
        for (k = 0; k < LINES * COLS; k++) {
            screen[k].Char.UnicodeChar = L' ';
            screen[k].Attributes = 0;
        }
        clear_screen();
        var_init();
    }
}

int main(int argc, char *argv[]) {
    int i, y, z, keypress;
    int j = 0;
    int count = 0;
    int screensaver = 0;
    int asynch = 0;
    int bold = 0;
    int firstcoldone = 0;
    int oldstyle = 0;
    int random = 0;
    int update = 4;
    int mcolor = COLOR_GREEN;
    int rainbow = 0;
    int lambda = 0;
    int randnum = 0;
    int highnum = 0;
    int randmin = 0;
    int paused = 0;
    int classic = 0;
    int cval;

    srand((unsigned) time(NULL));
    setlocale(LC_ALL, "");

    init_console();

    /* Ручной разбор аргументов: getopt в MinGW нет */
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];
        char *p;

        if (arg[0] != '-' || arg[1] == '\0')
            continue;

        for (p = arg + 1; *p != '\0'; p++) {
            switch (*p) {
            case 's':
                screensaver = 1;
                break;
            case 'a':
                asynch = 1;
                break;
            case 'b':
                if (bold != 2)
                    bold = 1;
                break;
            case 'B':
                bold = 2;
                break;
            case 'C':
            case 'u': {
                const char *val;
                if (p[1] != '\0') {
                    val = p + 1;
                    p += strlen(p) - 1; /* дошли до конца этой опции */
                } else if (i + 1 < argc) {
                    val = argv[++i];
                } else {
                    usage();
                    finish();
                }
                if (*p == 'C') {
                    cval = color_from_name(val);
                    if (cval < 0) {
                        c_die(" Invalid color selection\n Valid "
                              "colors are green, red, blue, "
                              "white, yellow, cyan, magenta and black.\n");
                    }
                    mcolor = cval;
                } else {
                    update = atoi(val);
                }
                break;
            }
            case 'c':
                classic = 1;
                break;
            case 'f':
                /* на Windows не имеет смысла */
                break;
            case 'l':
                console = 1;
                break;
            case 'L':
                lock = 1;
                break;
            case 'n':
                bold = -1;
                break;
            case 'h':
            case '?':
                usage();
                finish();
            case 'o':
                oldstyle = 1;
                break;
            case 'x':
                xwindow = 1;
                break;
            case 'V':
                version();
                finish();
            case 'r':
                rainbow = 1;
                break;
            case 'm':
                lambda = 1;
                break;
            default:
                usage();
                finish();
            }
        }
    }

    /* Цвета в консоли Windows есть всегда — задаём атрибут по умолчанию */
    SetConsoleTextAttribute(hOut,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    /* Set up values for random number generation */
    if (classic) {
        /* Unicode-символы в стиле оригинального cmatrix (CJK) */
        randmin = 12288;
        highnum = 12351;
    } else if (console || xwindow) {
        randmin = 166;
        highnum = 217;
    } else {
        randmin = 33;
        highnum = 123;
    }
    randnum = highnum - randmin;

    check_resize();
    var_init();
    clear_screen();

    while (1) {
        count++;
        if (count > 4) {
            count = 1;
        }

        /* Реакция на изменение размера окна */
        check_resize();

        /* Ввод: неблокирующее чтение клавиши */
        keypress = -1;
        if (_kbhit()) {
            keypress = _getch();
            if (keypress == 0 || keypress == 224) {
                /* расширенный код (стрелки, F-клавиши): съедаем второй байт */
                if (_kbhit())
                    _getch();
                keypress = -1;
            }
        }

        if (keypress != -1) {
            if (screensaver == 1) {
                finish();
            } else {
                switch (keypress) {
                case 'q':
                    if (lock != 1)
                        finish();
                    break;
                case 'a':
                    asynch = 1 - asynch;
                    break;
                case 'b':
                    bold = 1;
                    break;
                case 'B':
                    bold = 2;
                    break;
                case 'L':
                    lock = 1;
                    break;
                case 'n':
                    bold = 0;
                    break;
                case '0': /* Fall through */
                case '1': /* Fall through */
                case '2': /* Fall through */
                case '3': /* Fall through */
                case '4': /* Fall through */
                case '5': /* Fall through */
                case '6': /* Fall through */
                case '7': /* Fall through */
                case '8': /* Fall through */
                case '9':
                    update = keypress - 48;
                    break;
                case '!':
                    mcolor = COLOR_RED;
                    rainbow = 0;
                    break;
                case '@':
                    mcolor = COLOR_GREEN;
                    rainbow = 0;
                    break;
                case '#':
                    mcolor = COLOR_YELLOW;
                    rainbow = 0;
                    break;
                case '$':
                    mcolor = COLOR_BLUE;
                    rainbow = 0;
                    break;
                case '%':
                    mcolor = COLOR_MAGENTA;
                    rainbow = 0;
                    break;
                case 'r':
                    rainbow = 1;
                    break;
                case 'm':
                    lambda = !lambda;
                    break;
                case '^':
                    mcolor = COLOR_CYAN;
                    rainbow = 0;
                    break;
                case '&':
                    mcolor = COLOR_WHITE;
                    rainbow = 0;
                    break;
                case 'p':
                case 'P':
                    paused = (paused == 0) ? 1 : 0;
                    break;
                }
            }
        }

        for (j = 0; j <= COLS - 1; j += 2) {
            if ((count > updates[j] || asynch == 0) && paused == 0) {

                /* I dont like old-style scrolling, yuck */
                if (oldstyle) {
                    for (i = LINES - 1; i >= 1; i--) {
                        matrix[i][j].val = matrix[i - 1][j].val;
                    }
                    random = (int) rand() % (randnum + 8) + randmin;

                    if (matrix[1][j].val == 0) {
                        matrix[0][j].val = 1;
                    } else if (matrix[1][j].val == ' '
                             || matrix[1][j].val == -1) {
                        if (spaces[j] > 0) {
                            matrix[0][j].val = ' ';
                            spaces[j]--;
                        } else {
                            if (((int) rand() % 3) == 1) {
                                matrix[0][j].val = 0;
                            } else {
                                matrix[0][j].val = (int) rand() % randnum + randmin;
                            }
                            spaces[j] = (int) rand() % LINES + 1;
                        }
                    } else if (random > highnum && matrix[1][j].val != 1) {
                        matrix[0][j].val = ' ';
                    } else {
                        matrix[0][j].val = (int) rand() % randnum + randmin;
                    }

                } else { /* New style scrolling (default) */
                    if (matrix[0][j].val == -1 && matrix[1][j].val == ' '
                        && spaces[j] > 0) {
                        matrix[0][j].val = -1;
                        spaces[j]--;
                    } else if (matrix[0][j].val == -1
                        && matrix[1][j].val == ' ') {
                        length[j] = (int) rand() % (LINES - 3) + 3;
                        matrix[0][j].val = (int) rand() % randnum + randmin;

                        spaces[j] = (int) rand() % LINES + 1;
                    }
                    i = 0;
                    y = 0;
                    firstcoldone = 0;
                    while (i <= LINES) {

                        /* Skip over spaces */
                        while (i <= LINES && (matrix[i][j].val == ' ' ||
                               matrix[i][j].val == -1)) {
                            i++;
                        }

                        if (i > LINES) {
                            break;
                        }

                        /* Go to the head of this column */
                        z = i;
                        y = 0;
                        while (i <= LINES && (matrix[i][j].val != ' ' &&
                               matrix[i][j].val != -1)) {
                            matrix[i][j].is_head = false;
                            i++;
                            y++;
                        }

                        if (i > LINES) {
                            matrix[z][j].val = ' ';
                            continue;
                        }

                        matrix[i][j].val = (int) rand() % randnum + randmin;
                        matrix[i][j].is_head = true;

                        if (y > length[j] || firstcoldone) {
                            matrix[z][j].val = ' ';
                            matrix[0][j].val = -1;
                        }
                        firstcoldone = 1;
                        i++;
                    }
                }
            }

            /* A simple hack */
            if (!oldstyle) {
                y = 1;
                z = LINES;
            } else {
                y = 0;
                z = LINES - 1;
            }
            for (i = y; i <= z; i++) {
                move(i - y, j);

                if (matrix[i][j].is_head && !rainbow) {
                    attron(COLOR_PAIR(COLOR_WHITE));
                    if (bold)
                        attron(A_BOLD);
                    if (matrix[i][j].val == 0) {
                        if (console || xwindow) {
                            addch(183);
                        } else {
                            addch('&');
                        }
                    } else {
                        addch(matrix[i][j].val);
                    }
                    if (bold)
                        attroff(A_BOLD);
                } else {
                    if (rainbow) {
                        int randomColor = rand() % 6;

                        switch (randomColor) {
                            case 0: mcolor = COLOR_GREEN;   break;
                            case 1: mcolor = COLOR_BLUE;    break;
                            case 2: mcolor = COLOR_BLACK;   break;
                            case 3: mcolor = COLOR_YELLOW;  break;
                            case 4: mcolor = COLOR_CYAN;    break;
                            case 5: mcolor = COLOR_MAGENTA; break;
                        }
                    }
                    attron(COLOR_PAIR(mcolor));
                    if (matrix[i][j].val == 1) {
                        if (bold) {
                            attron(A_BOLD);
                        }
                        addch('|');
                        if (bold) {
                            attroff(A_BOLD);
                        }
                    } else {
                        if (bold == 2 ||
                            (bold == 1 && matrix[i][j].val % 2 == 0)) {
                            attron(A_BOLD);
                        }
                        if (matrix[i][j].val == -1) {
                            addch(' ');
                        } else if (lambda && matrix[i][j].val != ' ') {
                            addch(0x03BB); /* λ как Unicode-символ */
                        } else {
                            addch(matrix[i][j].val);
                        }
                        if (bold == 2 ||
                            (bold == 1 && matrix[i][j].val % 2 == 0)) {
                            attroff(A_BOLD);
                        }
                    }
                }
            }
        }

        /* Check if computer is locked */
        if (lock == 1) {
            char *msg = "Computer locked.";
            int msg_x = LINES / 2;
            int msg_y = COLS / 2 - (int) strlen(msg) / 2;
            int k;

            cur_color = COLOR_WHITE;
            cur_bold = 0;

            /* Add space before message */
            move(msg_x - 1, msg_y - 2);
            for (k = 0; k < (int) strlen(msg) + 4; k++)
                addch(' ');

            /* Write message */
            move(msg_x, msg_y - 2);
            addch(' ');
            addch(' ');
            addstr(msg);
            addch(' ');
            addch(' ');

            /* Add space after message */
            move(msg_x + 1, msg_y - 2);
            for (k = 0; k < (int) strlen(msg) + 4; k++)
                addch(' ');
        }

        flush_screen();
        Sleep(update * 10);
    }

    finish();
    return 0;
}
