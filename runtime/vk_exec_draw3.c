// The draw translator of the second helper thread (vk_spec.h): vk_exec_draw.c compiled once more, so that every table,
// memo and per-command-buffer resource of the translator exists once per thread and no thread waits for another.
// The differences are in vk_exec_draw.c under VK_EXEC_DRAW_HELPER: the exported names (vk_exec_draw3_*), and the
// points where this copy gives its part up instead of changing what the replay thread sees.
#define VK_EXEC_DRAW_HELPER 3
#include "vk_exec_draw.c"
