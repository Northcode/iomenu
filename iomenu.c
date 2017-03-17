#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>


#define OFFSET     5
#define CONTINUE   2  /* as opposed to EXIT_SUCCESS and EXIT_FAILURE */

#define CONTROL(char) (char ^ 0x40)
#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))


struct line {
	char *text;  /* sent as output and matched by input */
	int   match;   /* whether it matches linev's input */
};


char          input[BUFSIZ];
size_t        current = 0, matching = 0, linec = 0, offset = 0;
struct line **linev = NULL;
int           opt_lines = 0;
char         *opt_prompt = "";


void
free_linev(struct line **linev)
{
	for (; linec > 0; linec--) {
		free(linev[linec - 1]->text);
		free(linev[linec - 1]);
	}

	free(linev);
}


void
die(const char *s)
{
	/* tcsetattr(STDIN_FILENO, TCSANOW, &termio_old); */
	fprintf(stderr, "%s\n", s);
	exit(EXIT_FAILURE);
}


struct termios
set_terminal(int tty_fd)
{
	struct termios termio_old;
	struct termios termio_new;

	if (tcgetattr(tty_fd, &termio_old) < 0)
		die("Can not get terminal attributes with tcgetattr().");

	termio_new          = termio_old;
	termio_new.c_lflag &= ~(ICANON | ECHO | IGNBRK);

	tcsetattr(tty_fd, TCSANOW, &termio_new);

	return termio_old;
}


void
read_lines(void)
{
	extern struct line **linev;
	extern size_t linec, matching;

	char s[BUFSIZ];
	size_t size = 1 << 4;

	if (!(linev = malloc(sizeof(struct line *) * size)))
		die("malloc");
	linev[0] = NULL;

	/* read the file into an array of lines */
	for (; fgets(s, BUFSIZ, stdin); linec++, matching++) {
		size_t len = strlen(s);

		if (len > 0 && s[len - 1] == '\n')
			s[len - 1] = '\0';

		if (linec > size) {
			size *= 2;
			linev = realloc(linev, sizeof(struct line *) * size);

			if (linev == NULL)
				die("realloc");
		}

		if (!(linev[linec] = malloc(sizeof(struct line))))
			die("malloc");
		if (!(linev[linec]->text = malloc(len)))
			die("malloc");
		strcpy(linev[linec]->text, s);

		linev[linec]->match = 1;
	}
}


int
line_matches(struct line *line, char **tokv, size_t tokc)
{
	for (size_t i = 0; i < tokc; i++)
		if (strstr(line->text, tokv[i]) == NULL)
			return 0;

	return 1;
}


/*
 * If inc is 1, it will only check already matching lines.
 * If inc is 0, it will only check non-matching lines.
 */
void
filter_lines(int inc)
{
	char   **tokv = NULL;
	char    *s, buf[sizeof(input)];
	size_t   n = 0, tokc = 0;

	/* tokenize input from space characters, this comes from dmenu */
	strcpy(buf, input);
	for (s = strtok(buf, " "); s; s = strtok(NULL, " "), tokc++) {

		if (tokc >= n) {
			tokv = realloc(tokv, ++n * sizeof(*tokv));

			if (tokv == NULL)
				die("realloc");
		}

		tokv[tokc] = s;
	}

	/* match lines */
	matching = 0;
	for (size_t i = 0; i < linec; i++)
		/* if (!(inc ^ linev[i]->match)) */
			matching += linev[i]->match =
				line_matches(linev[i], tokv, tokc);
}


int
matching_prev(int pos)
{
	for (size_t i = pos - 1; i > 0; i--)
		if (linev[i]->match)
			return i;
	return pos;
}


int
matching_next(size_t pos)
{
	for (size_t i = pos + 1; i < linec; i++)
		if (linev[i]->match)
			return i;

	return pos;
}


void
draw_line(size_t pos, const size_t cols)
{
	fprintf(stderr, pos == current ?
		"\n\033[30;47m\033[K%s\033[m" : "\n\033[K%s",
		linev[pos]->text
	);
}


void
draw_lines(size_t count, size_t cols)
{
	size_t printed = 0;

	for (size_t i = offset; printed < count && i < linec; i++) {
		if (linev[i]->match) {
			draw_line(i, cols);
			printed++;
		}
	}

	while (printed++ < count)
		fputs("\n\033[K", stderr);
}


int
draw_column(size_t pos, size_t col, size_t cols)
{
	fputs(pos == current ? "\033[30;47m " : " ", stderr);

	for (size_t i = 0; col < cols ;) {
		int len = mblen(linev[pos]->text + i, BUFSIZ - i);

		if (len < 0) {
			i++;
			continue;
		} else if (len == 0) {
			break;
		}

		col += linev[pos]->text[i] = '\t' ? pos + 1 % 8 : 1;

		for (; len > 0; len--, i++)
			fputc(linev[pos]->text[i], stderr);
	}

	fputs(pos == current ? " \033[m" : " ", stderr);

	return col;
}


