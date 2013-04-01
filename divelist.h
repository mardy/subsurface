#ifndef DIVELIST_H
#define DIVELIST_H

#ifdef __cplusplus
extern "C" {
#endif

struct dive;

extern void dive_list_update_dives(void);
extern void update_dive_list_col_visibility(void);
extern void update_dive_list_units(void);
extern void flush_divelist(struct dive *);
extern void update_cylinder_related_info(struct dive *);
extern void mark_divelist_changed(int);
extern int unsaved_changes(void);
extern void remove_autogen_trips(void);
extern void select_next_dive(void);
extern void select_prev_dive(void);
extern void show_and_select_dive(struct dive *dive);
extern double init_decompression(struct dive * dive);
extern void export_all_dives_uddf_cb();

#ifdef __cplusplus
}
#endif

#endif
