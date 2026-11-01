#!/usr/bin/env python3
"""Profiling LVGL on the board (console command "profile", docs/PROTOCOL.md §2). Run from the repository root:

    python tools/harness/lvgl_profile_patch.py apply     # tag each draw task type in managed_components (local only)
    python tools/harness/lvgl_profile_patch.py revert

managed_components is not in git: the patch is a local experiment. Also set, in <build_dir>/sdkconfig only (forge.json build_dir):
    CONFIG_LV_USE_PROFILER=y, CONFIG_LV_USE_PROFILER_BUILTIN=y,
    CONFIG_LV_PROFILER_INCLUDE="src/misc/lv_profiler_builtin.h"
then build, flash, send "profile" to the test console (tools/devloop/devloop.py send profile) and run
tools/harness/profile.py (it reads .devloop/serial_live.txt).
Revert both afterwards: the profiler slows rendering.
"""
import os
import sys

P = os.path.join(os.path.dirname(__file__), '..', '..', 'managed_components', 'lvgl__lvgl', 'src', 'draw', 'sw',
                 'lv_draw_sw.c')
A = '''    lv_draw_task_t * t = u->task_act;
    switch(t->type) {'''
B = '''    lv_draw_task_t * t = u->task_act;
    static const char * const tnames[] = {"t_none", "t_fill", "t_border", "t_box_shadow", "t_label", "t_image",
                                          "t_layer", "t_line", "t_arc", "t_triangle", "t_mask_rect", "t_mask_bitmap", "t_vector"};
    const char * tn = (unsigned)t->type < sizeof(tnames) / sizeof(tnames[0]) ? tnames[t->type] : "t_other";
    LV_PROFILER_BEGIN_TAG(tn);
    switch(t->type) {'''
A2 = '''        default:
            break;
    }

#if LV_USE_PARALLEL_DRAW_DEBUG'''
B2 = '''        default:
            break;
    }
    LV_PROFILER_END_TAG(tn);

#if LV_USE_PARALLEL_DRAW_DEBUG'''

s = open(P, encoding='utf-8', newline='').read()
if sys.argv[1:] == ['apply'] and 'tnames' not in s:
    s = s.replace(A, B).replace(A2, B2)
elif sys.argv[1:] == ['revert'] and 'tnames' in s:
    s = s.replace(B, A).replace(B2, A2)
open(P, 'w', encoding='utf-8', newline='').write(s)
print('patched' if 'tnames' in s else 'clean')
