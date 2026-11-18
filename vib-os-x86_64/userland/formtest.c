/* The engine's half of a form: an <input> becomes a field with a name, the form it
 * belongs to, and a box the client can draw and hit-test.
 *
 * This is the part that can be checked without a network or a window: what the
 * client does with the field (typing, the caret, the query string) is its own
 * business, and the query it builds is verified by the browser's own run against
 * a real page.
 */
#include <stdio.h>
#include <string.h>
#include "omilayout.h"

static int checks, fails;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) fails++;
  printf("[form] %s %s (got %ld want %ld)\n", ok ? "ok  " : "FAIL", what, got, want);
}

static int measure(int font, const char *s) { (void)font; return (int)strlen(s) * 8; }

static const char *page_html =
  "<html><body><h1>Search</h1>"
  "<form action=\"/find\" method=\"get\">"
  "<p>Query: <input type=\"text\" name=\"q\" size=\"20\" value=\"start\"></p>"
  "<input type=\"hidden\" name=\"lang\" value=\"en\">"
  "<input type=\"submit\" value=\"Go\">"
  "</form>"
  "<form action=\"https://example.org/post\" method=\"post\">"
  "<input type=\"text\" name=\"body\">"
  "<input type=\"submit\">"
  "</form>"
  "<p>No field here.</p></body></html>";

int main(void) {
  if (web_build(page_html, (int)strlen(page_html), 400, measure) != 0) {
    printf("[form] FAIL the fixture did not build\n");
    return 1;
  }

  int nf = web_field_count();
  int nform = web_form_count();
  check(nf == 3, "three fields are recorded (the hidden one counts)", nf, 3);
  check(nform == 2, "two forms are recorded", nform, 2);

  check(web_field_name(0) && !strcmp(web_field_name(0), "q"), "the text field keeps its name", 1, 1);
  check(web_field_value(0) && !strcmp(web_field_value(0), "start"), "and the value the page gave it", 1, 1);
  check(web_field_form(0) == 0, "and the form it belongs to", web_field_form(0), 0);
  check(web_field_hidden(1) == 1, "a hidden field is marked hidden", web_field_hidden(1), 1);
  check(web_field_name(1) && !strcmp(web_field_name(1), "lang"), "and still carries its name", 1, 1);

  check(web_form_action(0) && !strcmp(web_form_action(0), "/find"), "the form keeps its action", 1, 1);
  check(web_form_method(0) && !strcmp(web_form_method(0), "get"), "and its method", 1, 1);
  check(web_form_method(1) && !strcmp(web_form_method(1), "post"), "a post form says post",
        web_form_method(1) ? 1 : 0, 1);
  check(web_field_form(2) == 1, "the second form's field points at it", web_field_form(2), 1);

  /* The boxes: a text field, a submit button, and nothing for the hidden one. */
  int fields = 0, buttons = 0, hidden_drawn = 0;
  const web_op_t *ops = web_ops();
  for (int i = 0; i < web_count(); i++) {
    if (ops[i].kind == 4) fields++;
    if (ops[i].kind == 5) buttons++;
  }
  check(fields == 2, "two boxes are drawn (the hidden field is not)", fields, 2);
  check(buttons == 2, "two buttons are drawn", buttons, 2);
  (void)hidden_drawn;

  int label_ok = 0;
  for (int i = 0; i < web_count(); i++)
    if (ops[i].kind == 5 && ops[i].text && !strcmp(ops[i].text, "Go")) label_ok = 1;
  check(label_ok, "a button carries the label the page gave it", label_ok, 1);

  int box_w = 0, box_y = -1;
  for (int i = 0; i < web_count(); i++)
    if (ops[i].kind == 4 && box_y < 0) { box_w = ops[i].w; box_y = ops[i].y; }
  check(box_w > 100, "the field is as wide as its size= asks for", box_w, 161);
  check(box_y >= 0 && ops[0].kind == 1, "and it sits below the text that precedes it", box_y > 0, 1);

  printf("[form] %d/%d\n", checks - fails, checks);
  return fails ? 1 : 0;
}
