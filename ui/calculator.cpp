#include "ui.h"
#include "fb.h"

static char expression[80];
static char result_text[64];
static const char* message = "Enter an integer expression and press ENTER";
static int expression_length = 0;

static void append_char(char* out, int* pos, char c, int capacity)
{
    if (*pos < capacity - 1) out[(*pos)++] = c;
}

static void format_number(int64_t value, char* out, int capacity)
{
    char reverse[32]; int n = 0; int negative = value < 0;
    uint64_t magnitude = negative ? (uint64_t)(-(value + 1)) + 1ULL : (uint64_t)value;
    if (magnitude == 0) reverse[n++] = '0';
    while (magnitude > 0 && n < (int)sizeof(reverse))
    {
        reverse[n++] = (char)('0' + magnitude % 10ULL);
        magnitude /= 10ULL;
    }
    int pos = 0;
    if (negative) append_char(out, &pos, '-', capacity);
    while (n > 0) append_char(out, &pos, reverse[--n], capacity);
    out[pos] = '\0';
}

static int parse_position = 0;
static int parse_error = 0;
static int64_t parse_expression_value();

static void parse_skip_spaces()
{
    while (expression[parse_position] == ' ' || expression[parse_position] == '\t') ++parse_position;
}

static int64_t parse_factor()
{
    parse_skip_spaces();
    int sign = 1;
    if (expression[parse_position] == '+' || expression[parse_position] == '-')
    {
        if (expression[parse_position] == '-') sign = -1;
        ++parse_position;
        parse_skip_spaces();
    }
    if (expression[parse_position] == '(')
    {
        ++parse_position;
        int64_t value = parse_expression_value();
        parse_skip_spaces();
        if (expression[parse_position] != ')') parse_error = 1;
        else ++parse_position;
        return sign < 0 ? -value : value;
    }
    if (expression[parse_position] < '0' || expression[parse_position] > '9')
    {
        parse_error = 1;
        return 0;
    }
    int64_t value = 0;
    while (expression[parse_position] >= '0' && expression[parse_position] <= '9')
    {
        int digit = expression[parse_position] - '0';
        if (value > 100000000000LL) { parse_error = 1; return 0; }
        value = value * 10 + digit;
        ++parse_position;
    }
    return sign < 0 ? -value : value;
}

static int64_t parse_term()
{
    int64_t value = parse_factor();
    for (;;)
    {
        parse_skip_spaces();
        char op = expression[parse_position];
        if (op != '*' && op != '/') break;
        ++parse_position;
        int64_t rhs = parse_factor();
        if (op == '/')
        {
            if (rhs == 0) { parse_error = 2; return 0; }
            value /= rhs;
        }
        else
        {
            int64_t av = value < 0 ? -value : value;
            int64_t bv = rhs < 0 ? -rhs : rhs;
            if (bv != 0 && av > 1000000000000LL / bv) { parse_error = 1; return 0; }
            value *= rhs;
        }
    }
    return value;
}

static int64_t parse_expression_value()
{
    int64_t value = parse_term();
    for (;;)
    {
        parse_skip_spaces();
        char op = expression[parse_position];
        if (op != '+' && op != '-') break;
        ++parse_position;
        int64_t rhs = parse_term();
        if (op == '+')
        {
            if ((rhs > 0 && value > 1000000000000LL - rhs) ||
                (rhs < 0 && value < -1000000000000LL - rhs)) { parse_error = 1; return 0; }
            value += rhs;
        }
        else
        {
            if ((rhs < 0 && value > 1000000000000LL + rhs) ||
                (rhs > 0 && value < -1000000000000LL + rhs)) { parse_error = 1; return 0; }
            value -= rhs;
        }
    }
    return value;
}

static int evaluate_expression(int64_t* output)
{
    parse_position = 0;
    parse_error = 0;
    int64_t value = parse_expression_value();
    parse_skip_spaces();
    if (parse_error == 2) return -2;
    if (parse_error != 0 || expression[parse_position] != '\0') return -1;
    *output = value;
    return 0;
}

