/* pli_rt_state.h — shared TU state for the split libpli sources.
 *
 * Each rt_*.c includes this header (via pli_rt.h) so the cross-file users
 * keep working exactly as in the single-file layout: put_raw/next_char for
 * stream sinks/sources, the FILE routing table, STRING buffers, and the
 * data-directed input flag. One TU owns each definition (rt_stream.c);
 * every other TU sees it as extern.
 */
#ifndef PLI_RT_STATE_H
#define PLI_RT_STATE_H

#include <stddef.h>
#include <stdio.h>

/* SYSPRINT state. A full implementation tracks page/line/column against
 * LINESIZE and PAGESIZE and raises ENDPAGE; M0 tracks the column only. */
extern int rt_col;
extern int rt_items_on_line;
/* Data-directed output (rule (106), QR1.5): names emitted in the open DATA
 * list, and a flag suppressing the value's blank separator after NAME=. */
extern int rt_data_items;
extern int rt_data_value_next;
/* Set when get_token terminates at ';' (data-directed pairs, rule (106)),
 * so pli_get_data_next ends the list there. */
extern int rt_tok_semi;

/* STRING (rule 105) sink/source: when out_buf is non-null, list-directed output
 * is written into it instead of stdout; when in_buf is non-null, list-directed
 * input is read from it instead of stdin. */
extern char *rt_out_buf;
extern size_t rt_out_cap, rt_out_len;
extern const char *rt_in_buf;
extern size_t rt_in_len, rt_in_pos;

/* Named files (rules 100-103, 105): a fixed table indexed by the compile-time
 * slot assigned to each FILE variable. out_f/in_f are the streams selected by
 * the FILE ( f ) option of a PUT/GET; when set they override stdout/stdin. */
#define PLI_MAX_FILES 16
extern FILE *rt_pli_files[PLI_MAX_FILES];
extern FILE *rt_out_f;
extern FILE *rt_in_f;

/* Stream helpers shared across TUs (defined in rt_stream.c). */
void rt_put_raw(const char *p, size_t n);
int rt_next_char(void);

/* Compatibility aliases so split sources keep their original identifiers. */
#define col rt_col
#define items_on_line rt_items_on_line
#define data_items rt_data_items
#define data_value_next rt_data_value_next
#define tok_semi rt_tok_semi
#define out_buf rt_out_buf
#define out_cap rt_out_cap
#define out_len rt_out_len
#define in_buf rt_in_buf
#define in_len rt_in_len
#define in_pos rt_in_pos
#define pli_files rt_pli_files
#define out_f rt_out_f
#define in_f rt_in_f
#define put_raw rt_put_raw
#define next_char rt_next_char

#endif /* PLI_RT_STATE_H */

/* Intra-runtime helpers shared across TUs (single definitions named). */
void rt_separate(void);
void rt_display_begin(void);
void rt_display_end(void);
void rt_format_decfixed(char *buf, size_t cap, long long v, long long q);
int rt_get_token(char *buf, size_t cap);
int rt_data_namechar(int c);
void rt_pli_on_push(long long key, long long id);
long long rt_pli_on_top(long long key);
void rt_pli_on_pop(long long key);

#define separate rt_separate
#define display_begin rt_display_begin
#define display_end rt_display_end
#define format_decfixed rt_format_decfixed
#define get_token rt_get_token
#define data_namechar rt_data_namechar
#define pli_on_push rt_pli_on_push
#define pli_on_top rt_pli_on_top
#define pli_on_pop rt_pli_on_pop