void
draw_columns(size_t cols)
{
	size_t col = 20;

	for (size_t i = offset; col < cols; i++)
		col = draw_column(i, col, cols);
}


void
draw_prompt(size_t cols)
{
	size_t limit = opt_lines ? cols : 20;

	fputc('\r', stderr);
	for (size_t i = 0; i < limit; i++)
		fputc(' ', stderr);

	fprintf(stderr, "\r\033[1m%s%s\033[m", opt_prompt, input);
}


void
draw_screen(int tty_fd)
{
	struct winsize w;
	size_t count;

	if (ioctl(tty_fd, TIOCGWINSZ, &w) < 0)
		die("could not get terminal size");

	count = MIN(opt_lines, w.ws_row - 2);

	if (opt_lines) {
		draw_lines(count, w.ws_col);
		fprintf(stderr, "\033[%ldA", count);
	} else {
		draw_columns(w.ws_col);
	}

	draw_prompt(w.ws_col);
}


void
draw_clear(size_t lines)
{
	for (size_t i = 0; i < lines + 1; i++)
		fputs("\r\033[K\n", stderr);
	fprintf(stderr, "\033[%ldA", lines + 1);
}


void
remove_word_input()
{
	size_t len = strlen(input) - 1;

	for (int i = len; i >= 0 && isspace(input[i]); i--)
		input[i] = '\0';

	len = strlen(input) - 1;
	for (int i = len; i >= 0 && !isspace(input[i]); i--)
		input[i] = '\0';
}


void
add_character(char key)
{
	size_t len = strlen(input);

	if (isprint(key)) {
		input[len]     = key;
		input[len + 1] = '\0';
	}

	filter_lines(1);

	current = linev[0]->match ? 0 : matching_next(0);
}


/*
 * Send the selection to stdout.
 */
void
print_selection(int return_input)
{
	fputs("\r\033[K", stderr);

	if (return_input || !matching) {
		puts(input);

	} else if (matching > 0) {
		puts(linev[current]->text);
	}
}


/*
 * Perform action associated with key
 */
int
input_key(FILE *tty_fp)
{
	extern char input[];

	char key = fgetc(tty_fp);

	if (key == '\n') {
		print_selection(0);
		return EXIT_SUCCESS;
	}

	switch (key) {

	case CONTROL('C'):
		draw_clear(opt_lines);
		return EXIT_FAILURE;

	case CONTROL('U'):
		input[0] = '\0';
		current = 0;
		filter_lines(0);
		break;

	case CONTROL('W'):
		remove_word_input();
		filter_lines(0);
		break;

	case 127:
	case CONTROL('H'):  /* backspace */
		input[strlen(input) - 1] = '\0';
		filter_lines(0);
		current = linev[0]->match ? 0 : matching_next(0);
		break;

	case CONTROL('N'):
		current = matching_next(current);
		break;

	case CONTROL('P'):
		current = matching_prev(current);
		break;

	case CONTROL('I'):  /* tab */
		strcpy(input, linev[current]->text);
		filter_lines(1);
		break;

	case CONTROL('J'):
	case CONTROL('M'):  /* enter */
		print_selection(0);
		return EXIT_SUCCESS;

	default:
		add_character(key);
	}

	return CONTINUE;
}


/*
 * Listen for the user input and call the appropriate functions.
 */
int
input_get(int tty_fd)
{
	FILE *tty_fp = fopen("/dev/tty", "r");
	int   exit_code;
	struct termios termio_old = set_terminal(tty_fd);

	input[0] = '\0';

	while ((exit_code = input_key(tty_fp)) == CONTINUE)
		draw_screen(tty_fd);

	/* resets the terminal to the previous state. */
	tcsetattr(tty_fd, TCSANOW, &termio_old);

	fclose(tty_fp);

	return exit_code;
}


void
usage(void)
{
	fputs("usage: iomenu [-n] [-p prompt] [-l lines]\n", stderr);

	exit(EXIT_FAILURE);
}


int
main(int argc, char *argv[])
{
	int i, exit_code, tty_fd = open("/dev/tty", O_RDWR);

	/* command line arguments */
	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-' || strlen(argv[i]) != 2)
			usage();

		switch (argv[i][1]) {
		case 'l':
			if (sscanf(argv[++i], "%d", &opt_lines) <= 0)
				die("wrong number format after -l");
			break;
		case 'p':
			if (++i >= argc)
				die("missing string after -p");
			opt_prompt = argv[i];
			break;
		default:
			usage();
		}
	}

	/* command line arguments */
	read_lines();

	/* set the interface */
	draw_screen(tty_fd);

	/* listen and interact to input */
	exit_code = input_get(tty_fd);

	draw_clear(opt_lines);

	/* close files descriptors and pointers, and free memory */
	close(tty_fd);
	free_linev(linev);

	return exit_code;
}