void ui_calculator_init()
{
    expression[0] = '\0'; result_text[0] = '\0'; expression_length = 0;
    message = "Enter integers, operators + - * /, then ENTER";
}

void ui_calculator_render(uint64_t epoch)
{
    (void)epoch;
    ui_draw_app_frame("Calculator", "Built-in integer calculator");
    int w = fb_get_width();
    fb_draw_round_rect(92, 160, w - 184, 156, 18, 0x263C54U);
    fb_draw_ui_text("EXPRESSION", 120, 182, 0x9FBAD8U, 1);
    fb_draw_ui_text(expression, 120, 220, 0xFFFFFFFFU, 2);
    fb_draw_round_rect(92, 335, w - 184, 106, 18, 0x203348U);
    fb_draw_ui_text("RESULT", 120, 354, 0x9FBAD8U, 1);
    fb_draw_ui_text(result_text[0] ? result_text : "0", 120, 386, 0x8DF0C8U, 2);

    const int cell = 88, gap = 14, start_x = (w - (4 * cell + 3 * gap)) / 2, start_y = 478;
    const char* keys[4][4] = {{"7","8","9","/"},{"4","5","6","*"},{"1","2","3","-"},{"C","0","=","+"}};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
        {
            int x = start_x + col * (cell + gap), y = start_y + row * 43;
            uint32_t color = (keys[row][col][0] == '=' || keys[row][col][0] == '+') ? 0x347B78U : 0x334962U;
            fb_draw_round_rect(x, y, cell, 36, 9, color);
            fb_draw_ui_text(keys[row][col], x + (cell - fb_ui_text_width(keys[row][col], 2)) / 2, y + 10, 0xFFFFFFU, 2);
        }
    fb_draw_ui_text(message, 104, 666, 0xB5C7DFU, 1);
    ui_draw_bottom_hint("Type expression over serial   ENTER: Calculate   BACKSPACE: Edit   Q: Back");
    fb_present();
}

int ui_calculator_key(char key)
{
    if (key == 'q' || key == 'Q') return 1;
    if (key == '\b' || (unsigned char)key == 127U)
    {
        if (expression_length > 0) expression[--expression_length] = '\0';
        return 0;
    }
    if (key == 'c' || key == 'C')
    {
        expression_length = 0; expression[0] = '\0'; result_text[0] = '\0';
        message = "Cleared"; return 0;
    }
    if (key == '\r' || key == '\n' || key == '=')
    {
        int64_t value = 0;
        int rc = evaluate_expression(&value);
        if (rc == 0) { format_number(value, result_text, sizeof(result_text)); message = "Calculation complete"; }
        else if (rc == -2) { result_text[0] = '\0'; message = "Error: division by zero"; }
        else { result_text[0] = '\0'; message = "Invalid expression. Example: 12 * 8"; }
        return 0;
    }
    if (((key >= '0' && key <= '9') || key == '+' || key == '-' || key == '*' || key == '/' || key == ' ') && expression_length < (int)sizeof(expression) - 1)
    {
        expression[expression_length++] = key; expression[expression_length] = '\0';
        result_text[0] = '\0';
    }
    return 0;
}

int ui_calculator_pointer(int x, int y)
{
    const int cell = 88, gap = 14;
    const int start_x = (fb_get_width() - (4 * cell + 3 * gap)) / 2;
    const int start_y = 478;
    const char* keys[4][4] = {{"7","8","9","/"},{"4","5","6","*"},
                              {"1","2","3","-"},{"C","0","=","+"}};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            int left = start_x + col * (cell + gap);
            int top = start_y + row * 43;
            if (x >= left && x < left + cell && y >= top && y < top + 36)
            {
                (void)ui_calculator_key(keys[row][col][0]);
                return 0;
            }
        }
    }
    return 0;
}
